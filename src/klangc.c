/*
 * klangc — Klang compiler, v0.2 (Phase 1).
 * Compiles the subset described in docs/LANGUAGE_SPEC.md to C.
 *
 * Pipeline: lex -> parse -> monomorphize+typecheck (worklist) -> emit C.
 *
 * Generics are real: each instantiation of a generic fn/struct/enum gets its own
 * cloned, fully-concrete AST and its own generated C type/function.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>

/* ───────────────────────── generic dynamic array ───────────────────────── */

typedef struct {
    void *data;
    int count;
    int cap;
    int elem_size;
} Vec;

static void vec_init(Vec *v, int elem_size) {
    v->data = NULL; v->count = 0; v->cap = 0; v->elem_size = elem_size;
}
static void *vec_push(Vec *v) {
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 4;
        v->data = realloc(v->data, (size_t)v->cap * v->elem_size);
    }
    void *slot = (char *)v->data + (size_t)v->count * v->elem_size;
    v->count++;
    return slot;
}
static void *vec_get(const Vec *v, int i) { return (char *)v->data + (size_t)i * v->elem_size; }

#define VEC_PTR(v, i, T) (*(T **)vec_get((v), (i)))
#define VEC_PUSH_PTR(v, p) (*(void **)vec_push(v) = (void *)(p))

/* ───────────────────────── string builder ───────────────────────── */

typedef struct { char *data; int len; int cap; } SB;
static void sb_init(SB *sb) { sb->data = malloc(64); sb->data[0] = 0; sb->len = 0; sb->cap = 64; }
static void sb_append(SB *sb, const char *s) {
    int n = (int)strlen(s);
    if (sb->len + n + 1 > sb->cap) {
        while (sb->len + n + 1 > sb->cap) sb->cap *= 2;
        sb->data = realloc(sb->data, (size_t)sb->cap);
    }
    memcpy(sb->data + sb->len, s, (size_t)n + 1);
    sb->len += n;
}
static void sb_appendf(SB *sb, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sb_append(sb, buf);
}

/* ───────────────────────── error reporting ───────────────────────── */

static const char *g_filename = "input";
static void fail(int line, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (line > 0) fprintf(stderr, "%s:%d: error: %s\n", g_filename, line, buf);
    else fprintf(stderr, "%s: error: %s\n", g_filename, buf);
    exit(1);
}

/* ───────────────────────── lexer ───────────────────────── */

typedef enum {
    TK_EOF, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING,
    TK_TRUE, TK_FALSE, TK_LET, TK_MUT, TK_FN, TK_STRUCT, TK_ENUM, TK_MATCH,
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_IN, TK_RETURN,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_COLON, TK_ARROW, TK_FATARROW, TK_DOT, TK_DOTDOT, TK_QUESTION,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ANDAND, TK_OROR, TK_NOT
} TokKind;

/* One chunk of a string literal. `expr` is the source text inside `${...}`,
   or NULL when the chunk is plain text. */
typedef struct { char *lit; char *expr; int line; } StrPart;

typedef struct {
    TokKind kind;
    char *text;
    int64_t ival;
    double fval;
    int line;
    Vec *parts;   /* TK_STRING with interpolation; NULL otherwise */
} Token;

typedef struct { const char *src; int pos, len, line; } Lexer;

static void lexer_init(Lexer *lx, const char *src) {
    lx->src = src; lx->pos = 0; lx->len = (int)strlen(src); lx->line = 1;
}
static int lx_peek(Lexer *lx) { return lx->pos < lx->len ? (unsigned char)lx->src[lx->pos] : -1; }
static int lx_peek2(Lexer *lx) { return lx->pos + 1 < lx->len ? (unsigned char)lx->src[lx->pos + 1] : -1; }
static int lx_adv(Lexer *lx) {
    int c = lx_peek(lx);
    if (c == '\n') lx->line++;
    lx->pos++;
    return c;
}
static char *dupn(const char *s, int n) { char *r = malloc((size_t)n + 1); memcpy(r, s, (size_t)n); r[n] = 0; return r; }
static Token make_tok(TokKind k, int line) {
    Token t; t.kind = k; t.text = NULL; t.ival = 0; t.fval = 0; t.line = line; t.parts = NULL; return t;
}

static Token lex_next(Lexer *lx) {
    for (;;) {
        int c = lx_peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { lx_adv(lx); continue; }
        if (c == '/' && lx_peek2(lx) == '/') {
            while (lx_peek(lx) != -1 && lx_peek(lx) != '\n') lx_adv(lx);
            continue;
        }
        break;
    }
    int line = lx->line;
    int c = lx_peek(lx);
    if (c == -1) return make_tok(TK_EOF, line);

    if (isalpha(c) || c == '_') {
        int start = lx->pos;
        while (isalnum(lx_peek(lx)) || lx_peek(lx) == '_') lx_adv(lx);
        char *text = dupn(lx->src + start, lx->pos - start);
        struct { const char *kw; TokKind k; } kws[] = {
            {"true", TK_TRUE}, {"false", TK_FALSE}, {"let", TK_LET}, {"mut", TK_MUT},
            {"fn", TK_FN}, {"struct", TK_STRUCT}, {"enum", TK_ENUM}, {"match", TK_MATCH},
            {"if", TK_IF}, {"else", TK_ELSE}, {"while", TK_WHILE}, {"return", TK_RETURN},
            {"for", TK_FOR}, {"in", TK_IN},
        };
        for (size_t i = 0; i < sizeof kws / sizeof kws[0]; i++) {
            if (strcmp(text, kws[i].kw) == 0) { free(text); return make_tok(kws[i].k, line); }
        }
        Token t = make_tok(TK_IDENT, line);
        t.text = text;
        return t;
    }
    if (isdigit(c)) {
        int start = lx->pos;
        bool is_float = false;
        while (isdigit(lx_peek(lx))) lx_adv(lx);
        if (lx_peek(lx) == '.' && isdigit(lx_peek2(lx))) {
            is_float = true;
            lx_adv(lx);
            while (isdigit(lx_peek(lx))) lx_adv(lx);
        }
        char *text = dupn(lx->src + start, lx->pos - start);
        Token t = make_tok(is_float ? TK_FLOAT : TK_INT, line);
        if (is_float) t.fval = atof(text); else t.ival = atoll(text);
        free(text);
        return t;
    }
    if (c == '"') {
        lx_adv(lx);
        SB sb; sb_init(&sb);
        Vec *parts = NULL;   /* allocated lazily, only when `${` shows up */
        while (lx_peek(lx) != -1 && lx_peek(lx) != '"') {
            int ch = lx_peek(lx);
            if (ch == '$' && lx_peek2(lx) == '{') {
                lx_adv(lx); lx_adv(lx);
                if (!parts) { parts = malloc(sizeof(Vec)); vec_init(parts, sizeof(StrPart)); }
                if (sb.len > 0) {
                    StrPart *lp = vec_push(parts);
                    lp->lit = sb.data; lp->expr = NULL; lp->line = line;
                    sb_init(&sb);
                }
                /* Capture the source between the braces; nesting and nested string
                   literals are skipped so `${f("}")}` and `${a[b]}` survive. */
                int depth = 1, start = lx->pos, eline = lx->line;
                while (lx_peek(lx) != -1) {
                    int e = lx_peek(lx);
                    if (e == '"') {                 /* skip a nested string literal */
                        lx_adv(lx);
                        while (lx_peek(lx) != -1 && lx_peek(lx) != '"') {
                            if (lx_peek(lx) == '\\') lx_adv(lx);
                            lx_adv(lx);
                        }
                        if (lx_peek(lx) != '"') fail(eline, "unterminated string inside '${...}'");
                        lx_adv(lx);
                        continue;
                    }
                    if (e == '{') depth++;
                    if (e == '}') { depth--; if (depth == 0) break; }
                    lx_adv(lx);
                }
                if (lx_peek(lx) != '}') fail(eline, "unterminated '${' in string literal");
                char *expr_src = dupn(lx->src + start, lx->pos - start);
                lx_adv(lx);   /* consume '}' */
                bool blank = true;
                for (char *q = expr_src; *q; q++) if (!isspace((unsigned char)*q)) blank = false;
                if (blank) fail(eline, "empty '${}' in string literal");
                StrPart *ep = vec_push(parts);
                ep->lit = NULL; ep->expr = expr_src; ep->line = eline;
                continue;
            }
            lx_adv(lx);
            char buf[2] = {0, 0};
            if (ch == '\\') {
                int esc = lx_adv(lx);
                switch (esc) {
                    case 'n': buf[0] = '\n'; break;
                    case 't': buf[0] = '\t'; break;
                    case '"': buf[0] = '"'; break;
                    case '$': buf[0] = '$'; break;
                    case '\\': buf[0] = '\\'; break;
                    default: buf[0] = (char)esc; break;
                }
            } else buf[0] = (char)ch;
            sb_append(&sb, buf);
        }
        if (lx_peek(lx) != '"') fail(line, "unterminated string literal");
        lx_adv(lx);
        Token t = make_tok(TK_STRING, line);
        t.text = sb.data;
        if (parts) {
            if (sb.len > 0) {
                StrPart *lp = vec_push(parts);
                lp->lit = sb.data; lp->expr = NULL; lp->line = line;
            }
            t.parts = parts;
        }
        return t;
    }

#define TWO(a, b, k) if (c == (a) && lx_peek2(lx) == (b)) { lx_adv(lx); lx_adv(lx); return make_tok(k, line); }
    TWO('-', '>', TK_ARROW)
    TWO('=', '>', TK_FATARROW)
    TWO('.', '.', TK_DOTDOT)
    TWO('=', '=', TK_EQEQ)
    TWO('!', '=', TK_NEQ)
    TWO('<', '=', TK_LE)
    TWO('>', '=', TK_GE)
    TWO('&', '&', TK_ANDAND)
    TWO('|', '|', TK_OROR)
#undef TWO

    lx_adv(lx);
    switch (c) {
        case '(': return make_tok(TK_LPAREN, line);
        case ')': return make_tok(TK_RPAREN, line);
        case '{': return make_tok(TK_LBRACE, line);
        case '}': return make_tok(TK_RBRACE, line);
        case '[': return make_tok(TK_LBRACKET, line);
        case ']': return make_tok(TK_RBRACKET, line);
        case ',': return make_tok(TK_COMMA, line);
        case ':': return make_tok(TK_COLON, line);
        case '.': return make_tok(TK_DOT, line);
        case '?': return make_tok(TK_QUESTION, line);
        case '=': return make_tok(TK_EQ, line);
        case '<': return make_tok(TK_LT, line);
        case '>': return make_tok(TK_GT, line);
        case '+': return make_tok(TK_PLUS, line);
        case '-': return make_tok(TK_MINUS, line);
        case '*': return make_tok(TK_STAR, line);
        case '/': return make_tok(TK_SLASH, line);
        case '%': return make_tok(TK_PERCENT, line);
        case '!': return make_tok(TK_NOT, line);
        default: fail(line, "unexpected character '%c'", c); return make_tok(TK_EOF, line);
    }
}

/* ───────────────────────── types ───────────────────────── */

typedef enum { TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_STRING, TY_NAMED, TY_ARRAY, TY_VAR, TY_UNKNOWN } TyKind;

typedef struct Type Type;
struct Type {
    TyKind kind;
    char *name;  /* TY_NAMED: struct/enum name.  TY_VAR: type-parameter name. */
    Vec args;    /* Vec<Type*> — generic arguments for TY_NAMED, element type for TY_ARRAY */
};

static Type *ty_new(TyKind k) {
    Type *t = calloc(1, sizeof(Type));
    t->kind = k;
    vec_init(&t->args, sizeof(Type *));
    return t;
}
static Type *ty_prim(TyKind k) { static Type *cache[TY_UNKNOWN + 1]; if (!cache[k]) cache[k] = ty_new(k); return cache[k]; }
static Type *ty_void(void) { return ty_prim(TY_VOID); }
static Type *ty_int(void) { return ty_prim(TY_INT); }
static Type *ty_float(void) { return ty_prim(TY_FLOAT); }
static Type *ty_bool(void) { return ty_prim(TY_BOOL); }
static Type *ty_string(void) { return ty_prim(TY_STRING); }
static Type *ty_unknown(void) { return ty_prim(TY_UNKNOWN); }
static Type *ty_named(const char *name) { Type *t = ty_new(TY_NAMED); t->name = strdup(name); return t; }
static Type *ty_var(const char *name) { Type *t = ty_new(TY_VAR); t->name = strdup(name); return t; }
static Type *ty_array(Type *elem) { Type *t = ty_new(TY_ARRAY); VEC_PUSH_PTR(&t->args, elem); return t; }
static Type *ty_elem(const Type *t) { return VEC_PTR(&t->args, 0, Type); }

static bool ty_eq(const Type *a, const Type *b) {
    if (a->kind != b->kind) return false;
    if (a->kind == TY_ARRAY) return ty_eq(ty_elem(a), ty_elem(b));
    if (a->kind == TY_NAMED || a->kind == TY_VAR) {
        if (strcmp(a->name, b->name) != 0) return false;
        if (a->args.count != b->args.count) return false;
        for (int i = 0; i < a->args.count; i++)
            if (!ty_eq(VEC_PTR(&a->args, i, Type), VEC_PTR(&b->args, i, Type))) return false;
    }
    return true;
}
static bool ty_has_var(const Type *t) {
    if (t->kind == TY_VAR) return true;
    for (int i = 0; i < t->args.count; i++)
        if (ty_has_var(VEC_PTR(&t->args, i, Type))) return true;
    return false;
}
/* Human-readable, for error messages: Option<int> */
static const char *ty_str(const Type *t) {
    static char bufs[8][256];
    static int slot = 0;
    char *buf = bufs[slot = (slot + 1) % 8];
    switch (t->kind) {
        case TY_VOID: return "void";
        case TY_INT: return "int";
        case TY_FLOAT: return "float";
        case TY_BOOL: return "bool";
        case TY_STRING: return "string";
        case TY_UNKNOWN: return "<unknown>";
        case TY_ARRAY: snprintf(buf, 256, "[%s]", ty_str(ty_elem(t))); return buf;
        default: break;
    }
    if (t->args.count == 0) { snprintf(buf, 256, "%s", t->name); return buf; }
    char inner[200] = "";
    for (int i = 0; i < t->args.count; i++) {
        if (i) strncat(inner, ", ", sizeof inner - strlen(inner) - 1);
        strncat(inner, ty_str(VEC_PTR(&t->args, i, Type)), sizeof inner - strlen(inner) - 1);
    }
    snprintf(buf, 256, "%s<%s>", t->name, inner);
    return buf;
}
/* C-identifier-safe name: Option_int, Result_int_string */
static void ty_mangle_into(const Type *t, SB *sb) {
    switch (t->kind) {
        case TY_VOID: sb_append(sb, "void"); return;
        case TY_INT: sb_append(sb, "int"); return;
        case TY_FLOAT: sb_append(sb, "float"); return;
        case TY_BOOL: sb_append(sb, "bool"); return;
        case TY_STRING: sb_append(sb, "string"); return;
        case TY_VAR: sb_append(sb, t->name); return;
        case TY_ARRAY: sb_append(sb, "arr_"); ty_mangle_into(ty_elem(t), sb); return;
        default: break;
    }
    sb_append(sb, t->name);
    for (int i = 0; i < t->args.count; i++) {
        sb_append(sb, "_");
        ty_mangle_into(VEC_PTR(&t->args, i, Type), sb);
    }
}
static char *ty_mangle(const Type *t) { SB sb; sb_init(&sb); ty_mangle_into(t, &sb); return sb.data; }

/* ───────────────────────── AST ───────────────────────── */

typedef enum {
    EX_INT, EX_FLOAT, EX_BOOL, EX_STRING, EX_IDENT,
    EX_BINARY, EX_UNARY, EX_CALL, EX_FIELD, EX_STRUCT_LIT,
    EX_VARIANT, EX_MATCH, EX_TRY, EX_ARRAY_LIT, EX_INDEX
} ExprKind;

typedef struct Expr Expr;
typedef struct Stmt Stmt;

typedef struct { char *name; Expr *value; } FieldInit;

typedef struct {
    char *variant;  /* NULL means wildcard `_` */
    Vec binds;      /* Vec<char*> — names bound to payload slots */
    int line;
} Pattern;

typedef struct {
    Pattern pat;
    Expr *value;   /* `=> expr` form */
    Vec body;      /* `=> { ... }` form, Vec<Stmt*> */
    bool is_block;
    int line;
} MatchArm;

struct Expr {
    ExprKind kind;
    int line;
    Type *type;

    int64_t ival;
    double fval;
    bool bval;
    char *sval;      /* string literal / ident / field / callee / struct or variant name */
    char *op;
    char *resolved;  /* mangled callee name after monomorphization */
    Expr *lhs, *rhs;
    Vec args;        /* Vec<Expr*> */
    Vec fields;      /* Vec<FieldInit> */
    Vec arms;        /* Vec<MatchArm> */
};

typedef enum { ST_LET, ST_ASSIGN, ST_IF, ST_WHILE, ST_FOR, ST_RETURN, ST_EXPR, ST_BLOCK } StmtKind;

typedef struct { Expr *cond; Vec body; } CondBlock;

struct Stmt {
    StmtKind kind;
    int line;
    char *name;       /* ST_LET / ST_FOR: bound variable */
    bool is_mut;
    bool has_type;
    bool is_range;    /* ST_FOR: `for i in a..b` rather than over an array */
    Type *decl_type;
    Expr *expr;       /* ST_FOR: the array, or the range's lower bound */
    Expr *expr2;      /* ST_FOR: the range's upper bound */
    Expr *target;     /* ST_ASSIGN: lvalue being assigned to */
    Vec cond_blocks;  /* Vec<CondBlock> */
    Vec body;         /* Vec<Stmt*> */
};

typedef struct { char *name; Type *type; bool is_mut; } Field;
typedef struct { char *name; Vec payload; /* Vec<Type*> */ } Variant;

typedef struct { char *name; Vec type_params; Vec fields; char *mangled; } StructDecl;
typedef struct { char *name; Vec type_params; Vec variants; char *mangled; } EnumDecl;
typedef struct {
    char *name; Vec type_params; Vec params; /* Vec<Field> */
    Type *ret_type; Vec body; char *mangled;
} FnDecl;

typedef enum { DECL_STRUCT, DECL_ENUM, DECL_FN } DeclKind;
typedef struct { DeclKind kind; StructDecl *s; EnumDecl *e; FnDecl *f; } Decl;

static Vec g_decls;  /* Vec<Decl> — generic originals, from prelude + user source */

/* ───────────────────────── parser ───────────────────────── */

typedef struct { Lexer lx; Token cur; bool no_struct_lit; } Parser;

static void p_advance(Parser *p) { p->cur = lex_next(&p->lx); }
static void parser_init(Parser *p, const char *src) {
    lexer_init(&p->lx, src); p->no_struct_lit = false; p_advance(p);
}
static const char *tok_name(TokKind k) {
    switch (k) {
        case TK_EOF: return "end of file";
        case TK_IDENT: return "identifier";
        case TK_LBRACE: return "'{'"; case TK_RBRACE: return "'}'";
        case TK_LPAREN: return "'('"; case TK_RPAREN: return "')'";
        case TK_COLON: return "':'"; case TK_COMMA: return "','";
        case TK_EQ: return "'='"; case TK_ARROW: return "'->'";
        case TK_FATARROW: return "'=>'"; case TK_GT: return "'>'";
        default: return "token";
    }
}
static bool p_check(Parser *p, TokKind k) { return p->cur.kind == k; }
static Token p_expect(Parser *p, TokKind k) {
    if (p->cur.kind != k) fail(p->cur.line, "expected %s", tok_name(k));
    Token t = p->cur;
    p_advance(p);
    return t;
}
static bool p_match(Parser *p, TokKind k) {
    if (p_check(p, k)) { p_advance(p); return true; }
    return false;
}

static Expr *new_expr(ExprKind k, int line) {
    Expr *e = calloc(1, sizeof(Expr));
    e->kind = k; e->line = line; e->type = ty_unknown();
    vec_init(&e->args, sizeof(Expr *));
    vec_init(&e->fields, sizeof(FieldInit));
    vec_init(&e->arms, sizeof(MatchArm));
    return e;
}
static Stmt *new_stmt(StmtKind k, int line) {
    Stmt *s = calloc(1, sizeof(Stmt));
    s->kind = k; s->line = line;
    vec_init(&s->cond_blocks, sizeof(CondBlock));
    vec_init(&s->body, sizeof(Stmt *));
    return s;
}

static Type *parse_type(Parser *p) {
    if (p_match(p, TK_LBRACKET)) {
        Type *elem = parse_type(p);
        p_expect(p, TK_RBRACKET);
        return ty_array(elem);
    }
    Token t = p_expect(p, TK_IDENT);
    if (strcmp(t.text, "int") == 0) return ty_int();
    if (strcmp(t.text, "float") == 0) return ty_float();
    if (strcmp(t.text, "bool") == 0) return ty_bool();
    if (strcmp(t.text, "string") == 0) return ty_string();
    Type *ty = ty_named(t.text);
    if (p_match(p, TK_LT)) {
        do { VEC_PUSH_PTR(&ty->args, parse_type(p)); } while (p_match(p, TK_COMMA));
        p_expect(p, TK_GT);
    }
    return ty;
}

static Vec parse_type_params(Parser *p) {
    Vec tp; vec_init(&tp, sizeof(char *));
    if (p_match(p, TK_LT)) {
        do { VEC_PUSH_PTR(&tp, p_expect(p, TK_IDENT).text); } while (p_match(p, TK_COMMA));
        p_expect(p, TK_GT);
    }
    return tp;
}

/* `T` parses as a named type; once we know the enclosing declaration's type
   parameters, rewrite those occurrences into real type variables. */
static Type *bind_type_vars(Type *t, const Vec *type_params) {
    if (t->kind == TY_ARRAY) return ty_array(bind_type_vars(ty_elem(t), type_params));
    if (t->kind != TY_NAMED) return t;
    if (t->args.count == 0) {
        for (int i = 0; i < type_params->count; i++)
            if (strcmp(VEC_PTR(type_params, i, char), t->name) == 0) return ty_var(t->name);
        return t;
    }
    Type *r = ty_named(t->name);
    for (int i = 0; i < t->args.count; i++)
        VEC_PUSH_PTR(&r->args, bind_type_vars(VEC_PTR(&t->args, i, Type), type_params));
    return r;
}

static Expr *parse_expr(Parser *p);
static Vec parse_block(Parser *p);

static Pattern parse_pattern(Parser *p) {
    Pattern pat;
    pat.line = p->cur.line;
    pat.variant = NULL;
    vec_init(&pat.binds, sizeof(char *));
    Token t = p_expect(p, TK_IDENT);
    if (strcmp(t.text, "_") == 0) return pat;
    pat.variant = t.text;
    if (p_match(p, TK_LPAREN)) {
        while (!p_check(p, TK_RPAREN)) {
            VEC_PUSH_PTR(&pat.binds, p_expect(p, TK_IDENT).text);
            if (!p_match(p, TK_COMMA)) break;
        }
        p_expect(p, TK_RPAREN);
    }
    return pat;
}

static Expr *parse_match(Parser *p, int line) {
    Expr *e = new_expr(EX_MATCH, line);
    bool saved = p->no_struct_lit;
    p->no_struct_lit = true;
    e->lhs = parse_expr(p);
    p->no_struct_lit = saved;
    p_expect(p, TK_LBRACE);
    while (!p_check(p, TK_RBRACE)) {
        MatchArm arm;
        memset(&arm, 0, sizeof arm);
        arm.line = p->cur.line;
        arm.pat = parse_pattern(p);
        p_expect(p, TK_FATARROW);
        vec_init(&arm.body, sizeof(Stmt *));
        if (p_check(p, TK_LBRACE)) { arm.is_block = true; arm.body = parse_block(p); }
        else arm.value = parse_expr(p);
        *(MatchArm *)vec_push(&e->arms) = arm;
        p_match(p, TK_COMMA);
    }
    p_expect(p, TK_RBRACE);
    return e;
}

/* `"a${x}b"` desugars to `"a" + to_string(x) + "b"`. to_string is a no-op on
   strings, so every printable type interpolates without the caller saying so. */
static Expr *parse_interp(const Token *tok) {
    Expr *acc = NULL;
    for (int i = 0; i < tok->parts->count; i++) {
        StrPart *sp = vec_get(tok->parts, i);
        Expr *piece;
        if (sp->expr) {
            Parser sub;
            lexer_init(&sub.lx, sp->expr);
            sub.lx.line = sp->line;      /* so errors point at the real source line */
            sub.no_struct_lit = false;
            p_advance(&sub);
            piece = parse_expr(&sub);
            if (!p_check(&sub, TK_EOF)) fail(sp->line, "unexpected trailing tokens inside '${...}'");
            Expr *call = new_expr(EX_CALL, sp->line);
            call->sval = "to_string";
            VEC_PUSH_PTR(&call->args, piece);
            piece = call;
        } else {
            piece = new_expr(EX_STRING, sp->line);
            piece->sval = sp->lit;
        }
        if (!acc) { acc = piece; continue; }
        Expr *cat = new_expr(EX_BINARY, sp->line);
        cat->op = strdup("+");
        cat->lhs = acc;
        cat->rhs = piece;
        acc = cat;
    }
    return acc;
}

static Expr *parse_primary(Parser *p) {
    int line = p->cur.line;
    if (p_check(p, TK_INT)) { Expr *e = new_expr(EX_INT, line); e->ival = p->cur.ival; p_advance(p); return e; }
    if (p_check(p, TK_FLOAT)) { Expr *e = new_expr(EX_FLOAT, line); e->fval = p->cur.fval; p_advance(p); return e; }
    if (p_check(p, TK_STRING)) {
        Token tok = p->cur;
        p_advance(p);
        if (tok.parts) return parse_interp(&tok);
        Expr *e = new_expr(EX_STRING, line);
        e->sval = tok.text;
        return e;
    }
    if (p_check(p, TK_LBRACKET)) {
        Expr *e = new_expr(EX_ARRAY_LIT, line);
        p_advance(p);
        bool saved = p->no_struct_lit;
        p->no_struct_lit = false;
        while (!p_check(p, TK_RBRACKET)) {
            VEC_PUSH_PTR(&e->args, parse_expr(p));
            if (!p_match(p, TK_COMMA)) break;
        }
        p->no_struct_lit = saved;
        p_expect(p, TK_RBRACKET);
        return e;
    }
    if (p_match(p, TK_TRUE)) { Expr *e = new_expr(EX_BOOL, line); e->bval = true; return e; }
    if (p_match(p, TK_FALSE)) { Expr *e = new_expr(EX_BOOL, line); e->bval = false; return e; }
    if (p_match(p, TK_MATCH)) return parse_match(p, line);
    if (p_check(p, TK_IDENT)) {
        char *name = p->cur.text;
        p_advance(p);
        if (p_check(p, TK_LPAREN)) {
            /* call or variant constructor — resolved during typecheck */
            Expr *e = new_expr(EX_CALL, line);
            e->sval = name;
            p_advance(p);
            while (!p_check(p, TK_RPAREN)) {
                VEC_PUSH_PTR(&e->args, parse_expr(p));
                if (!p_match(p, TK_COMMA)) break;
            }
            p_expect(p, TK_RPAREN);
            return e;
        }
        if (p_check(p, TK_LBRACE) && !p->no_struct_lit) {
            Expr *e = new_expr(EX_STRUCT_LIT, line);
            e->sval = name;
            p_advance(p);
            while (!p_check(p, TK_RBRACE)) {
                Token fname = p_expect(p, TK_IDENT);
                p_expect(p, TK_COLON);
                FieldInit *fi = vec_push(&e->fields);
                fi->name = fname.text;
                fi->value = parse_expr(p);
                if (!p_match(p, TK_COMMA)) break;
            }
            p_expect(p, TK_RBRACE);
            return e;
        }
        Expr *e = new_expr(EX_IDENT, line);
        e->sval = name;
        return e;
    }
    if (p_match(p, TK_LPAREN)) {
        bool saved = p->no_struct_lit;
        p->no_struct_lit = false;
        Expr *e = parse_expr(p);
        p->no_struct_lit = saved;
        p_expect(p, TK_RPAREN);
        return e;
    }
    fail(line, "unexpected token in expression");
    return NULL;
}

static Expr *parse_postfix(Parser *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        if (p_match(p, TK_DOT)) {
            Token f = p_expect(p, TK_IDENT);
            Expr *fe = new_expr(EX_FIELD, f.line);
            fe->lhs = e; fe->sval = f.text;
            e = fe;
        } else if (p_check(p, TK_LBRACKET)) {
            int line = p->cur.line;
            p_advance(p);
            bool saved = p->no_struct_lit;
            p->no_struct_lit = false;
            Expr *ix = new_expr(EX_INDEX, line);
            ix->lhs = e;
            ix->rhs = parse_expr(p);
            p->no_struct_lit = saved;
            p_expect(p, TK_RBRACKET);
            e = ix;
        } else if (p_check(p, TK_QUESTION)) {
            int line = p->cur.line;
            p_advance(p);
            Expr *te = new_expr(EX_TRY, line);
            te->lhs = e;
            e = te;
        } else return e;
    }
}

static Expr *parse_unary(Parser *p) {
    if (p_check(p, TK_MINUS) || p_check(p, TK_NOT)) {
        int line = p->cur.line;
        const char *op = p_check(p, TK_MINUS) ? "-" : "!";
        p_advance(p);
        Expr *e = new_expr(EX_UNARY, line);
        e->op = strdup(op);
        e->lhs = parse_unary(p);
        return e;
    }
    return parse_postfix(p);
}

static Expr *parse_binop(Parser *p, int min_prec) {
    Expr *lhs = parse_unary(p);
    for (;;) {
        int prec; const char *opname;
        switch (p->cur.kind) {
            case TK_OROR:   prec = 1; opname = "||"; break;
            case TK_ANDAND: prec = 2; opname = "&&"; break;
            case TK_EQEQ:   prec = 3; opname = "=="; break;
            case TK_NEQ:    prec = 3; opname = "!="; break;
            case TK_LT:     prec = 4; opname = "<";  break;
            case TK_LE:     prec = 4; opname = "<="; break;
            case TK_GT:     prec = 4; opname = ">";  break;
            case TK_GE:     prec = 4; opname = ">="; break;
            case TK_PLUS:   prec = 5; opname = "+";  break;
            case TK_MINUS:  prec = 5; opname = "-";  break;
            case TK_STAR:   prec = 6; opname = "*";  break;
            case TK_SLASH:  prec = 6; opname = "/";  break;
            case TK_PERCENT:prec = 6; opname = "%";  break;
            default: return lhs;
        }
        if (prec < min_prec) return lhs;
        int line = p->cur.line;
        p_advance(p);
        Expr *rhs = parse_binop(p, prec + 1);
        Expr *e = new_expr(EX_BINARY, line);
        e->op = strdup(opname);
        e->lhs = lhs; e->rhs = rhs;
        lhs = e;
    }
}
static Expr *parse_expr(Parser *p) { return parse_binop(p, 1); }

static Stmt *parse_stmt(Parser *p) {
    int line = p->cur.line;
    if (p_match(p, TK_LET)) {
        Stmt *s = new_stmt(ST_LET, line);
        s->is_mut = p_match(p, TK_MUT);
        s->name = p_expect(p, TK_IDENT).text;
        if (p_match(p, TK_COLON)) { s->has_type = true; s->decl_type = parse_type(p); }
        p_expect(p, TK_EQ);
        s->expr = parse_expr(p);
        return s;
    }
    if (p_match(p, TK_IF)) {
        Stmt *s = new_stmt(ST_IF, line);
        for (;;) {
            CondBlock *cb = vec_push(&s->cond_blocks);
            p->no_struct_lit = true;
            cb->cond = parse_expr(p);
            p->no_struct_lit = false;
            cb->body = parse_block(p);
            if (p_match(p, TK_ELSE)) {
                if (p_match(p, TK_IF)) continue;
                CondBlock *eb = vec_push(&s->cond_blocks);
                eb->cond = NULL;
                eb->body = parse_block(p);
            }
            break;
        }
        return s;
    }
    if (p_match(p, TK_WHILE)) {
        Stmt *s = new_stmt(ST_WHILE, line);
        p->no_struct_lit = true;
        s->expr = parse_expr(p);
        p->no_struct_lit = false;
        s->body = parse_block(p);
        return s;
    }
    if (p_match(p, TK_FOR)) {
        Stmt *s = new_stmt(ST_FOR, line);
        s->name = p_expect(p, TK_IDENT).text;
        if (!p_match(p, TK_IN)) fail(p->cur.line, "expected 'in' after the loop variable");
        p->no_struct_lit = true;
        s->expr = parse_expr(p);
        if (p_match(p, TK_DOTDOT)) { s->is_range = true; s->expr2 = parse_expr(p); }
        p->no_struct_lit = false;
        s->body = parse_block(p);
        return s;
    }
    if (p_match(p, TK_RETURN)) {
        Stmt *s = new_stmt(ST_RETURN, line);
        if (!p_check(p, TK_RBRACE)) s->expr = parse_expr(p);
        return s;
    }
    if (p_check(p, TK_LBRACE)) {
        Stmt *s = new_stmt(ST_BLOCK, line);
        s->body = parse_block(p);
        return s;
    }
    /* Anything else is an expression; a trailing '=' turns it into an assignment.
       Validating that the left side is actually assignable happens in typecheck. */
    Expr *e = parse_expr(p);
    if (p_match(p, TK_EQ)) {
        Stmt *s = new_stmt(ST_ASSIGN, line);
        s->target = e;
        s->expr = parse_expr(p);
        return s;
    }
    Stmt *s = new_stmt(ST_EXPR, line);
    s->expr = e;
    return s;
}

static Vec parse_block(Parser *p) {
    Vec body; vec_init(&body, sizeof(Stmt *));
    p_expect(p, TK_LBRACE);
    while (!p_check(p, TK_RBRACE)) VEC_PUSH_PTR(&body, parse_stmt(p));
    p_expect(p, TK_RBRACE);
    return body;
}

static void bind_stmts_type_vars(Vec *body, const Vec *tp);

static void bind_expr_type_vars(Expr *e, const Vec *tp) {
    if (!e) return;
    bind_expr_type_vars(e->lhs, tp);
    bind_expr_type_vars(e->rhs, tp);
    for (int i = 0; i < e->args.count; i++) bind_expr_type_vars(VEC_PTR(&e->args, i, Expr), tp);
    for (int i = 0; i < e->fields.count; i++) bind_expr_type_vars(((FieldInit *)vec_get(&e->fields, i))->value, tp);
    for (int i = 0; i < e->arms.count; i++) {
        MatchArm *arm = vec_get(&e->arms, i);
        bind_expr_type_vars(arm->value, tp);
        bind_stmts_type_vars(&arm->body, tp);
    }
}

static void bind_stmts_type_vars(Vec *body, const Vec *tp) {
    for (int i = 0; i < body->count; i++) {
        Stmt *s = VEC_PTR(body, i, Stmt);
        if (s->decl_type) s->decl_type = bind_type_vars(s->decl_type, tp);
        bind_expr_type_vars(s->expr, tp);
        bind_expr_type_vars(s->expr2, tp);
        bind_expr_type_vars(s->target, tp);
        for (int j = 0; j < s->cond_blocks.count; j++) {
            CondBlock *cb = vec_get(&s->cond_blocks, j);
            bind_expr_type_vars(cb->cond, tp);
            bind_stmts_type_vars(&cb->body, tp);
        }
        bind_stmts_type_vars(&s->body, tp);
    }
}

static void parse_program(Parser *p) {
    while (!p_check(p, TK_EOF)) {
        if (p_check(p, TK_STRUCT)) {
            p_advance(p);
            StructDecl *sd = calloc(1, sizeof(StructDecl));
            sd->name = p_expect(p, TK_IDENT).text;
            sd->type_params = parse_type_params(p);
            vec_init(&sd->fields, sizeof(Field));
            p_expect(p, TK_LBRACE);
            while (!p_check(p, TK_RBRACE)) {
                Field *f = vec_push(&sd->fields);
                f->name = p_expect(p, TK_IDENT).text;
                p_expect(p, TK_COLON);
                f->type = parse_type(p);
                if (!p_match(p, TK_COMMA)) break;
            }
            p_expect(p, TK_RBRACE);
            for (int i = 0; i < sd->fields.count; i++) {
                Field *f = vec_get(&sd->fields, i);
                f->type = bind_type_vars(f->type, &sd->type_params);
            }
            Decl *d = vec_push(&g_decls);
            memset(d, 0, sizeof *d);
            d->kind = DECL_STRUCT; d->s = sd;
        } else if (p_check(p, TK_ENUM)) {
            p_advance(p);
            EnumDecl *ed = calloc(1, sizeof(EnumDecl));
            ed->name = p_expect(p, TK_IDENT).text;
            ed->type_params = parse_type_params(p);
            vec_init(&ed->variants, sizeof(Variant));
            p_expect(p, TK_LBRACE);
            while (!p_check(p, TK_RBRACE)) {
                Variant *v = vec_push(&ed->variants);
                v->name = p_expect(p, TK_IDENT).text;
                vec_init(&v->payload, sizeof(Type *));
                if (p_match(p, TK_LPAREN)) {
                    while (!p_check(p, TK_RPAREN)) {
                        VEC_PUSH_PTR(&v->payload, parse_type(p));
                        if (!p_match(p, TK_COMMA)) break;
                    }
                    p_expect(p, TK_RPAREN);
                }
                if (!p_match(p, TK_COMMA)) break;
            }
            p_expect(p, TK_RBRACE);
            for (int i = 0; i < ed->variants.count; i++) {
                Variant *v = vec_get(&ed->variants, i);
                for (int j = 0; j < v->payload.count; j++)
                    *(Type **)vec_get(&v->payload, j) =
                        bind_type_vars(VEC_PTR(&v->payload, j, Type), &ed->type_params);
            }
            Decl *d = vec_push(&g_decls);
            memset(d, 0, sizeof *d);
            d->kind = DECL_ENUM; d->e = ed;
        } else if (p_check(p, TK_FN)) {
            p_advance(p);
            FnDecl *fd = calloc(1, sizeof(FnDecl));
            fd->name = p_expect(p, TK_IDENT).text;
            fd->type_params = parse_type_params(p);
            vec_init(&fd->params, sizeof(Field));
            p_expect(p, TK_LPAREN);
            while (!p_check(p, TK_RPAREN)) {
                Field *pm = vec_push(&fd->params);
                pm->is_mut = p_match(p, TK_MUT);
                pm->name = p_expect(p, TK_IDENT).text;
                p_expect(p, TK_COLON);
                pm->type = parse_type(p);
                if (!p_match(p, TK_COMMA)) break;
            }
            p_expect(p, TK_RPAREN);
            fd->ret_type = p_match(p, TK_ARROW) ? parse_type(p) : ty_void();
            fd->body = parse_block(p);
            for (int i = 0; i < fd->params.count; i++) {
                Field *pm = vec_get(&fd->params, i);
                pm->type = bind_type_vars(pm->type, &fd->type_params);
            }
            fd->ret_type = bind_type_vars(fd->ret_type, &fd->type_params);
            bind_stmts_type_vars(&fd->body, &fd->type_params);
            Decl *d = vec_push(&g_decls);
            memset(d, 0, sizeof *d);
            d->kind = DECL_FN; d->f = fd;
        } else fail(p->cur.line, "expected 'fn', 'struct' or 'enum' at top level");
    }
}

/* ───────────────────────── generic lookups ───────────────────────── */

static StructDecl *find_struct(const char *name) {
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_STRUCT && strcmp(d->s->name, name) == 0) return d->s;
    }
    return NULL;
}
static EnumDecl *find_enum(const char *name) {
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_ENUM && strcmp(d->e->name, name) == 0) return d->e;
    }
    return NULL;
}
static FnDecl *find_fn(const char *name) {
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_FN && strcmp(d->f->name, name) == 0) return d->f;
    }
    return NULL;
}
static int variant_index(const EnumDecl *ed, const char *name) {
    for (int i = 0; i < ed->variants.count; i++)
        if (strcmp(((Variant *)vec_get(&ed->variants, i))->name, name) == 0) return i;
    return -1;
}
/* Variants are used unqualified (`Some(x)`, not `Option::Some(x)`), so find the
   unique enum declaring this variant name. */
static EnumDecl *find_enum_by_variant(const char *vname, int line) {
    EnumDecl *found = NULL;
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind != DECL_ENUM) continue;
        if (variant_index(d->e, vname) < 0) continue;
        if (found) fail(line, "variant '%s' is declared by both '%s' and '%s' — rename one to disambiguate",
                        vname, found->name, d->e->name);
        found = d->e;
    }
    return found;
}

/* ───────────────────────── substitution & cloning ───────────────────────── */

typedef struct { char *name; Type *type; } Binding;

static Type *subst_type(Type *t, const Vec *subst) {
    if (t->kind == TY_VAR) {
        for (int i = 0; i < subst->count; i++) {
            Binding *b = vec_get(subst, i);
            if (strcmp(b->name, t->name) == 0) return b->type;
        }
        return t;
    }
    if (t->kind == TY_ARRAY) {
        Type *e = subst_type(ty_elem(t), subst);
        return e == ty_elem(t) ? t : ty_array(e);
    }
    if (t->kind != TY_NAMED || t->args.count == 0) return t;
    Type *r = ty_named(t->name);
    for (int i = 0; i < t->args.count; i++)
        VEC_PUSH_PTR(&r->args, subst_type(VEC_PTR(&t->args, i, Type), subst));
    return r;
}

static bool unify(Type *pat, Type *actual, Vec *subst) {
    if (pat->kind == TY_VAR) {
        for (int i = 0; i < subst->count; i++) {
            Binding *b = vec_get(subst, i);
            if (strcmp(b->name, pat->name) == 0) return ty_eq(b->type, actual);
        }
        Binding *b = vec_push(subst);
        b->name = pat->name; b->type = actual;
        return true;
    }
    if (pat->kind != actual->kind) return false;
    if (pat->kind == TY_ARRAY) return unify(ty_elem(pat), ty_elem(actual), subst);
    if (pat->kind == TY_NAMED) {
        if (strcmp(pat->name, actual->name) != 0) return false;
        if (pat->args.count != actual->args.count) return false;
        for (int i = 0; i < pat->args.count; i++)
            if (!unify(VEC_PTR(&pat->args, i, Type), VEC_PTR(&actual->args, i, Type), subst)) return false;
    }
    return true;
}

static Expr *clone_expr(Expr *e, const Vec *subst);
static Stmt *clone_stmt(Stmt *s, const Vec *subst);

static Vec clone_stmts(const Vec *body, const Vec *subst) {
    Vec r; vec_init(&r, sizeof(Stmt *));
    for (int i = 0; i < body->count; i++) VEC_PUSH_PTR(&r, clone_stmt(VEC_PTR(body, i, Stmt), subst));
    return r;
}

static Expr *clone_expr(Expr *e, const Vec *subst) {
    if (!e) return NULL;
    Expr *c = new_expr(e->kind, e->line);
    c->ival = e->ival; c->fval = e->fval; c->bval = e->bval;
    c->sval = e->sval; c->op = e->op;
    c->lhs = clone_expr(e->lhs, subst);
    c->rhs = clone_expr(e->rhs, subst);
    for (int i = 0; i < e->args.count; i++)
        VEC_PUSH_PTR(&c->args, clone_expr(VEC_PTR(&e->args, i, Expr), subst));
    for (int i = 0; i < e->fields.count; i++) {
        FieldInit *src = vec_get(&e->fields, i);
        FieldInit *dst = vec_push(&c->fields);
        dst->name = src->name;
        dst->value = clone_expr(src->value, subst);
    }
    for (int i = 0; i < e->arms.count; i++) {
        MatchArm *src = vec_get(&e->arms, i);
        MatchArm *dst = vec_push(&c->arms);
        memset(dst, 0, sizeof *dst);
        dst->line = src->line;
        dst->is_block = src->is_block;
        dst->pat = src->pat;
        dst->value = clone_expr(src->value, subst);
        dst->body = clone_stmts(&src->body, subst);
    }
    return c;
}

static Stmt *clone_stmt(Stmt *s, const Vec *subst) {
    Stmt *c = new_stmt(s->kind, s->line);
    c->name = s->name;
    c->is_mut = s->is_mut;
    c->has_type = s->has_type;
    c->is_range = s->is_range;
    c->decl_type = s->decl_type ? subst_type(s->decl_type, subst) : NULL;
    c->expr = clone_expr(s->expr, subst);
    c->expr2 = clone_expr(s->expr2, subst);
    c->target = clone_expr(s->target, subst);
    for (int i = 0; i < s->cond_blocks.count; i++) {
        CondBlock *src = vec_get(&s->cond_blocks, i);
        CondBlock *dst = vec_push(&c->cond_blocks);
        dst->cond = clone_expr(src->cond, subst);
        dst->body = clone_stmts(&src->body, subst);
    }
    c->body = clone_stmts(&s->body, subst);
    return c;
}

/* ───────────────────────── instantiation registries ───────────────────────── */

#define MAX_INSTANCES 500

static Vec g_mono_structs;  /* Vec<StructDecl*> */
static Vec g_mono_enums;    /* Vec<EnumDecl*>   */
static Vec g_mono_fns;      /* Vec<FnDecl*>     */
static Vec g_mono_arrays;   /* Vec<Type*> — one entry per distinct element type */
static Vec g_fn_queue;      /* Vec<FnDecl*> — pending typecheck */

static bool array_registered(const char *mangled) {
    for (int i = 0; i < g_mono_arrays.count; i++)
        if (strcmp(ty_mangle(VEC_PTR(&g_mono_arrays, i, Type)), mangled) == 0) return true;
    return false;
}

static StructDecl *find_mono_struct(const char *mangled) {
    for (int i = 0; i < g_mono_structs.count; i++) {
        StructDecl *sd = VEC_PTR(&g_mono_structs, i, StructDecl);
        if (strcmp(sd->mangled, mangled) == 0) return sd;
    }
    return NULL;
}
static EnumDecl *find_mono_enum(const char *mangled) {
    for (int i = 0; i < g_mono_enums.count; i++) {
        EnumDecl *ed = VEC_PTR(&g_mono_enums, i, EnumDecl);
        if (strcmp(ed->mangled, mangled) == 0) return ed;
    }
    return NULL;
}
static FnDecl *find_mono_fn(const char *mangled) {
    for (int i = 0; i < g_mono_fns.count; i++) {
        FnDecl *fd = VEC_PTR(&g_mono_fns, i, FnDecl);
        if (strcmp(fd->mangled, mangled) == 0) return fd;
    }
    return NULL;
}

static Vec make_subst(const Vec *type_params, const Vec *args, const char *what, const char *name, int line) {
    Vec subst; vec_init(&subst, sizeof(Binding));
    if (type_params->count != args->count)
        fail(line, "%s '%s' expects %d type argument(s), got %d",
             what, name, type_params->count, args->count);
    for (int i = 0; i < type_params->count; i++) {
        Binding *b = vec_push(&subst);
        b->name = VEC_PTR(type_params, i, char);
        b->type = VEC_PTR(args, i, Type);
    }
    return subst;
}

/* Ensure the concrete type `t` (and everything it contains) has been instantiated. */
static void request_type(Type *t, int line) {
    if (t->kind == TY_ARRAY) {
        request_type(ty_elem(t), line);
        char *am = ty_mangle(t);
        if (!array_registered(am)) VEC_PUSH_PTR(&g_mono_arrays, t);
        free(am);
        return;
    }
    if (t->kind != TY_NAMED) return;
    for (int i = 0; i < t->args.count; i++) request_type(VEC_PTR(&t->args, i, Type), line);

    char *mangled = ty_mangle(t);
    if (find_mono_struct(mangled) || find_mono_enum(mangled)) { free(mangled); return; }

    if (g_mono_structs.count + g_mono_enums.count > MAX_INSTANCES)
        fail(line, "too many generic instantiations — is a type defined in terms of itself? "
                   "(recursive types need indirection, which Klang does not have yet)");

    StructDecl *gs = find_struct(t->name);
    if (gs) {
        Vec subst = make_subst(&gs->type_params, &t->args, "struct", t->name, line);
        StructDecl *sd = calloc(1, sizeof(StructDecl));
        sd->name = gs->name;
        sd->mangled = mangled;
        vec_init(&sd->type_params, sizeof(char *));
        vec_init(&sd->fields, sizeof(Field));
        VEC_PUSH_PTR(&g_mono_structs, sd);   /* register before recursing, to break cycles */
        for (int i = 0; i < gs->fields.count; i++) {
            Field *src = vec_get(&gs->fields, i);
            Field *dst = vec_push(&sd->fields);
            dst->name = src->name;
            dst->type = subst_type(src->type, &subst);
            request_type(dst->type, line);
        }
        return;
    }
    EnumDecl *ge = find_enum(t->name);
    if (ge) {
        Vec subst = make_subst(&ge->type_params, &t->args, "enum", t->name, line);
        EnumDecl *ed = calloc(1, sizeof(EnumDecl));
        ed->name = ge->name;
        ed->mangled = mangled;
        vec_init(&ed->type_params, sizeof(char *));
        vec_init(&ed->variants, sizeof(Variant));
        VEC_PUSH_PTR(&g_mono_enums, ed);
        for (int i = 0; i < ge->variants.count; i++) {
            Variant *src = vec_get(&ge->variants, i);
            Variant *dst = vec_push(&ed->variants);
            dst->name = src->name;
            vec_init(&dst->payload, sizeof(Type *));
            for (int j = 0; j < src->payload.count; j++) {
                Type *pt = subst_type(VEC_PTR(&src->payload, j, Type), &subst);
                VEC_PUSH_PTR(&dst->payload, pt);
                request_type(pt, line);
            }
        }
        return;
    }
    fail(line, "unknown type '%s'", t->name);
}

/* Queue a function instantiation; returns its mangled name. */
static char *request_fn(FnDecl *generic, const Vec *type_args, int line) {
    SB sb; sb_init(&sb);
    sb_append(&sb, generic->name);
    for (int i = 0; i < type_args->count; i++) {
        sb_append(&sb, "_");
        ty_mangle_into(VEC_PTR(type_args, i, Type), &sb);
    }
    char *mangled = sb.data;
    /* `main` is emitted as klang_main; the real C main is a wrapper that anchors
       the collector's stack scan below every Klang frame. */
    if (strcmp(generic->name, "main") == 0) mangled = strdup("klang_main");
    if (find_mono_fn(mangled)) return mangled;

    if (g_mono_fns.count > MAX_INSTANCES)
        fail(line, "too many generic function instantiations — is a generic function "
                   "calling itself with an ever-growing type?");

    Vec subst = make_subst(&generic->type_params, type_args, "function", generic->name, line);
    FnDecl *fd = calloc(1, sizeof(FnDecl));
    fd->name = generic->name;
    fd->mangled = mangled;
    vec_init(&fd->type_params, sizeof(char *));
    vec_init(&fd->params, sizeof(Field));
    for (int i = 0; i < generic->params.count; i++) {
        Field *src = vec_get(&generic->params, i);
        Field *dst = vec_push(&fd->params);
        dst->name = src->name;
        dst->is_mut = src->is_mut;
        dst->type = subst_type(src->type, &subst);
        request_type(dst->type, line);
    }
    fd->ret_type = subst_type(generic->ret_type, &subst);
    request_type(fd->ret_type, line);
    fd->body = clone_stmts(&generic->body, &subst);

    VEC_PUSH_PTR(&g_mono_fns, fd);
    VEC_PUSH_PTR(&g_fn_queue, fd);
    return mangled;
}

/* ───────────────────────── scopes ───────────────────────── */

typedef struct { char *name; Type *type; bool is_mut; } Var;
typedef struct { Vec scopes; } Scope;

static void scope_init(Scope *sc) { vec_init(&sc->scopes, sizeof(Vec)); }
static void scope_push(Scope *sc) { Vec *v = vec_push(&sc->scopes); vec_init(v, sizeof(Var)); }
static void scope_pop(Scope *sc) { sc->scopes.count--; }
static void scope_declare(Scope *sc, const char *name, Type *type, bool is_mut) {
    Vec *top = vec_get(&sc->scopes, sc->scopes.count - 1);
    Var *v = vec_push(top);
    v->name = strdup(name); v->type = type; v->is_mut = is_mut;
}
static Var *scope_lookup(Scope *sc, const char *name) {
    for (int i = sc->scopes.count - 1; i >= 0; i--) {
        Vec *scope = vec_get(&sc->scopes, i);
        for (int j = scope->count - 1; j >= 0; j--) {
            Var *v = vec_get(scope, j);
            if (strcmp(v->name, name) == 0) return v;
        }
    }
    return NULL;
}

/* ───────────────────────── typecheck ───────────────────────── */

static Type *g_cur_ret;  /* return type of the function being checked (for `?`) */

static Type *tc_expr(Expr *e, Scope *sc, Type *expected);
static void tc_block(Vec *body, Scope *sc);
static void tc_args(Vec *wants, Vec *args, Vec *labels, Vec *subst, Scope *sc);
static char *labelf(const char *fmt, ...);

static bool ty_numeric(const Type *t) { return t->kind == TY_INT || t->kind == TY_FLOAT; }

static Type *tc_binary(Expr *e, Scope *sc) {
    Type *lt = tc_expr(e->lhs, sc, NULL);
    Type *rt = tc_expr(e->rhs, sc, NULL);
    const char *op = e->op;
    if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
        if (lt->kind != TY_BOOL || rt->kind != TY_BOOL)
            fail(e->line, "operator '%s' needs bool operands, got %s and %s", op, ty_str(lt), ty_str(rt));
        return ty_bool();
    }
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
        if (!ty_eq(lt, rt)) fail(e->line, "cannot compare %s with %s", ty_str(lt), ty_str(rt));
        if (lt->kind == TY_NAMED)
            fail(e->line, "'%s' is not comparable with '%s' yet — use 'match' instead", ty_str(lt), op);
        return ty_bool();
    }
    if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
        if (!ty_numeric(lt) || !ty_numeric(rt) || !ty_eq(lt, rt))
            fail(e->line, "operator '%s' needs matching numeric operands, got %s and %s", op, ty_str(lt), ty_str(rt));
        return ty_bool();
    }
    if (strcmp(op, "+") == 0 && lt->kind == TY_STRING && rt->kind == TY_STRING) return ty_string();
    if (!ty_numeric(lt) || !ty_numeric(rt) || !ty_eq(lt, rt))
        fail(e->line, "operator '%s' needs matching numeric operands (or string '+'), got %s and %s",
             op, ty_str(lt), ty_str(rt));
    return lt;
}

/* `Some(x)`, `None`, `Ok(v)`, `Circle(r)` — enum determined by variant name, with the
   expected type steering the generic arguments when the payload can't pin them down. */
static Type *tc_variant(Expr *e, Scope *sc, Type *expected, EnumDecl *ge) {
    int vi = variant_index(ge, e->sval);
    Variant *gv = vec_get(&ge->variants, vi);
    if (e->args.count != gv->payload.count)
        fail(e->line, "variant '%s' takes %d value(s), got %d", e->sval, gv->payload.count, e->args.count);

    Vec subst; vec_init(&subst, sizeof(Binding));
    if (expected && expected->kind == TY_NAMED && strcmp(expected->name, ge->name) == 0 &&
        expected->args.count == ge->type_params.count) {
        for (int i = 0; i < ge->type_params.count; i++) {
            Binding *b = vec_push(&subst);
            b->name = VEC_PTR(&ge->type_params, i, char);
            b->type = VEC_PTR(&expected->args, i, Type);
        }
    }
    Vec labels; vec_init(&labels, sizeof(char *));
    for (int i = 0; i < e->args.count; i++)
        VEC_PUSH_PTR(&labels, labelf("variant '%s' value %d", e->sval, i + 1));
    tc_args(&gv->payload, &e->args, &labels, &subst, sc);

    Type *result = ty_named(ge->name);
    for (int i = 0; i < ge->type_params.count; i++) {
        char *tp = VEC_PTR(&ge->type_params, i, char);
        Type *bound = NULL;
        for (int j = 0; j < subst.count; j++) {
            Binding *b = vec_get(&subst, j);
            if (strcmp(b->name, tp) == 0) { bound = b->type; break; }
        }
        if (!bound)
            fail(e->line, "cannot infer type '%s' of %s here — add a type annotation, "
                          "e.g. 'let x: %s<...> = ...'", tp, ge->name, ge->name);
        VEC_PUSH_PTR(&result->args, bound);
    }
    e->kind = EX_VARIANT;
    return result;
}

static bool type_mentions_var(const Type *t, const char *var) {
    if (t->kind == TY_VAR) return strcmp(t->name, var) == 0;
    for (int i = 0; i < t->args.count; i++)
        if (type_mentions_var(VEC_PTR(&t->args, i, Type), var)) return true;
    return false;
}
static EnumDecl *find_enum_by_variant_quiet(const char *vname) {
    EnumDecl *found = NULL;
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind != DECL_ENUM) continue;
        if (variant_index(d->e, vname) < 0) continue;
        if (found) return NULL;
        found = d->e;
    }
    return found;
}
/* True when this expression cannot determine its own type — a variant like `None`
   whose payload doesn't mention every type parameter of its enum. */
static bool needs_expected(Expr *e, Scope *sc) {
    if (e->kind != EX_CALL && e->kind != EX_IDENT) return false;
    if (e->kind == EX_IDENT && scope_lookup(sc, e->sval)) return false;
    EnumDecl *ge = find_enum_by_variant_quiet(e->sval);
    if (!ge || ge->type_params.count == 0) return false;
    Variant *v = vec_get(&ge->variants, variant_index(ge, e->sval));
    for (int i = 0; i < ge->type_params.count; i++) {
        char *tp = VEC_PTR(&ge->type_params, i, char);
        bool mentioned = false;
        for (int j = 0; j < v->payload.count; j++)
            if (type_mentions_var(VEC_PTR(&v->payload, j, Type), tp)) { mentioned = true; break; }
        if (!mentioned) return true;
    }
    return false;
}

/* Check arguments against (possibly generic) expected types, inferring type
   arguments as we go. Self-describing arguments are checked first so that e.g.
   `first_or(None, "default")` can learn T from the second argument. */
static void tc_args(Vec *wants, Vec *args, Vec *labels, Vec *subst, Scope *sc) {
    int n = args->count;
    bool *done = calloc((size_t)n, sizeof(bool));
    int remaining = n;
    while (remaining > 0) {
        bool progress = false;
        for (int i = 0; i < n; i++) {
            if (done[i]) continue;
            Type *want = subst_type(VEC_PTR(wants, i, Type), subst);
            if (ty_has_var(want)) continue;
            Expr *arg = VEC_PTR(args, i, Expr);
            Type *got = tc_expr(arg, sc, want);
            if (!unify(VEC_PTR(wants, i, Type), got, subst))
                fail(arg->line, "%s: expected %s, got %s",
                     VEC_PTR(labels, i, char), ty_str(want), ty_str(got));
            done[i] = true; remaining--; progress = true;
        }
        if (remaining == 0) break;
        if (progress) continue;
        int pick = -1;
        for (int i = 0; i < n && pick < 0; i++)
            if (!done[i] && !needs_expected(VEC_PTR(args, i, Expr), sc)) pick = i;
        if (pick < 0) for (int i = 0; i < n && pick < 0; i++) if (!done[i]) pick = i;
        Expr *arg = VEC_PTR(args, pick, Expr);
        Type *want = subst_type(VEC_PTR(wants, pick, Type), subst);
        Type *got = tc_expr(arg, sc, ty_has_var(want) ? NULL : want);
        if (!unify(VEC_PTR(wants, pick, Type), got, subst))
            fail(arg->line, "%s: expected %s, got %s",
                 VEC_PTR(labels, pick, char), ty_str(want), ty_str(got));
        done[pick] = true; remaining--;
    }
    free(done);
}

static char *labelf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* Walk to the variable an lvalue is rooted at, so we can check it was declared `mut`. */
static Var *lvalue_root(Expr *e, Scope *sc) {
    while (e->kind == EX_FIELD || e->kind == EX_INDEX) e = e->lhs;
    if (e->kind != EX_IDENT) return NULL;
    return scope_lookup(sc, e->sval);
}

static Type *tc_call(Expr *e, Scope *sc, Type *expected) {
    if (strcmp(e->sval, "println") == 0 || strcmp(e->sval, "print") == 0) {
        if (e->args.count != 1) fail(e->line, "'%s' takes exactly 1 argument", e->sval);
        tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        return ty_void();
    }
    if (strcmp(e->sval, "assert") == 0) {
        if (e->args.count != 2)
            fail(e->line, "'assert' takes 2 arguments: the condition and a message");
        Type *ct = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, ty_bool());
        if (ct->kind != TY_BOOL) fail(e->line, "'assert' needs a bool condition, got %s", ty_str(ct));
        Type *mt = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, ty_string());
        if (mt->kind != TY_STRING) fail(e->line, "'assert' needs a string message, got %s", ty_str(mt));
        return ty_void();
    }
    if (strcmp(e->sval, "gc_collect") == 0) {
        if (e->args.count != 0) fail(e->line, "'gc_collect' takes no arguments");
        return ty_void();
    }
    if (strcmp(e->sval, "gc_heap") == 0) {
        if (e->args.count != 0) fail(e->line, "'gc_heap' takes no arguments");
        return ty_int();
    }
    if (strcmp(e->sval, "len") == 0) {
        if (e->args.count != 1) fail(e->line, "'len' takes exactly 1 argument");
        Type *at = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        if (at->kind != TY_ARRAY && at->kind != TY_STRING)
            fail(e->line, "'len' needs an array or string, got %s", ty_str(at));
        return ty_int();
    }
    if (strcmp(e->sval, "push") == 0) {
        if (e->args.count != 2) fail(e->line, "'push' takes exactly 2 arguments: the array and the value");
        Expr *arr = VEC_PTR(&e->args, 0, Expr);
        Type *at = tc_expr(arr, sc, NULL);
        if (at->kind != TY_ARRAY) fail(e->line, "'push' needs an array, got %s", ty_str(at));
        Var *root = lvalue_root(arr, sc);
        if (!root)
            fail(e->line, "'push' needs a variable to push into");
        if (!root->is_mut)
            fail(e->line, "cannot push to '%s' because it is immutable "
                          "(declare it 'let mut' to allow this)", root->name);
        Type *vt = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, ty_elem(at));
        if (!ty_eq(vt, ty_elem(at)))
            fail(e->line, "cannot push %s into %s", ty_str(vt), ty_str(at));
        return ty_void();
    }
    if (strcmp(e->sval, "to_string") == 0) {
        if (e->args.count != 1) fail(e->line, "'to_string' takes exactly 1 argument");
        Type *at = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        if (at->kind == TY_NAMED)
            fail(e->line, "'to_string' does not support %s — match on it and build the string yourself", ty_str(at));
        if (at->kind == TY_VOID) fail(e->line, "'to_string' needs a value, got void");
        return ty_string();
    }
    EnumDecl *ge = find_enum_by_variant(e->sval, e->line);
    if (ge) return tc_variant(e, sc, expected, ge);

    FnDecl *fn = find_fn(e->sval);
    if (!fn) fail(e->line, "call to undefined function '%s'", e->sval);
    if (e->args.count != fn->params.count)
        fail(e->line, "'%s' expects %d argument(s), got %d", e->sval, fn->params.count, e->args.count);

    Vec subst; vec_init(&subst, sizeof(Binding));
    Vec wants; vec_init(&wants, sizeof(Type *));
    Vec labels; vec_init(&labels, sizeof(char *));
    for (int i = 0; i < fn->params.count; i++) {
        VEC_PUSH_PTR(&wants, ((Field *)vec_get(&fn->params, i))->type);
        VEC_PUSH_PTR(&labels, labelf("argument %d to '%s'", i + 1, e->sval));
    }
    tc_args(&wants, &e->args, &labels, &subst, sc);

    Vec type_args; vec_init(&type_args, sizeof(Type *));
    for (int i = 0; i < fn->type_params.count; i++) {
        char *tp = VEC_PTR(&fn->type_params, i, char);
        Type *bound = NULL;
        for (int j = 0; j < subst.count; j++) {
            Binding *b = vec_get(&subst, j);
            if (strcmp(b->name, tp) == 0) { bound = b->type; break; }
        }
        if (!bound) fail(e->line, "cannot infer type '%s' for call to '%s'", tp, e->sval);
        VEC_PUSH_PTR(&type_args, bound);
    }
    e->resolved = request_fn(fn, &type_args, e->line);
    return subst_type(fn->ret_type, &subst);
}

static Type *tc_struct_lit(Expr *e, Scope *sc, Type *expected) {
    StructDecl *gs = find_struct(e->sval);
    if (!gs) fail(e->line, "unknown struct '%s'", e->sval);
    if (e->fields.count != gs->fields.count)
        fail(e->line, "struct '%s' has %d field(s), got %d", e->sval, gs->fields.count, e->fields.count);

    Vec subst; vec_init(&subst, sizeof(Binding));
    if (expected && expected->kind == TY_NAMED && strcmp(expected->name, gs->name) == 0 &&
        expected->args.count == gs->type_params.count) {
        for (int i = 0; i < gs->type_params.count; i++) {
            Binding *b = vec_push(&subst);
            b->name = VEC_PTR(&gs->type_params, i, char);
            b->type = VEC_PTR(&expected->args, i, Type);
        }
    }
    Vec wants; vec_init(&wants, sizeof(Type *));
    Vec vals; vec_init(&vals, sizeof(Expr *));
    Vec labels; vec_init(&labels, sizeof(char *));
    for (int i = 0; i < e->fields.count; i++) {
        FieldInit *fi = vec_get(&e->fields, i);
        Field *df = NULL;
        for (int j = 0; j < gs->fields.count; j++) {
            Field *f = vec_get(&gs->fields, j);
            if (strcmp(f->name, fi->name) == 0) { df = f; break; }
        }
        if (!df) fail(e->line, "struct '%s' has no field '%s'", e->sval, fi->name);
        VEC_PUSH_PTR(&wants, df->type);
        VEC_PUSH_PTR(&vals, fi->value);
        VEC_PUSH_PTR(&labels, labelf("field '%s'", fi->name));
    }
    tc_args(&wants, &vals, &labels, &subst, sc);

    Type *result = ty_named(gs->name);
    for (int i = 0; i < gs->type_params.count; i++) {
        char *tp = VEC_PTR(&gs->type_params, i, char);
        Type *bound = NULL;
        for (int j = 0; j < subst.count; j++) {
            Binding *b = vec_get(&subst, j);
            if (strcmp(b->name, tp) == 0) { bound = b->type; break; }
        }
        if (!bound) fail(e->line, "cannot infer type '%s' of struct '%s'", tp, gs->name);
        VEC_PUSH_PTR(&result->args, bound);
    }
    return result;
}

static Type *tc_match(Expr *e, Scope *sc, Type *expected) {
    Type *st = tc_expr(e->lhs, sc, NULL);
    if (st->kind != TY_NAMED) fail(e->lhs->line, "can only match on an enum, got %s", ty_str(st));
    char *mangled = ty_mangle(st);
    EnumDecl *ed = find_mono_enum(mangled);
    if (!ed) fail(e->lhs->line, "can only match on an enum, got %s", ty_str(st));
    if (e->arms.count == 0) fail(e->line, "match needs at least one arm");

    bool *covered = calloc((size_t)ed->variants.count, sizeof(bool));
    bool has_wildcard = false;
    Type *result = NULL;
    bool any_block = false;

    for (int i = 0; i < e->arms.count; i++) {
        MatchArm *arm = vec_get(&e->arms, i);
        scope_push(sc);
        if (arm->pat.variant) {
            int vi = variant_index(ed, arm->pat.variant);
            if (vi < 0) fail(arm->pat.line, "'%s' has no variant '%s'", ty_str(st), arm->pat.variant);
            if (covered[vi]) fail(arm->pat.line, "variant '%s' is already handled above", arm->pat.variant);
            covered[vi] = true;
            Variant *v = vec_get(&ed->variants, vi);
            if (arm->pat.binds.count != v->payload.count)
                fail(arm->pat.line, "variant '%s' carries %d value(s), pattern binds %d",
                     arm->pat.variant, v->payload.count, arm->pat.binds.count);
            for (int j = 0; j < arm->pat.binds.count; j++)
                scope_declare(sc, VEC_PTR(&arm->pat.binds, j, char), VEC_PTR(&v->payload, j, Type), false);
        } else {
            if (has_wildcard) fail(arm->pat.line, "duplicate '_' arm");
            has_wildcard = true;
        }
        if (arm->is_block) {
            any_block = true;
            tc_block(&arm->body, sc);
        } else {
            Type *at = tc_expr(arm->value, sc, expected);
            if (!result) result = at;
            else if (!ty_eq(result, at))
                fail(arm->value->line, "match arms disagree: earlier arm gives %s, this one gives %s",
                     ty_str(result), ty_str(at));
        }
        scope_pop(sc);
    }
    if (!has_wildcard) {
        SB missing; sb_init(&missing);
        int n = 0;
        for (int i = 0; i < ed->variants.count; i++) {
            if (covered[i]) continue;
            if (n++) sb_append(&missing, ", ");
            sb_append(&missing, ((Variant *)vec_get(&ed->variants, i))->name);
        }
        if (n) fail(e->line, "match on %s is not exhaustive — missing: %s (add those arms, or a '_' arm)",
                    ty_str(st), missing.data);
    }
    free(covered);
    if (any_block) return ty_void();
    return result ? result : ty_void();
}

static Type *tc_try(Expr *e, Scope *sc) {
    Type *it = tc_expr(e->lhs, sc, NULL);
    if (it->kind != TY_NAMED || it->args.count == 0 ||
        (strcmp(it->name, "Result") != 0 && strcmp(it->name, "Option") != 0))
        fail(e->line, "'?' works on Result or Option, got %s", ty_str(it));
    if (!g_cur_ret || g_cur_ret->kind != TY_NAMED || strcmp(g_cur_ret->name, it->name) != 0)
        fail(e->line, "'?' on %s requires the enclosing function to return %s too, but it returns %s",
             ty_str(it), it->name, g_cur_ret ? ty_str(g_cur_ret) : "nothing");
    if (strcmp(it->name, "Result") == 0) {
        Type *ie = VEC_PTR(&it->args, 1, Type);
        Type *oe = VEC_PTR(&g_cur_ret->args, 1, Type);
        if (!ty_eq(ie, oe))
            fail(e->line, "'?' error type mismatch: this gives %s but the function returns errors of %s",
                 ty_str(ie), ty_str(oe));
    }
    return VEC_PTR(&it->args, 0, Type);
}

static Type *tc_expr(Expr *e, Scope *sc, Type *expected) {
    Type *t;
    switch (e->kind) {
        case EX_INT: t = ty_int(); break;
        case EX_FLOAT: t = ty_float(); break;
        case EX_BOOL: t = ty_bool(); break;
        case EX_STRING: t = ty_string(); break;
        case EX_IDENT: {
            Var *v = scope_lookup(sc, e->sval);
            if (v) { t = v->type; break; }
            EnumDecl *ge = find_enum_by_variant(e->sval, e->line);
            if (ge) { t = tc_variant(e, sc, expected, ge); break; }
            fail(e->line, "undefined variable '%s'", e->sval);
            return NULL;
        }
        case EX_UNARY: {
            Type *inner = tc_expr(e->lhs, sc, NULL);
            if (strcmp(e->op, "!") == 0) {
                if (inner->kind != TY_BOOL) fail(e->line, "'!' needs a bool operand, got %s", ty_str(inner));
                t = ty_bool();
            } else {
                if (!ty_numeric(inner)) fail(e->line, "unary '-' needs a numeric operand, got %s", ty_str(inner));
                t = inner;
            }
            break;
        }
        case EX_BINARY: t = tc_binary(e, sc); break;
        case EX_FIELD: {
            Type *base = tc_expr(e->lhs, sc, NULL);
            if (base->kind != TY_NAMED) fail(e->line, "'.%s' used on %s, which is not a struct", e->sval, ty_str(base));
            char *mangled = ty_mangle(base);
            StructDecl *sd = find_mono_struct(mangled);
            if (!sd) fail(e->line, "'.%s' used on %s, which is not a struct", e->sval, ty_str(base));
            Field *found = NULL;
            for (int i = 0; i < sd->fields.count; i++) {
                Field *f = vec_get(&sd->fields, i);
                if (strcmp(f->name, e->sval) == 0) { found = f; break; }
            }
            if (!found) fail(e->line, "%s has no field '%s'", ty_str(base), e->sval);
            t = found->type;
            break;
        }
        case EX_STRUCT_LIT: t = tc_struct_lit(e, sc, expected); break;
        case EX_CALL: t = tc_call(e, sc, expected); break;
        case EX_MATCH: t = tc_match(e, sc, expected); break;
        case EX_TRY: t = tc_try(e, sc); break;
        case EX_ARRAY_LIT: {
            Type *want = expected && expected->kind == TY_ARRAY ? ty_elem(expected) : NULL;
            if (e->args.count == 0) {
                if (!want)
                    fail(e->line, "cannot tell what this empty array holds — "
                                  "annotate it, as in 'let xs: [int] = []'");
                t = ty_array(want);
                break;
            }
            Type *elem = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, want);
            if (elem->kind == TY_VOID) fail(e->line, "an array cannot hold void values");
            for (int i = 1; i < e->args.count; i++) {
                Expr *it = VEC_PTR(&e->args, i, Expr);
                Type *at = tc_expr(it, sc, elem);
                if (!ty_eq(at, elem))
                    fail(it->line, "array element %d is %s but the first is %s — "
                                   "every element must have the same type",
                         i + 1, ty_str(at), ty_str(elem));
            }
            t = ty_array(elem);
            break;
        }
        case EX_INDEX: {
            Type *base = tc_expr(e->lhs, sc, NULL);
            if (base->kind != TY_ARRAY)
                fail(e->line, "cannot index %s — only arrays support '[...]'", ty_str(base));
            Type *ix = tc_expr(e->rhs, sc, ty_int());
            if (ix->kind != TY_INT) fail(e->rhs->line, "an array index must be int, got %s", ty_str(ix));
            t = ty_elem(base);
            break;
        }
        default: t = ty_unknown(); break;
    }
    if (t->kind == TY_NAMED || t->kind == TY_ARRAY) request_type(t, e->line);
    e->type = t;
    return t;
}

static void tc_stmt(Stmt *s, Scope *sc) {
    switch (s->kind) {
        case ST_LET: {
            if (s->has_type) request_type(s->decl_type, s->line);
            Type *vt = tc_expr(s->expr, sc, s->has_type ? s->decl_type : NULL);
            if (vt->kind == TY_VOID) fail(s->line, "'%s' cannot hold a void value", s->name);
            if (s->has_type) {
                if (!ty_eq(vt, s->decl_type))
                    fail(s->line, "'%s' is declared %s but initialized with %s",
                         s->name, ty_str(s->decl_type), ty_str(vt));
            } else s->decl_type = vt;
            scope_declare(sc, s->name, s->decl_type, s->is_mut);
            break;
        }
        case ST_ASSIGN: {
            Expr *tgt = s->target;
            if (tgt->kind != EX_IDENT && tgt->kind != EX_FIELD && tgt->kind != EX_INDEX)
                fail(s->line, "this is not something you can assign to");
            Var *root = lvalue_root(tgt, sc);
            if (!root) {
                if (tgt->kind == EX_IDENT) fail(s->line, "undefined variable '%s'", tgt->sval);
                fail(s->line, "this is not something you can assign to");
            }
            if (!root->is_mut)
                fail(s->line, "cannot assign to '%s' because it is immutable "
                              "(declare it 'let mut' to allow this)", root->name);
            Type *cur = tc_expr(tgt, sc, NULL);
            Type *rt = tc_expr(s->expr, sc, cur);
            if (!ty_eq(cur, rt)) fail(s->line, "cannot assign %s to a target of type %s", ty_str(rt), ty_str(cur));
            break;
        }
        case ST_IF:
            for (int i = 0; i < s->cond_blocks.count; i++) {
                CondBlock *cb = vec_get(&s->cond_blocks, i);
                if (cb->cond) {
                    Type *ct = tc_expr(cb->cond, sc, NULL);
                    if (ct->kind != TY_BOOL) fail(cb->cond->line, "if condition must be bool, got %s", ty_str(ct));
                }
                tc_block(&cb->body, sc);
            }
            break;
        case ST_WHILE: {
            Type *ct = tc_expr(s->expr, sc, NULL);
            if (ct->kind != TY_BOOL) fail(s->expr->line, "while condition must be bool, got %s", ty_str(ct));
            tc_block(&s->body, sc);
            break;
        }
        case ST_FOR: {
            Type *bound;
            if (s->is_range) {
                Type *lo = tc_expr(s->expr, sc, ty_int());
                Type *hi = tc_expr(s->expr2, sc, ty_int());
                if (lo->kind != TY_INT || hi->kind != TY_INT)
                    fail(s->line, "a '..' range needs int bounds, got %s..%s", ty_str(lo), ty_str(hi));
                bound = ty_int();
            } else {
                Type *it = tc_expr(s->expr, sc, NULL);
                if (it->kind != TY_ARRAY)
                    fail(s->expr->line, "'for ... in' needs an array or an 'a..b' range, got %s", ty_str(it));
                bound = ty_elem(it);
            }
            /* The loop variable is rebound each iteration, so it is never `mut`. */
            scope_push(sc);
            scope_declare(sc, s->name, bound, false);
            tc_block(&s->body, sc);
            scope_pop(sc);
            break;
        }
        case ST_RETURN:
            if (s->expr) {
                if (g_cur_ret->kind == TY_VOID) fail(s->line, "this function returns nothing, but 'return' has a value");
                Type *rt = tc_expr(s->expr, sc, g_cur_ret);
                if (!ty_eq(rt, g_cur_ret))
                    fail(s->line, "returning %s but the function declares %s", ty_str(rt), ty_str(g_cur_ret));
            } else if (g_cur_ret->kind != TY_VOID) {
                fail(s->line, "the function returns %s but 'return' has no value", ty_str(g_cur_ret));
            }
            break;
        case ST_EXPR: tc_expr(s->expr, sc, NULL); break;
        case ST_BLOCK: tc_block(&s->body, sc); break;
    }
}

static void tc_block(Vec *body, Scope *sc) {
    scope_push(sc);
    for (int i = 0; i < body->count; i++) tc_stmt(VEC_PTR(body, i, Stmt), sc);
    scope_pop(sc);
}

static void tc_fn(FnDecl *fd) {
    Scope sc; scope_init(&sc); scope_push(&sc);
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        scope_declare(&sc, pm->name, pm->type, pm->is_mut);
    }
    g_cur_ret = fd->ret_type;
    tc_block(&fd->body, &sc);
    scope_pop(&sc);
}

static void monomorphize_and_check(void) {
    FnDecl *main_fn = find_fn("main");
    if (!main_fn) fail(0, "no 'main' function defined");
    if (main_fn->params.count != 0) fail(0, "'main' must take no arguments");
    if (main_fn->ret_type->kind != TY_VOID) fail(0, "'main' must not declare a return type");
    if (main_fn->type_params.count != 0) fail(0, "'main' must not be generic");

    /* Seed with every non-generic function so unused ones are still checked. */
    Vec empty; vec_init(&empty, sizeof(Type *));
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_FN && d->f->type_params.count == 0) request_fn(d->f, &empty, 0);
    }
    /* Worklist: checking a body can queue more instantiations. */
    while (g_fn_queue.count > 0) {
        FnDecl *fd = VEC_PTR(&g_fn_queue, g_fn_queue.count - 1, FnDecl);
        g_fn_queue.count--;
        tc_fn(fd);
    }
}

/* ───────────────────────── codegen ───────────────────────── */

static int g_tmp = 0;
static Type *g_cg_ret;

static const char *c_type(const Type *t) {
    switch (t->kind) {
        case TY_VOID: return "void";
        case TY_INT: return "int64_t";
        case TY_FLOAT: return "double";
        case TY_BOOL: return "bool";
        case TY_STRING: return "char*";
        case TY_NAMED: case TY_ARRAY: return ty_mangle(t);
        default: return "void*";
    }
}
static char *fresh_tmp(void) {
    char *s = malloc(24);
    snprintf(s, 24, "_k%d", g_tmp++);
    return s;
}
static void indent_to(SB *sb, int n) { for (int i = 0; i < n; i++) sb_append(sb, "    "); }

static void cg_expr(Expr *e, SB *out, SB *pre, int ind);
static void cg_stmts(Vec *body, SB *sb, int ind);

/* Emit a variant constructor value: (Option_int){ .tag = ..., .data.Some._0 = ... } */
static void cg_variant_value(const Type *enum_ty, const char *variant, Vec *arg_exprs,
                             SB *out, SB *pre, int ind) {
    char *m = ty_mangle(enum_ty);
    sb_appendf(out, "(%s){ .tag = %s_TAG_%s", m, m, variant);
    for (int i = 0; i < arg_exprs->count; i++) {
        sb_appendf(out, ", .data.%s._%d = ", variant, i);
        cg_expr(VEC_PTR(arg_exprs, i, Expr), out, pre, ind);
    }
    sb_append(out, " }");
}

/* Lower a match. If result_var is non-NULL each expression arm assigns into it. */
static void cg_match(Expr *e, SB *pre, int ind, const char *result_var) {
    char *scrut = fresh_tmp();
    SB scrut_pre; sb_init(&scrut_pre);
    SB scrut_val; sb_init(&scrut_val);
    cg_expr(e->lhs, &scrut_val, &scrut_pre, ind);
    sb_append(pre, scrut_pre.data);
    indent_to(pre, ind);
    sb_appendf(pre, "%s %s = %s;\n", c_type(e->lhs->type), scrut, scrut_val.data);

    char *em = ty_mangle(e->lhs->type);
    EnumDecl *ed = find_mono_enum(em);

    /* Exhaustiveness is already proven, but C can't see that. Emitting the final arm
       as `default:` keeps the compiler from warning that the result may be unset. */
    int default_idx = -1;
    for (int i = 0; i < e->arms.count; i++)
        if (!((MatchArm *)vec_get(&e->arms, i))->pat.variant) default_idx = i;
    if (default_idx < 0) default_idx = e->arms.count - 1;

    indent_to(pre, ind);
    sb_appendf(pre, "switch (%s.tag) {\n", scrut);
    for (int i = 0; i < e->arms.count; i++) {
        MatchArm *arm = vec_get(&e->arms, i);
        indent_to(pre, ind + 1);
        if (i == default_idx) sb_append(pre, "default: {\n");
        else sb_appendf(pre, "case %s_TAG_%s: {\n", em, arm->pat.variant);

        if (arm->pat.variant) {
            int vi = variant_index(ed, arm->pat.variant);
            Variant *v = vec_get(&ed->variants, vi);
            for (int j = 0; j < arm->pat.binds.count; j++) {
                const char *bind = VEC_PTR(&arm->pat.binds, j, char);
                indent_to(pre, ind + 2);
                sb_appendf(pre, "%s %s = %s.data.%s._%d; (void)%s;\n",
                           c_type(VEC_PTR(&v->payload, j, Type)),
                           bind, scrut, arm->pat.variant, j, bind);
            }
        }
        if (arm->is_block) {
            cg_stmts(&arm->body, pre, ind + 2);
        } else {
            SB val_pre; sb_init(&val_pre);
            SB val; sb_init(&val);
            cg_expr(arm->value, &val, &val_pre, ind + 2);
            sb_append(pre, val_pre.data);
            indent_to(pre, ind + 2);
            if (result_var) sb_appendf(pre, "%s = %s;\n", result_var, val.data);
            else if (arm->value->type->kind != TY_VOID) sb_appendf(pre, "(void)(%s);\n", val.data);
            else sb_appendf(pre, "%s;\n", val.data);
        }
        indent_to(pre, ind + 2);
        sb_append(pre, "break;\n");
        indent_to(pre, ind + 1);
        sb_append(pre, "}\n");
    }
    indent_to(pre, ind);
    sb_append(pre, "}\n");
}

static void cg_try(Expr *e, SB *out, SB *pre, int ind) {
    SB inner_pre; sb_init(&inner_pre);
    SB inner; sb_init(&inner);
    cg_expr(e->lhs, &inner, &inner_pre, ind);
    sb_append(pre, inner_pre.data);

    Type *it = e->lhs->type;
    char *im = ty_mangle(it);
    char *tmp = fresh_tmp();
    indent_to(pre, ind);
    sb_appendf(pre, "%s %s = %s;\n", im, tmp, inner.data);

    char *om = ty_mangle(g_cg_ret);
    bool is_result = strcmp(it->name, "Result") == 0;
    const char *bad = is_result ? "Err" : "None";
    indent_to(pre, ind);
    sb_appendf(pre, "if (%s.tag == %s_TAG_%s) {\n", tmp, im, bad);
    indent_to(pre, ind + 1);
    if (is_result)
        sb_appendf(pre, "return (%s){ .tag = %s_TAG_Err, .data.Err._0 = %s.data.Err._0 };\n", om, om, tmp);
    else
        sb_appendf(pre, "return (%s){ .tag = %s_TAG_None };\n", om, om);
    indent_to(pre, ind);
    sb_append(pre, "}\n");

    sb_appendf(out, "%s.data.%s._0", tmp, is_result ? "Ok" : "Some");
}

static void cg_expr(Expr *e, SB *out, SB *pre, int ind) {
    switch (e->kind) {
        case EX_INT: sb_appendf(out, "INT64_C(%lld)", (long long)e->ival); break;
        case EX_FLOAT: sb_appendf(out, "%.17g", e->fval); break;
        case EX_BOOL: sb_append(out, e->bval ? "true" : "false"); break;
        case EX_STRING:
            sb_append(out, "\"");
            for (const char *p = e->sval; *p; p++) {
                if (*p == '"' || *p == '\\') sb_appendf(out, "\\%c", *p);
                else if (*p == '\n') sb_append(out, "\\n");
                else if (*p == '\t') sb_append(out, "\\t");
                else { char b[2] = {*p, 0}; sb_append(out, b); }
            }
            sb_append(out, "\"");
            break;
        case EX_IDENT: sb_append(out, e->sval); break;
        case EX_VARIANT: cg_variant_value(e->type, e->sval, &e->args, out, pre, ind); break;
        case EX_UNARY:
            sb_appendf(out, "(%s", e->op);
            cg_expr(e->lhs, out, pre, ind);
            sb_append(out, ")");
            break;
        case EX_BINARY:
            if (strcmp(e->op, "+") == 0 && e->lhs->type->kind == TY_STRING) {
                sb_append(out, "klang_str_concat(");
                cg_expr(e->lhs, out, pre, ind);
                sb_append(out, ", ");
                cg_expr(e->rhs, out, pre, ind);
                sb_append(out, ")");
            } else if (strcmp(e->op, "==") == 0 && e->lhs->type->kind == TY_STRING) {
                sb_append(out, "klang_str_eq(");
                cg_expr(e->lhs, out, pre, ind);
                sb_append(out, ", ");
                cg_expr(e->rhs, out, pre, ind);
                sb_append(out, ")");
            } else if (strcmp(e->op, "!=") == 0 && e->lhs->type->kind == TY_STRING) {
                sb_append(out, "(!klang_str_eq(");
                cg_expr(e->lhs, out, pre, ind);
                sb_append(out, ", ");
                cg_expr(e->rhs, out, pre, ind);
                sb_append(out, "))");
            } else {
                sb_append(out, "(");
                cg_expr(e->lhs, out, pre, ind);
                sb_appendf(out, " %s ", e->op);
                cg_expr(e->rhs, out, pre, ind);
                sb_append(out, ")");
            }
            break;
        case EX_FIELD:
            sb_append(out, "(");
            cg_expr(e->lhs, out, pre, ind);
            sb_appendf(out, ").%s", e->sval);
            break;
        case EX_STRUCT_LIT: {
            sb_appendf(out, "(%s){ ", ty_mangle(e->type));
            for (int i = 0; i < e->fields.count; i++) {
                FieldInit *fi = vec_get(&e->fields, i);
                if (i) sb_append(out, ", ");
                sb_appendf(out, ".%s = ", fi->name);
                cg_expr(fi->value, out, pre, ind);
            }
            sb_append(out, " }");
            break;
        }
        case EX_MATCH: {
            char *tmp = fresh_tmp();
            indent_to(pre, ind);
            sb_appendf(pre, "%s %s;\n", c_type(e->type), tmp);
            cg_match(e, pre, ind, tmp);
            sb_append(out, tmp);
            break;
        }
        case EX_ARRAY_LIT: {
            char *m = ty_mangle(e->type);
            char *tmp = fresh_tmp();
            indent_to(pre, ind);
            sb_appendf(pre, "%s %s = %s_new(%d);\n", m, tmp, m, e->args.count);
            for (int i = 0; i < e->args.count; i++) {
                SB val; sb_init(&val);
                cg_expr(VEC_PTR(&e->args, i, Expr), &val, pre, ind);
                indent_to(pre, ind);
                sb_appendf(pre, "%s->data[%d] = %s;\n", tmp, i, val.data);
            }
            sb_append(out, tmp);
            break;
        }
        case EX_INDEX: {
            sb_appendf(out, "(*%s_at(", ty_mangle(e->lhs->type));
            cg_expr(e->lhs, out, pre, ind);
            sb_append(out, ", ");
            cg_expr(e->rhs, out, pre, ind);
            sb_append(out, "))");
            break;
        }
        case EX_TRY: cg_try(e, out, pre, ind); break;
        case EX_CALL: {
            if (strcmp(e->sval, "println") == 0 || strcmp(e->sval, "print") == 0) {
                Expr *arg = VEC_PTR(&e->args, 0, Expr);
                const char *fmt;
                switch (arg->type->kind) {
                    case TY_INT: fmt = "%\" PRId64 \""; break;
                    case TY_FLOAT: fmt = "%g"; break;
                    case TY_BOOL: case TY_STRING: fmt = "%s"; break;
                    default: fail(arg->line, "cannot print a value of type %s", ty_str(arg->type)); return;
                }
                sb_appendf(out, "printf(\"%s%s\", ", fmt, strcmp(e->sval, "println") == 0 ? "\\n" : "");
                if (arg->type->kind == TY_BOOL) {
                    sb_append(out, "(");
                    cg_expr(arg, out, pre, ind);
                    sb_append(out, ") ? \"true\" : \"false\"");
                } else cg_expr(arg, out, pre, ind);
                sb_append(out, ")");
                break;
            }
            if (strcmp(e->sval, "assert") == 0) {
                sb_append(out, "klang_assert(");
                cg_expr(VEC_PTR(&e->args, 0, Expr), out, pre, ind);
                sb_append(out, ", ");
                cg_expr(VEC_PTR(&e->args, 1, Expr), out, pre, ind);
                sb_append(out, ")");
                break;
            }
            if (strcmp(e->sval, "gc_collect") == 0) { sb_append(out, "klang_gc_collect()"); break; }
            if (strcmp(e->sval, "gc_heap") == 0) { sb_append(out, "klang_gc_heap()"); break; }
            if (strcmp(e->sval, "len") == 0) {
                Expr *arg = VEC_PTR(&e->args, 0, Expr);
                if (arg->type->kind == TY_STRING) {
                    sb_append(out, "(int64_t)strlen(");
                    cg_expr(arg, out, pre, ind);
                    sb_append(out, ")");
                } else {
                    sb_append(out, "(");
                    cg_expr(arg, out, pre, ind);
                    sb_append(out, ")->len");
                }
                break;
            }
            if (strcmp(e->sval, "push") == 0) {
                Expr *arr = VEC_PTR(&e->args, 0, Expr);
                sb_appendf(out, "%s_push(", ty_mangle(arr->type));
                cg_expr(arr, out, pre, ind);
                sb_append(out, ", ");
                cg_expr(VEC_PTR(&e->args, 1, Expr), out, pre, ind);
                sb_append(out, ")");
                break;
            }
            if (strcmp(e->sval, "to_string") == 0) {
                Expr *arg = VEC_PTR(&e->args, 0, Expr);
                switch (arg->type->kind) {
                    case TY_INT: sb_append(out, "klang_int_to_string("); break;
                    case TY_FLOAT: sb_append(out, "klang_float_to_string("); break;
                    case TY_BOOL: sb_append(out, "klang_bool_to_string("); break;
                    case TY_STRING: sb_append(out, "klang_strdup("); break;
                    default: fail(arg->line, "cannot convert %s to string", ty_str(arg->type)); return;
                }
                cg_expr(arg, out, pre, ind);
                sb_append(out, ")");
                break;
            }
            sb_appendf(out, "%s(", e->resolved ? e->resolved : e->sval);
            for (int i = 0; i < e->args.count; i++) {
                if (i) sb_append(out, ", ");
                cg_expr(VEC_PTR(&e->args, i, Expr), out, pre, ind);
            }
            sb_append(out, ")");
            break;
        }
    }
}

/* Emit `pre` statements then the finished line. */
static void flush(SB *sb, SB *pre) { sb_append(sb, pre->data); }

static void cg_stmt(Stmt *s, SB *sb, int ind) {
    SB pre; sb_init(&pre);
    SB line; sb_init(&line);
    switch (s->kind) {
        case ST_LET:
            cg_expr(s->expr, &line, &pre, ind);
            flush(sb, &pre);
            indent_to(sb, ind);
            sb_appendf(sb, "%s %s = %s;\n", c_type(s->decl_type), s->name, line.data);
            break;
        case ST_ASSIGN: {
            SB tgt; sb_init(&tgt);
            cg_expr(s->target, &tgt, &pre, ind);
            cg_expr(s->expr, &line, &pre, ind);
            flush(sb, &pre);
            indent_to(sb, ind);
            sb_appendf(sb, "%s = %s;\n", tgt.data, line.data);
            break;
        }
        case ST_IF: {
            /* Conditions may need pre-statements, so lower to nested if/else blocks. */
            for (int i = 0; i < s->cond_blocks.count; i++) {
                CondBlock *cb = vec_get(&s->cond_blocks, i);
                int level = ind + i;
                if (cb->cond) {
                    SB cpre; sb_init(&cpre);
                    SB cval; sb_init(&cval);
                    cg_expr(cb->cond, &cval, &cpre, level);
                    sb_append(sb, cpre.data);
                    indent_to(sb, level);
                    sb_appendf(sb, "if (%s) {\n", cval.data);
                } else {
                    indent_to(sb, level);
                    sb_append(sb, "{\n");
                }
                cg_stmts(&cb->body, sb, level + 1);
                indent_to(sb, level);
                sb_append(sb, "}");
                if (i + 1 < s->cond_blocks.count) sb_append(sb, " else {\n");
                else sb_append(sb, "\n");
            }
            for (int i = s->cond_blocks.count - 2; i >= 0; i--) {
                indent_to(sb, ind + i);
                sb_append(sb, "}\n");
            }
            break;
        }
        case ST_WHILE: {
            SB cpre; sb_init(&cpre);
            SB cval; sb_init(&cval);
            cg_expr(s->expr, &cval, &cpre, ind + 1);
            if (cpre.len == 0) {
                indent_to(sb, ind);
                sb_appendf(sb, "while (%s) {\n", cval.data);
                cg_stmts(&s->body, sb, ind + 1);
                indent_to(sb, ind);
                sb_append(sb, "}\n");
            } else {
                /* Condition needs statements: while (1) { <pre> if (!c) break; ... } */
                indent_to(sb, ind);
                sb_append(sb, "while (1) {\n");
                sb_append(sb, cpre.data);
                indent_to(sb, ind + 1);
                sb_appendf(sb, "if (!(%s)) break;\n", cval.data);
                cg_stmts(&s->body, sb, ind + 1);
                indent_to(sb, ind);
                sb_append(sb, "}\n");
            }
            break;
        }
        case ST_FOR: {
            /* Bounds and the array are evaluated once, before the loop. */
            char *idx = fresh_tmp();
            if (s->is_range) {
                SB lo; sb_init(&lo);
                SB hi; sb_init(&hi);
                cg_expr(s->expr, &lo, &pre, ind);
                cg_expr(s->expr2, &hi, &pre, ind);
                char *end = fresh_tmp();
                flush(sb, &pre);
                indent_to(sb, ind);
                sb_appendf(sb, "int64_t %s = %s;\n", end, hi.data);
                indent_to(sb, ind);
                sb_appendf(sb, "for (int64_t %s = %s; %s < %s; %s++) {\n", idx, lo.data, idx, end, idx);
                indent_to(sb, ind + 1);
                sb_appendf(sb, "int64_t %s = %s; (void)%s;\n", s->name, idx, s->name);
            } else {
                SB arr; sb_init(&arr);
                cg_expr(s->expr, &arr, &pre, ind);
                char *av = fresh_tmp();
                flush(sb, &pre);
                indent_to(sb, ind);
                sb_appendf(sb, "%s %s = %s;\n", c_type(s->expr->type), av, arr.data);
                indent_to(sb, ind);
                sb_appendf(sb, "for (int64_t %s = 0; %s < %s->len; %s++) {\n", idx, idx, av, idx);
                indent_to(sb, ind + 1);
                sb_appendf(sb, "%s %s = %s->data[%s]; (void)%s;\n",
                           c_type(ty_elem(s->expr->type)), s->name, av, idx, s->name);
            }
            cg_stmts(&s->body, sb, ind + 1);
            indent_to(sb, ind);
            sb_append(sb, "}\n");
            break;
        }
        case ST_RETURN:
            if (s->expr) {
                cg_expr(s->expr, &line, &pre, ind);
                flush(sb, &pre);
                indent_to(sb, ind);
                sb_appendf(sb, "return %s;\n", line.data);
            } else {
                indent_to(sb, ind);
                sb_append(sb, "return;\n");
            }
            break;
        case ST_EXPR:
            if (s->expr->kind == EX_MATCH) {   /* statement-position match needs no temp */
                cg_match(s->expr, &pre, ind, NULL);
                flush(sb, &pre);
                break;
            }
            cg_expr(s->expr, &line, &pre, ind);
            flush(sb, &pre);
            indent_to(sb, ind);
            sb_appendf(sb, "%s;\n", line.data);
            break;
        case ST_BLOCK:
            indent_to(sb, ind);
            sb_append(sb, "{\n");
            cg_stmts(&s->body, sb, ind + 1);
            indent_to(sb, ind);
            sb_append(sb, "}\n");
            break;
    }
}

static void cg_stmts(Vec *body, SB *sb, int ind) {
    for (int i = 0; i < body->count; i++) cg_stmt(VEC_PTR(body, i, Stmt), sb, ind);
}

/* Klang's garbage collector, emitted into every program.
 *
 * Conservative mark-sweep. "Conservative" means the collector does not need the
 * compiler to tell it where the live pointers are: it scans the machine stack and
 * callee-saved registers, and treats any word that happens to point at one of our
 * objects as a reference. The cost is that an integer that looks like a pointer can
 * keep an object alive a little longer than necessary. The payoff is that it needs
 * no cooperation from generated code — every C local, every compiler temporary, and
 * every value the optimizer parked in a register is covered automatically, with no
 * shadow stack to keep in sync and nothing to get wrong at an early `return`.
 *
 * String literals live in .rodata rather than the heap, so they simply never appear
 * in the object set and are ignored — no interning pass needed.
 */
static const char *GC_RUNTIME =
    "/* ── Klang GC: conservative mark-sweep ─────────────────────────────── */\n"
    "typedef struct KObj { struct KObj *next; size_t size; unsigned char mark; } KObj;\n"
    "#define KPAY(o) ((void *)((char *)(o) + sizeof(KObj)))\n"
    "#define KHDR(p) ((KObj *)((char *)(p) - sizeof(KObj)))\n"
    "\n"
    "KObj *k_objs = NULL;            /* every live object, newest first */\n"
    "void **k_tab = NULL;            /* open-addressed set of payload addresses */\n"
    "size_t k_tab_cap = 0, k_tab_n = 0;\n"
    "char *k_lo = NULL, *k_hi = NULL;   /* payload address range, for a fast reject */\n"
    "size_t k_live = 0, k_limit = 4u << 20;\n"
    "void *k_stack_bottom = NULL;\n"
    "KObj **k_gray = NULL; size_t k_gray_n = 0, k_gray_cap = 0;\n"
    "size_t k_collections = 0;\n"
    "\n"
    "void k_oom(void) { fprintf(stderr, \"klang: out of memory\\n\"); exit(1); }\n"
    "\n"
    "size_t k_hash(void *p) {\n"
    "    uintptr_t x = (uintptr_t)p;\n"
    "    x ^= x >> 33; x *= (uintptr_t)0xff51afd7ed558ccdULL;\n"
    "    x ^= x >> 29; x *= (uintptr_t)0xc4ceb9fe1a85ec53ULL;\n"
    "    x ^= x >> 32;\n"
    "    return (size_t)x;\n"
    "}\n"
    "void k_tab_put(void **tab, size_t cap, void *p) {\n"
    "    size_t i = k_hash(p) & (cap - 1);\n"
    "    while (tab[i]) i = (i + 1) & (cap - 1);\n"
    "    tab[i] = p;\n"
    "}\n"
    "void k_tab_resize(size_t cap) {\n"
    "    void **nt = calloc(cap, sizeof(void *));\n"
    "    if (!nt) k_oom();\n"
    "    for (size_t i = 0; i < k_tab_cap; i++) if (k_tab[i]) k_tab_put(nt, cap, k_tab[i]);\n"
    "    free(k_tab); k_tab = nt; k_tab_cap = cap;\n"
    "}\n"
    "void k_tab_add(void *p) {\n"
    "    if ((k_tab_n + 1) * 10 >= k_tab_cap * 7) k_tab_resize(k_tab_cap ? k_tab_cap * 2 : 1024);\n"
    "    k_tab_put(k_tab, k_tab_cap, p); k_tab_n++;\n"
    "}\n"
    "int k_tab_has(void *p) {\n"
    "    if (!k_tab_cap) return 0;\n"
    "    size_t i = k_hash(p) & (k_tab_cap - 1);\n"
    "    while (k_tab[i]) { if (k_tab[i] == p) return 1; i = (i + 1) & (k_tab_cap - 1); }\n"
    "    return 0;\n"
    "}\n"
    "\n"
    "void k_gray_push(KObj *o) {\n"
    "    if (k_gray_n == k_gray_cap) {\n"
    "        k_gray_cap = k_gray_cap ? k_gray_cap * 2 : 256;\n"
    "        k_gray = realloc(k_gray, k_gray_cap * sizeof(KObj *));\n"
    "        if (!k_gray) k_oom();\n"
    "    }\n"
    "    k_gray[k_gray_n++] = o;\n"
    "}\n"
    "/* A word is a reference only if it is exactly the payload address of a live\n"
    "   object. Interior pointers are not followed, and never arise: the only pointer\n"
    "   we store into an object is an array's data buffer, kept at its base. */\n"
    "void k_mark(void *p) {\n"
    "    if ((char *)p < k_lo || (char *)p > k_hi) return;\n"
    "    if (!k_tab_has(p)) return;\n"
    "    KObj *o = KHDR(p);\n"
    "    if (o->mark) return;\n"
    "    o->mark = 1;\n"
    "    k_gray_push(o);\n"
    "}\n"
    "void k_scan(void *lo, void *hi) {\n"
    "    char *a = (char *)lo, *b = (char *)hi;\n"
    "    if (a > b) { char *t = a; a = b; b = t; }\n"
    "    while ((uintptr_t)a % sizeof(void *)) a++;\n"
    "    for (; a + sizeof(void *) <= b; a += sizeof(void *)) {\n"
    "        void *cand;\n"
    "        memcpy(&cand, a, sizeof cand);\n"
    "        k_mark(cand);\n"
    "    }\n"
    "}\n"
    "\n"
    "void klang_gc_collect(void) {\n"
    "    if (!k_stack_bottom) return;   /* before main() set the anchor */\n"
    "    jmp_buf regs;\n"
    "    memset(&regs, 0, sizeof regs);\n"
    "    setjmp(regs);                  /* force callee-saved registers onto the stack */\n"
    "    for (KObj *o = k_objs; o; o = o->next) o->mark = 0;\n"
    "    k_gray_n = 0;\n"
    "    k_scan(&regs, (char *)&regs + sizeof regs);\n"
    "    k_scan((void *)&regs, k_stack_bottom);\n"
    "    while (k_gray_n) {\n"
    "        KObj *o = k_gray[--k_gray_n];\n"
    "        void *pl = KPAY(o);\n"
    "        k_scan(pl, (char *)pl + o->size);\n"
    "    }\n"
    "    KObj **link = &k_objs;\n"
    "    size_t live = 0, n = 0;\n"
    "    while (*link) {\n"
    "        KObj *o = *link;\n"
    "        if (o->mark) { live += o->size; n++; link = &o->next; }\n"
    "        else { *link = o->next; free(o); }\n"
    "    }\n"
    "    /* Rebuild the address set from the survivors — cheaper and simpler than\n"
    "       deleting from an open-addressed table one entry at a time. */\n"
    "    size_t cap = 1024;\n"
    "    while (cap * 7 <= (n + 1) * 10) cap *= 2;\n"
    "    free(k_tab); k_tab = calloc(cap, sizeof(void *));\n"
    "    if (!k_tab) k_oom();\n"
    "    k_tab_cap = cap; k_tab_n = 0;\n"
    "    k_lo = NULL; k_hi = NULL;\n"
    "    for (KObj *o = k_objs; o; o = o->next) {\n"
    "        void *pl = KPAY(o);\n"
    "        k_tab_put(k_tab, k_tab_cap, pl); k_tab_n++;\n"
    "        if (!k_lo || (char *)pl < k_lo) k_lo = pl;\n"
    "        if (!k_hi || (char *)pl > k_hi) k_hi = pl;\n"
    "    }\n"
    "    k_live = live;\n"
    "    k_limit = live * 2 < (4u << 20) ? (4u << 20) : live * 2;\n"
    "    k_collections++;\n"
    "}\n"
    "\n"
    "void *klang_gc_alloc(size_t n) {\n"
    "    if (k_live + n > k_limit) klang_gc_collect();\n"
    "    KObj *o = malloc(sizeof(KObj) + n);\n"
    "    if (!o) { klang_gc_collect(); o = malloc(sizeof(KObj) + n); if (!o) k_oom(); }\n"
    "    o->size = n; o->mark = 0; o->next = k_objs; k_objs = o;\n"
    "    void *pl = KPAY(o);\n"
    "    if (!k_lo || (char *)pl < k_lo) k_lo = pl;\n"
    "    if (!k_hi || (char *)pl > k_hi) k_hi = pl;\n"
    "    k_tab_add(pl);\n"
    "    k_live += n;\n"
    "    return pl;\n"
    "}\n"
    "void *klang_gc_grow(void *p, size_t n) {\n"
    "    void *q = klang_gc_alloc(n);\n"
    "    if (p) { size_t old = KHDR(p)->size; memcpy(q, p, old < n ? old : n); }\n"
    "    return q;\n"
    "}\n"
    "void klang_gc_init(void *bottom) { k_stack_bottom = bottom; }\n"
    "int64_t klang_gc_heap(void) { return (int64_t)k_live; }\n"
    "\n";

static const char *RUNTIME =
    /* Non-static so an unused helper doesn't trip -Wunused-function in the output. */
    "/* ── Klang runtime ─────────────────────────────────────────────────── */\n"
    "char *klang_strdup(const char *s) { size_t n = strlen(s) + 1; char *r = klang_gc_alloc(n); memcpy(r, s, n); return r; }\n"
    "char *klang_str_concat(const char *a, const char *b) {\n"
    "    size_t la = strlen(a), lb = strlen(b);\n"
    "    char *r = klang_gc_alloc(la + lb + 1);\n"
    "    memcpy(r, a, la); memcpy(r + la, b, lb + 1);\n"
    "    return r;\n"
    "}\n"
    "bool klang_str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; }\n"
    "char *klang_int_to_string(int64_t v) { char *r = klang_gc_alloc(24); snprintf(r, 24, \"%\" PRId64 \"\", v); return r; }\n"
    "char *klang_float_to_string(double v) { char *r = klang_gc_alloc(32); snprintf(r, 32, \"%g\", v); return r; }\n"
    "char *klang_bool_to_string(bool v) { return klang_strdup(v ? \"true\" : \"false\"); }\n"
    "void klang_assert(bool ok, const char *msg) {\n"
    "    if (!ok) { fprintf(stderr, \"klang: assertion failed: %s\\n\", msg); exit(1); }\n"
    "}\n"
    "void klang_bounds(int64_t i, int64_t len) {\n"
    "    fprintf(stderr, \"klang: index %\" PRId64 \" is out of bounds for an array of length %\" PRId64 \"\\n\", i, len);\n"
    "    exit(1);\n"
    "}\n";

/* Arrays are heap objects behind a pointer, so pushing through one binding is
   visible through every other binding — no surprise copies. */
static void emit_array(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    const char *e = c_type(ty_elem(t));
    sb_appendf(sb, "typedef struct %s_s { int64_t len; int64_t cap; %s *data; } *%s;\n", m, e, m);
    /* The header is allocated first and kept in a local, so it is visible to the
       collector on the stack while the data buffer is being allocated. */
    sb_appendf(sb, "%s %s_new(int64_t n) {\n"
                   "    %s a = klang_gc_alloc(sizeof(struct %s_s));\n"
                   "    a->len = n; a->cap = n < 4 ? 4 : n; a->data = NULL;\n"
                   "    a->data = klang_gc_alloc(sizeof(%s) * (size_t)a->cap);\n"
                   "    return a;\n}\n", m, m, m, m, e);
    sb_appendf(sb, "%s *%s_at(%s a, int64_t i) {\n"
                   "    if (i < 0 || i >= a->len) klang_bounds(i, a->len);\n"
                   "    return &a->data[i];\n}\n", e, m, m);
    sb_appendf(sb, "void %s_push(%s a, %s v) {\n"
                   "    if (a->len == a->cap) {\n"
                   "        int64_t ncap = a->cap * 2;\n"
                   "        %s *nd = klang_gc_grow(a->data, sizeof(%s) * (size_t)ncap);\n"
                   "        a->data = nd; a->cap = ncap;\n"
                   "    }\n"
                   "    a->data[a->len++] = v;\n}\n\n", m, m, e, e, e);
}

/* Collect the mangled names of named types a mono type embeds by value. */
static void collect_deps(const Type *t, Vec *out) {
    if (t->kind != TY_NAMED && t->kind != TY_ARRAY) return;
    VEC_PUSH_PTR(out, ty_mangle(t));
}

static void emit_struct(StructDecl *sd, SB *sb) {
    sb_appendf(sb, "typedef struct %s {\n", sd->mangled);
    for (int i = 0; i < sd->fields.count; i++) {
        Field *f = vec_get(&sd->fields, i);
        sb_appendf(sb, "    %s %s;\n", c_type(f->type), f->name);
    }
    if (sd->fields.count == 0) sb_append(sb, "    char _empty;\n");
    sb_appendf(sb, "} %s;\n\n", sd->mangled);
}

static void emit_enum(EnumDecl *ed, SB *sb) {
    sb_append(sb, "enum {");
    for (int i = 0; i < ed->variants.count; i++) {
        Variant *v = vec_get(&ed->variants, i);
        sb_appendf(sb, "%s %s_TAG_%s = %d", i ? "," : "", ed->mangled, v->name, i);
    }
    sb_append(sb, " };\n");
    sb_appendf(sb, "typedef struct %s {\n    int tag;\n", ed->mangled);
    bool any_payload = false;
    for (int i = 0; i < ed->variants.count; i++)
        if (((Variant *)vec_get(&ed->variants, i))->payload.count > 0) any_payload = true;
    if (any_payload) {
        sb_append(sb, "    union {\n");
        for (int i = 0; i < ed->variants.count; i++) {
            Variant *v = vec_get(&ed->variants, i);
            if (v->payload.count == 0) continue;
            sb_append(sb, "        struct {");
            for (int j = 0; j < v->payload.count; j++)
                sb_appendf(sb, " %s _%d;", c_type(VEC_PTR(&v->payload, j, Type)), j);
            sb_appendf(sb, " } %s;\n", v->name);
        }
        sb_append(sb, "    } data;\n");
    }
    sb_appendf(sb, "} %s;\n\n", ed->mangled);
}

/* Types embed each other by value, so emit them in dependency order. */
static void emit_types(SB *sb) {
    int n_s = g_mono_structs.count, n_e = g_mono_enums.count, n_a = g_mono_arrays.count;
    int total = n_s + n_e + n_a;
    bool *done = calloc((size_t)total, sizeof(bool));
    Vec emitted; vec_init(&emitted, sizeof(char *));

    for (int pass = 0; pass < total + 1; pass++) {
        bool progress = false;
        for (int i = 0; i < total; i++) {
            if (done[i]) continue;
            StructDecl *sd = i < n_s ? VEC_PTR(&g_mono_structs, i, StructDecl) : NULL;
            EnumDecl *ed = (!sd && i < n_s + n_e) ? VEC_PTR(&g_mono_enums, i - n_s, EnumDecl) : NULL;
            Type *at = (!sd && !ed) ? VEC_PTR(&g_mono_arrays, i - n_s - n_e, Type) : NULL;
            const char *mangled = sd ? sd->mangled : ed ? ed->mangled : ty_mangle(at);

            Vec deps; vec_init(&deps, sizeof(char *));
            if (sd) {
                for (int j = 0; j < sd->fields.count; j++)
                    collect_deps(((Field *)vec_get(&sd->fields, j))->type, &deps);
            } else if (ed) {
                for (int j = 0; j < ed->variants.count; j++) {
                    Variant *v = vec_get(&ed->variants, j);
                    for (int k = 0; k < v->payload.count; k++)
                        collect_deps(VEC_PTR(&v->payload, k, Type), &deps);
                }
            } else {
                /* An array stores its elements inline, so the element type must be complete. */
                collect_deps(ty_elem(at), &deps);
            }
            bool ready = true;
            for (int j = 0; j < deps.count && ready; j++) {
                char *dep = VEC_PTR(&deps, j, char);
                if (strcmp(dep, mangled) == 0) {
                    fail(0, "type '%s' contains itself by value — recursive types need indirection, "
                            "which Klang does not have yet", mangled);
                }
                bool found = false;
                for (int k = 0; k < emitted.count; k++)
                    if (strcmp(VEC_PTR(&emitted, k, char), dep) == 0) { found = true; break; }
                if (!found) ready = false;
            }
            if (!ready) continue;

            if (sd) emit_struct(sd, sb);
            else if (ed) emit_enum(ed, sb);
            else emit_array(at, sb);
            VEC_PUSH_PTR(&emitted, (char *)mangled);
            done[i] = true;
            progress = true;
        }
        if (!progress) break;
    }
    for (int i = 0; i < total; i++) {
        if (done[i]) continue;
        const char *m = i < n_s ? VEC_PTR(&g_mono_structs, i, StructDecl)->mangled
                      : i < n_s + n_e ? VEC_PTR(&g_mono_enums, i - n_s, EnumDecl)->mangled
                      : ty_mangle(VEC_PTR(&g_mono_arrays, i - n_s - n_e, Type));
        fail(0, "type '%s' is part of a cycle — recursive types need indirection, "
                "which Klang does not have yet", m);
    }
    free(done);
}

static void codegen(SB *sb) {
    sb_append(sb, "/* Generated by klangc 0.4 — do not edit by hand */\n");
    sb_append(sb, "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n");
    sb_append(sb, "#include <stdbool.h>\n#include <stdint.h>\n#include <inttypes.h>\n");
    sb_append(sb, "#include <stddef.h>\n#include <setjmp.h>\n\n");
    sb_append(sb, GC_RUNTIME);
    sb_append(sb, RUNTIME);
    sb_append(sb, "\n");

    emit_types(sb);

    for (int i = 0; i < g_mono_fns.count; i++) {
        FnDecl *fd = VEC_PTR(&g_mono_fns, i, FnDecl);
        bool is_main = strcmp(fd->name, "main") == 0;
        sb_appendf(sb, "%s %s(", is_main ? "int" : c_type(fd->ret_type), fd->mangled);
        for (int j = 0; j < fd->params.count; j++) {
            Field *pm = vec_get(&fd->params, j);
            if (j) sb_append(sb, ", ");
            sb_appendf(sb, "%s %s", c_type(pm->type), pm->name);
        }
        if (fd->params.count == 0) sb_append(sb, "void");
        sb_append(sb, ");\n");
    }
    sb_append(sb, "\n");

    for (int i = 0; i < g_mono_fns.count; i++) {
        FnDecl *fd = VEC_PTR(&g_mono_fns, i, FnDecl);
        bool is_main = strcmp(fd->name, "main") == 0;
        g_cg_ret = fd->ret_type;
        sb_appendf(sb, "%s %s(", is_main ? "int" : c_type(fd->ret_type), fd->mangled);
        for (int j = 0; j < fd->params.count; j++) {
            Field *pm = vec_get(&fd->params, j);
            if (j) sb_append(sb, ", ");
            sb_appendf(sb, "%s %s", c_type(pm->type), pm->name);
        }
        if (fd->params.count == 0) sb_append(sb, "void");
        sb_append(sb, ") {\n");
        cg_stmts(&fd->body, sb, 1);
        if (is_main) sb_append(sb, "    return 0;\n");
        sb_append(sb, "}\n\n");
    }

    /* The collector scans from its own frame up to this anchor. Taking the anchor
       here — in a frame that strictly encloses klang_main — guarantees every Klang
       local lies inside that range, whatever order the C compiler lays frames out. */
    sb_append(sb, "int main(void) {\n");
    sb_append(sb, "    int _kanchor = 0;\n");
    sb_append(sb, "    klang_gc_init(&_kanchor);\n");
    sb_append(sb, "    return klang_main();\n");
    sb_append(sb, "}\n");
}

/* ───────────────────────── driver ───────────────────────── */

static const char *PRELUDE =
    "enum Option<T> { None, Some(T) }\n"
    "enum Result<T, E> { Ok(T), Err(E) }\n";

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "klangc: cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("klangc — Klang compiler (0.4)\n\n"
               "Usage:\n"
               "  klangc <file.kkg>                Compile to output.c\n"
               "  klangc <file.kkg> -o <out.c>     Compile to a specific output file\n"
               "  klangc --version                 Show version\n"
               "  klangc --help                    Show this help\n");
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) { printf("klangc 0.4.0\n"); return 0; }

    const char *in_path = argv[1];
    const char *out_path = "output.c";
    if (argc >= 4 && strcmp(argv[2], "-o") == 0) out_path = argv[3];

    vec_init(&g_decls, sizeof(Decl));
    vec_init(&g_mono_structs, sizeof(StructDecl *));
    vec_init(&g_mono_enums, sizeof(EnumDecl *));
    vec_init(&g_mono_fns, sizeof(FnDecl *));
    vec_init(&g_mono_arrays, sizeof(Type *));
    vec_init(&g_fn_queue, sizeof(FnDecl *));

    Parser pre;
    g_filename = "<prelude>";
    parser_init(&pre, PRELUDE);
    parse_program(&pre);

    g_filename = in_path;
    char *src = read_file(in_path);
    Parser p;
    parser_init(&p, src);
    parse_program(&p);

    monomorphize_and_check();

    SB out; sb_init(&out);
    codegen(&out);

    FILE *f = fopen(out_path, "wb");
    if (!f) { fprintf(stderr, "klangc: cannot write '%s'\n", out_path); return 1; }
    fwrite(out.data, 1, (size_t)out.len, f);
    fclose(f);

    printf("klangc: compiled '%s' -> '%s'\n", in_path, out_path);
    return 0;
}
