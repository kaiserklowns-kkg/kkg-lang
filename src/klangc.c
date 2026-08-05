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

/* Every file that has been read, so an error can show the line it happened on. */
typedef struct { const char *path; const char *src; } SourceFile;
static SourceFile g_sources[64];
static int g_nsources = 0;

static void remember_source(const char *path, const char *src) {
    if (g_nsources < (int)(sizeof g_sources / sizeof g_sources[0])) {
        g_sources[g_nsources].path = path;
        g_sources[g_nsources].src = src;
        g_nsources++;
    }
}
static const char *source_of(const char *path) {
    for (int i = 0; i < g_nsources; i++)
        if (strcmp(g_sources[i].path, path) == 0) return g_sources[i].src;
    return NULL;
}

/* Print line `want` of `src` without its newline, and without trailing spaces. */
static void print_source_line(const char *src, int want) {
    int line = 1;
    const char *p = src;
    while (line < want && *p) { if (*p == '\n') line++; p++; }
    if (line != want) return;
    const char *end = p;
    while (*end && *end != '\n') end++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    fprintf(stderr, "  %4d | %.*s\n", want, (int)(end - p), p);
}

/* Diagnostics read as: what went wrong, where, and what to do about it. A message
   may carry the last part after an em dash, which is printed as a separate `help:`
   line rather than run on to a paragraph the reader has to parse. */
static void fail(int line, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    char *help = strstr(buf, " — ");
    if (help) { *help = 0; help += strlen(" — "); }

    if (line > 0) fprintf(stderr, "%s:%d: error: %s\n", g_filename, line, buf);
    else fprintf(stderr, "%s: error: %s\n", g_filename, buf);

    const char *src = source_of(g_filename);
    if (src && line > 0) print_source_line(src, line);
    if (help) fprintf(stderr, "       | help: %s\n", help);
    exit(1);
}

/* ───────────────────────── lexer ───────────────────────── */

typedef enum {
    TK_EOF, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING,
    TK_TRUE, TK_FALSE, TK_LET, TK_MUT, TK_FN, TK_STRUCT, TK_ENUM, TK_MATCH,
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_IN, TK_RETURN, TK_PUB, TK_IMPORT, TK_AS,
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_COLON, TK_ARROW, TK_FATARROW, TK_DOT, TK_DOTDOT, TK_QUESTION,
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ANDAND, TK_OROR, TK_NOT, TK_PIPE,
    TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ,
    TK_BREAK, TK_CONTINUE, TK_CONST, TK_EXTERN, TK_UNSAFE, TK_TYPE, TK_SPAWN, TK_AWAIT
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
        if (c == '/' && lx_peek2(lx) == '*') {
            int start = lx->line, depth = 0;
            while (lx_peek(lx) != -1) {
                if (lx_peek(lx) == '/' && lx_peek2(lx) == '*') { lx_adv(lx); lx_adv(lx); depth++; continue; }
                if (lx_peek(lx) == '*' && lx_peek2(lx) == '/') {
                    lx_adv(lx); lx_adv(lx);
                    if (--depth == 0) break;
                    continue;
                }
                lx_adv(lx);
            }
            if (depth != 0) fail(start, "unterminated '/*' comment");
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
            {"pub", TK_PUB}, {"import", TK_IMPORT}, {"as", TK_AS},
            {"break", TK_BREAK}, {"continue", TK_CONTINUE}, {"const", TK_CONST},
            {"extern", TK_EXTERN}, {"unsafe", TK_UNSAFE}, {"type", TK_TYPE},
            {"spawn", TK_SPAWN}, {"await", TK_AWAIT},
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
                    case 'r': buf[0] = '\r'; break;
                    case 't': buf[0] = '\t'; break;
                    case '0': buf[0] = '\0'; break;
                    case '"': buf[0] = '"'; break;
                    case '\'': buf[0] = '\''; break;
                    case '$': buf[0] = '$'; break;
                    case '\\': buf[0] = '\\'; break;
                    /* Silently dropping the backslash turns a typo into a wrong
                       string that looks right, so an unknown escape is an error. */
                    default:
                        fail(line, "'\\%c' is not an escape — use \\n \\r \\t \\0 \\\" \\' "
                                   "\\\\ or \\$, or write the backslash as \\\\", (char)esc);
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
    TWO('+', '=', TK_PLUSEQ)
    TWO('-', '=', TK_MINUSEQ)
    TWO('*', '=', TK_STAREQ)
    TWO('/', '=', TK_SLASHEQ)
    TWO('%', '=', TK_PERCENTEQ)
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
        case '|': return make_tok(TK_PIPE, line);
        default: fail(line, "unexpected character '%c'", c); return make_tok(TK_EOF, line);
    }
}

/* ── modules ──────────────────────────────────────────────────────────────
 * A module path is what an import names: "std/math". Two paths are reserved:
 *   ""   the prelude — Option, Result — visible from everywhere
 *   "@"  the file passed on the command line
 * A declaration's key is "<path>:<name>", so "std/math:abs" and "@:main".
 */
#define MOD_PRELUDE ""
#define MOD_ROOT    "@"
#define PRELUDE_OPTION MOD_PRELUDE ":Option"
#define PRELUDE_RESULT MOD_PRELUDE ":Result"

static char *qual_key(const char *module, const char *name) {
    size_t n = strlen(module) + strlen(name) + 2;
    char *k = malloc(n);
    snprintf(k, n, "%s:%s", module, name);
    return k;
}
/* The bare name, for messages: "std/math:abs" -> "abs" */
static const char *key_name(const char *key) {
    const char *c = strrchr(key, ':');
    return c ? c + 1 : key;
}
/* How a key should read in a diagnostic: "std/math:abs" -> "math.abs",
   and anything from the prelude or the root file as just its bare name. */
static const char *key_show(const char *key) {
    static char bufs[8][160];
    static int slot = 0;
    const char *colon = strrchr(key, ':');
    if (!colon) return key;
    size_t modlen = (size_t)(colon - key);
    if (modlen == 0 || (modlen == 1 && key[0] == '@')) return colon + 1;
    const char *seg = colon;
    while (seg > key && seg[-1] != '/') seg--;
    char *buf = bufs[slot = (slot + 1) % 8];
    snprintf(buf, sizeof bufs[0], "%.*s.%s", (int)(colon - seg), seg, colon + 1);
    return buf;
}
/* Keys carry '/' and ':', which C identifiers cannot. */
static char *key_mangle(const char *key) {
    char *r = strdup(key);
    for (char *p = r; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
    return r;
}
/* ───────────────────────── types ───────────────────────── */

typedef enum { TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_STRING, TY_NAMED, TY_ARRAY, TY_MAP,
               TY_FN, TY_TASK, TY_VAR, TY_UNKNOWN } TyKind;

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
static Type *ty_map(Type *k, Type *v) {
    Type *t = ty_new(TY_MAP);
    VEC_PUSH_PTR(&t->args, k); VEC_PUSH_PTR(&t->args, v);
    return t;
}
static Type *ty_key(const Type *t) { return VEC_PTR(&t->args, 0, Type); }
static Type *ty_val(const Type *t) { return VEC_PTR(&t->args, 1, Type); }

/* A function type keeps its parameters in args[0..n-1] and its result last. */
static Type *ty_fn(void) { return ty_new(TY_FN); }
static int ty_nparams(const Type *t) { return t->args.count - 1; }
static Type *ty_param(const Type *t, int i) { return VEC_PTR(&t->args, i, Type); }
static Type *ty_ret(const Type *t) { return VEC_PTR(&t->args, t->args.count - 1, Type); }
static Type *ty_task(Type *r) { Type *t = ty_new(TY_TASK); VEC_PUSH_PTR(&t->args, r); return t; }

static bool ty_eq(const Type *a, const Type *b) {
    if (a->kind != b->kind) return false;
    if (a->kind == TY_ARRAY || a->kind == TY_TASK) return ty_eq(ty_elem(a), ty_elem(b));
    if (a->kind == TY_MAP) return ty_eq(ty_key(a), ty_key(b)) && ty_eq(ty_val(a), ty_val(b));
    if (a->kind == TY_FN) {
        if (a->args.count != b->args.count) return false;
        for (int i = 0; i < a->args.count; i++)
            if (!ty_eq(VEC_PTR(&a->args, i, Type), VEC_PTR(&b->args, i, Type))) return false;
        return true;
    }
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
        case TY_TASK: snprintf(buf, 256, "Task<%s>", ty_str(ty_elem(t))); return buf;
        case TY_FN: {
            char ps[180] = "";
            for (int i = 0; i < ty_nparams(t); i++) {
                if (i) strncat(ps, ", ", sizeof ps - strlen(ps) - 1);
                strncat(ps, ty_str(ty_param(t, i)), sizeof ps - strlen(ps) - 1);
            }
            snprintf(buf, 256, "fn(%s) -> %s", ps, ty_str(ty_ret(t)));
            return buf;
        }
        case TY_MAP: {
            char kb[100];
            snprintf(kb, sizeof kb, "%s", ty_str(ty_key(t)));
            snprintf(buf, 256, "{%s: %s}", kb, ty_str(ty_val(t)));
            return buf;
        }
        default: break;
    }
    const char *shown = t->kind == TY_NAMED ? key_show(t->name) : t->name;
    if (t->args.count == 0) { snprintf(buf, 256, "%s", shown); return buf; }
    char inner[200] = "";
    for (int i = 0; i < t->args.count; i++) {
        if (i) strncat(inner, ", ", sizeof inner - strlen(inner) - 1);
        strncat(inner, ty_str(VEC_PTR(&t->args, i, Type)), sizeof inner - strlen(inner) - 1);
    }
    snprintf(buf, 256, "%s<%s>", shown, inner);
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
        case TY_TASK: sb_append(sb, "task_"); ty_mangle_into(ty_elem(t), sb); return;
        case TY_MAP:
            sb_append(sb, "map_"); ty_mangle_into(ty_key(t), sb);
            sb_append(sb, "_"); ty_mangle_into(ty_val(t), sb);
            return;
        case TY_FN:
            sb_append(sb, "fn");
            for (int i = 0; i < ty_nparams(t); i++) {
                sb_append(sb, "_"); ty_mangle_into(ty_param(t, i), sb);
            }
            sb_append(sb, "_to_"); ty_mangle_into(ty_ret(t), sb);
            return;
        default: break;
    }
    {
        char *m = key_mangle(t->name);   /* keys hold '/' and ':', C identifiers cannot */
        sb_append(sb, m);
        free(m);
    }
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
    EX_VARIANT, EX_MATCH, EX_IF, EX_TRY, EX_ARRAY_LIT, EX_INDEX, EX_MAP_LIT, EX_LAMBDA, EX_FNREF, EX_CONSTREF, EX_METHOD, EX_UNSAFE, EX_SPAWN, EX_AWAIT
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
    bool is_qual;    /* sval is already a fully-qualified declaration key */
    const char *mod; /* module the name was written in, for unqualified lookups */
    char *op;
    char *resolved;  /* mangled callee name after monomorphization */
    Expr *lhs, *rhs;
    Vec args;        /* Vec<Expr*> */
    Vec fields;      /* Vec<FieldInit> */
    Vec arms;        /* Vec<MatchArm> */
    Vec params;      /* EX_LAMBDA: Vec<Field> */
    Vec captures;    /* EX_LAMBDA: Vec<Field> — enclosing locals the body refers to */
    Vec body;        /* EX_LAMBDA: Vec<Stmt*> when the body is a block */
    bool is_block;   /* EX_LAMBDA: `|x| { ... }` rather than `|x| expr` */
    int lam_id;      /* EX_LAMBDA/EX_FNREF: index assigned when lifting */
};

typedef enum { ST_LET, ST_ASSIGN, ST_IF, ST_WHILE, ST_FOR, ST_RETURN, ST_EXPR, ST_BLOCK,
               ST_BREAK, ST_CONTINUE } StmtKind;

typedef struct { Expr *cond; Vec body; } CondBlock;

struct Stmt {
    StmtKind kind;
    int line;
    char *name;       /* ST_LET / ST_FOR: bound variable */
    bool is_mut;
    bool has_type;
    bool is_range;    /* ST_FOR: `for i in a..b` rather than over an array */
    bool is_unsafe;   /* ST_BLOCK: an `unsafe { ... }` block */
    Type *decl_type;
    Expr *expr;       /* ST_FOR: the array, or the range's lower bound */
    Expr *expr2;      /* ST_FOR: the range's upper bound */
    Expr *target;     /* ST_ASSIGN: lvalue being assigned to */
    Vec cond_blocks;  /* Vec<CondBlock> */
    Vec body;         /* Vec<Stmt*> */
};

typedef struct { char *name; Type *type; bool is_mut; } Field;
typedef struct { char *name; Vec payload; /* Vec<Type*> */ } Variant;

/* Every top-level declaration belongs to a module and is addressed by the key
   "<module path>:<name>" — see the module section below for what those look like.
   `name` stays the bare name, for error messages. */
typedef struct { char *name; char *key; const char *module; bool is_pub;
                 Vec type_params; Vec fields; char *mangled;
                 bool is_opaque;   /* extern type: a pointer Klang holds but never opens */
               } StructDecl;
typedef struct { char *name; char *key; const char *module; bool is_pub;
                 Vec type_params; Vec variants; char *mangled; } EnumDecl;
typedef struct {
    char *name; char *key; const char *module; const char *file; bool is_pub;
    Vec type_params; Vec params; /* Vec<Field> */
    Type *ret_type; Vec body; char *mangled;
    bool is_extern;    /* a C symbol, with no Klang body */
    bool is_unsafe;    /* callable only from an unsafe context */
    char *cname;       /* the C symbol to emit, when it differs from the name */
    char *js_body;     /* `js fn`: the JavaScript between the braces, verbatim */
    bool is_export;    /* `export fn`: reachable from JavaScript under its own name */
    int line;
} FnDecl;

/* A module-level constant. Its initializer is an ordinary expression, evaluated
   once at startup before main runs, so it may allocate. */
typedef struct {
    char *name; char *key; const char *module; const char *file; bool is_pub;
    Type *type; bool has_type; Expr *value; char *mangled; int line;
    bool is_mut;   /* `let mut` at module level: state that outlives main */
} ConstDecl;

typedef enum { DECL_STRUCT, DECL_ENUM, DECL_FN, DECL_CONST } DeclKind;
typedef struct { DeclKind kind; StructDecl *s; EnumDecl *e; FnDecl *f; ConstDecl *c; } Decl;

static Vec g_decls;  /* Vec<Decl> — generic originals, from prelude + every module */


/* ───────────────────────── parser ───────────────────────── */

typedef struct { char *alias; char *path; } Alias;

/* What each module imported, so `x.f(a)` knows which modules to search. */
typedef struct { const char *module; Vec imports; } ModuleInfo;
static Vec g_modules;

/* A cross-module reference, recorded where it is written so visibility can be
   checked once every module has been loaded. Qualified names are the only way to
   reach another module, so checking these covers every cross-module access. */
typedef struct { char *key; const char *from; const char *file; int line; } XRef;
static Vec g_xrefs;
static Vec g_c_headers;   /* Vec<char*> — #includes the generated C needs */
static Vec g_c_links;     /* Vec<char*> — libraries the build needs */

typedef struct {
    Lexer lx;
    Token cur;
    bool no_struct_lit;
    const char *module;    /* module path this file defines */
    const char *file;      /* for diagnostics */
    Vec aliases;           /* Vec<Alias> — import aliases in scope */
    Vec local_types;       /* Vec<char*> — struct/enum names declared in this file */
    Vec imports;           /* Vec<char*> — module paths this file imports */
    const Vec *tparams;    /* type parameters of the declaration being parsed */
} Parser;

static void p_advance(Parser *p) { p->cur = lex_next(&p->lx); }

/* Which type names this file declares — needed while parsing, because a type may
   be referred to above its own declaration. */
/* The body of a `js fn` is JavaScript, not Klang, so it is taken verbatim rather
   than tokenized — a semicolon alone would stop the Klang lexer dead. The scan
   tracks brace depth while skipping the three kinds of JavaScript string and both
   kinds of comment. It does not understand regex literals; braces inside one have
   to balance, which in practice they do.

   `lx` must sit just past the opening brace. On return it sits just past the
   matching close, and the text between them is the result. */
static char *take_js_body(Lexer *lx, int open_line) {
    const char *src = lx->src;
    int i = lx->pos, start = i, depth = 1, line = lx->line;
    while (depth > 0) {
        char c = src[i];
        if (!c) fail(open_line, "this JavaScript body is never closed — no matching '}'");
        if (c == '\n') { line++; i++; continue; }
        if (c == '/' && src[i + 1] == '/') {
            while (src[i] && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && src[i + 1] == '*') {
            i += 2;
            while (src[i] && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') line++; i++; }
            if (!src[i]) fail(open_line, "unterminated comment in this JavaScript body");
            i += 2;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            char q = c;
            i++;
            while (src[i] && src[i] != q) {
                if (src[i] == '\\' && src[i + 1]) i++;
                else if (src[i] == '\n') line++;
                i++;
            }
            if (!src[i]) fail(open_line, "unterminated string in this JavaScript body");
            i++;
            continue;
        }
        if (c == '{') depth++;
        if (c == '}') depth--;
        i++;
    }
    int len = i - 1 - start;      /* everything before the closing '}' */
    char *body = malloc((size_t)len + 1);
    memcpy(body, src + start, (size_t)len);
    body[len] = '\0';
    lx->pos = i;
    lx->line = line;
    return body;
}

static void prescan_types(Parser *p, const char *src) {
    Lexer lx;
    lexer_init(&lx, src);
    Token t = lex_next(&lx);
    while (t.kind != TK_EOF) {
        if (t.kind == TK_STRUCT || t.kind == TK_ENUM || t.kind == TK_TYPE) {
            Token n = lex_next(&lx);
            if (n.kind == TK_IDENT) VEC_PUSH_PTR(&p->local_types, n.text);
            t = n;
            continue;
        }
        /* Step over `js fn f(...) { ...javascript... }` — the prescan runs over the
           whole file, so it meets these bodies before the parser does. */
        if (t.kind == TK_IDENT && strcmp(t.text, "js") == 0) {
            Lexer save = lx;
            Token n = lex_next(&lx);
            if (n.kind != TK_FN) { lx = save; t = n; continue; }
            int line = n.line;
            while (n.kind != TK_LBRACE && n.kind != TK_EOF) n = lex_next(&lx);
            if (n.kind == TK_EOF) fail(line, "this 'js fn' has no JavaScript body");
            take_js_body(&lx, line);
            t = lex_next(&lx);
            continue;
        }
        t = lex_next(&lx);
    }
}

static void parser_init(Parser *p, const char *src, const char *module, const char *file) {
    lexer_init(&p->lx, src);
    p->no_struct_lit = false;
    p->module = module;
    p->file = file;
    p->tparams = NULL;
    vec_init(&p->aliases, sizeof(Alias));
    vec_init(&p->local_types, sizeof(char *));
    vec_init(&p->imports, sizeof(char *));
    prescan_types(p, src);
    p_advance(p);
}

static const char *alias_path(Parser *p, const char *name) {
    for (int i = 0; i < p->aliases.count; i++) {
        Alias *a = vec_get(&p->aliases, i);
        if (strcmp(a->alias, name) == 0) return a->path;
    }
    return NULL;
}
static bool declares_type(Parser *p, const char *name) {
    for (int i = 0; i < p->local_types.count; i++)
        if (strcmp(VEC_PTR(&p->local_types, i, char), name) == 0) return true;
    return false;
}
static bool is_tparam(Parser *p, const char *name) {
    if (!p->tparams) return false;
    for (int i = 0; i < p->tparams->count; i++)
        if (strcmp(VEC_PTR(p->tparams, i, char), name) == 0) return true;
    return false;
}
static void note_xref(Parser *p, char *key, int line) {
    XRef *x = vec_push(&g_xrefs);
    x->key = key; x->from = p->module; x->file = p->file; x->line = line;
}
/* Resolve a name written in this file to a declaration key. */
static char *resolve_written(Parser *p, const char *name, int line) {
    if (declares_type(p, name)) return qual_key(p->module, name);
    return qual_key(MOD_PRELUDE, name);
    (void)line;
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
    vec_init(&e->params, sizeof(Field));
    vec_init(&e->captures, sizeof(Field));
    vec_init(&e->body, sizeof(Stmt *));
    e->lam_id = -1;
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
    if (p_match(p, TK_LBRACE)) {          /* {string: int} */
        Type *k = parse_type(p);
        p_expect(p, TK_COLON);
        Type *v = parse_type(p);
        p_expect(p, TK_RBRACE);
        return ty_map(k, v);
    }
    if (p_match(p, TK_FN)) {              /* fn(int, string) -> bool */
        Type *f = ty_fn();
        p_expect(p, TK_LPAREN);
        while (!p_check(p, TK_RPAREN)) {
            VEC_PUSH_PTR(&f->args, parse_type(p));
            if (!p_match(p, TK_COMMA)) break;
        }
        p_expect(p, TK_RPAREN);
        VEC_PUSH_PTR(&f->args, p_match(p, TK_ARROW) ? parse_type(p) : ty_void());
        return f;
    }
    Token t = p_expect(p, TK_IDENT);
    if (strcmp(t.text, "int") == 0) return ty_int();
    if (strcmp(t.text, "float") == 0) return ty_float();
    if (strcmp(t.text, "bool") == 0) return ty_bool();
    if (strcmp(t.text, "string") == 0) return ty_string();
    if (is_tparam(p, t.text)) return ty_var(t.text);

    char *key;
    const char *path = alias_path(p, t.text);
    if (path && p_check(p, TK_DOT)) {          /* math.Vec */
        p_advance(p);
        Token member = p_expect(p, TK_IDENT);
        key = qual_key(path, member.text);
        note_xref(p, key, member.line);
    } else {
        key = resolve_written(p, t.text, t.line);
    }
    Type *ty = ty_named(key);
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

static Expr *parse_expr(Parser *p);
static Expr *parse_unary(Parser *p);
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

/* `if c { a } else { b }` in expression position.
 *
 * The braces hold one expression, not a block. Klang has no block-value rule
 * anywhere else — every function ends in an explicit `return` — and inventing one
 * here would mean two ways to read a pair of braces. `else` is required, because
 * without it there is no value when the condition is false.
 *
 * The chain is right-nested: the else of one is the next, so `else if` needs no
 * representation of its own. */
static Expr *parse_if_expr(Parser *p, int line) {
    Expr *e = new_expr(EX_IF, line);
    bool saved = p->no_struct_lit;
    p->no_struct_lit = true;         /* so `if x { ... }` is not read as a literal */
    e->lhs = parse_expr(p);
    p->no_struct_lit = saved;
    p_expect(p, TK_LBRACE);
    e->rhs = parse_expr(p);
    p_expect(p, TK_RBRACE);
    if (!p_match(p, TK_ELSE))
        fail(p->cur.line, "an 'if' used as a value needs an 'else' — otherwise there "
                          "is nothing to give back when the condition is false");
    int else_line = p->cur.line;
    if (p_match(p, TK_IF)) { VEC_PUSH_PTR(&e->args, parse_if_expr(p, else_line)); return e; }
    p_expect(p, TK_LBRACE);
    VEC_PUSH_PTR(&e->args, parse_expr(p));
    p_expect(p, TK_RBRACE);
    return e;
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
static Expr *parse_interp(Parser *p, const Token *tok) {
    Expr *acc = NULL;
    for (int i = 0; i < tok->parts->count; i++) {
        StrPart *sp = vec_get(tok->parts, i);
        Expr *piece;
        if (sp->expr) {
            /* The fragment is a piece of the same file: it must see the same module,
               imports, local types and type parameters as the code around it. */
            Parser sub = *p;
            lexer_init(&sub.lx, sp->expr);
            sub.lx.line = sp->line;      /* so errors point at the real source line */
            sub.no_struct_lit = false;
            p_advance(&sub);
            piece = parse_expr(&sub);
            if (!p_check(&sub, TK_EOF)) fail(sp->line, "unexpected trailing tokens inside '${...}'");
            Expr *call = new_expr(EX_CALL, sp->line);
            call->sval = "toString";
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
        if (tok.parts) return parse_interp(p, &tok);
        Expr *e = new_expr(EX_STRING, line);
        e->sval = tok.text;
        return e;
    }
    if (p_check(p, TK_SPAWN)) {
        /* `spawn e` runs e on another thread. It becomes a closure over whatever e
           mentions, so all the capture machinery is reused. */
        p_advance(p);
        Expr *s = new_expr(EX_SPAWN, line);
        Expr *lam = new_expr(EX_LAMBDA, line);
        lam->lhs = parse_expr(p);
        s->lhs = lam;
        return s;
    }
    if (p_check(p, TK_AWAIT)) {
        p_advance(p);
        Expr *a = new_expr(EX_AWAIT, line);
        a->lhs = parse_unary(p);
        return a;
    }
    if (p_check(p, TK_UNSAFE)) {
        /* `unsafe { expr }` in expression position; the statement form takes a block. */
        p_advance(p);
        Expr *u = new_expr(EX_UNSAFE, line);
        p_expect(p, TK_LBRACE);
        bool saved = p->no_struct_lit;
        p->no_struct_lit = false;
        u->lhs = parse_expr(p);
        p->no_struct_lit = saved;
        p_expect(p, TK_RBRACE);
        return u;
    }
    /* `|x, y| expr` and `|x, y| { ... }`. A zero-parameter closure is written `||`,
       which the lexer hands over as one token — an expression can never start with
       logical-or, so reading it as an empty parameter list is unambiguous. */
    if (p_check(p, TK_PIPE) || p_check(p, TK_OROR)) {
        Expr *e = new_expr(EX_LAMBDA, line);
        bool empty = p_check(p, TK_OROR);
        p_advance(p);
        if (!empty) {
            while (!p_check(p, TK_PIPE)) {
                Field *pm = vec_push(&e->params);
                pm->is_mut = p_match(p, TK_MUT);
                pm->name = p_expect(p, TK_IDENT).text;
                /* The type may be omitted when the surrounding code implies it. */
                pm->type = p_match(p, TK_COLON) ? parse_type(p) : NULL;
                if (!p_match(p, TK_COMMA)) break;
            }
            p_expect(p, TK_PIPE);
        }
        bool saved = p->no_struct_lit;
        p->no_struct_lit = false;
        if (p_check(p, TK_LBRACE)) { e->is_block = true; e->body = parse_block(p); }
        else e->lhs = parse_expr(p);
        p->no_struct_lit = saved;
        return e;
    }
    /* `{}` and `{k: v, ...}`. Not allowed where a block could start, so an `if`
       or `for` header needs parentheses around a map literal. */
    if (p_check(p, TK_LBRACE) && !p->no_struct_lit) {
        Expr *e = new_expr(EX_MAP_LIT, line);
        p_advance(p);
        while (!p_check(p, TK_RBRACE)) {
            VEC_PUSH_PTR(&e->args, parse_expr(p));   /* key */
            p_expect(p, TK_COLON);
            VEC_PUSH_PTR(&e->args, parse_expr(p));   /* value */
            if (!p_match(p, TK_COMMA)) break;
        }
        p_expect(p, TK_RBRACE);
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
    if (p_match(p, TK_IF)) return parse_if_expr(p, line);
    if (p_check(p, TK_IDENT)) {
        char *name = p->cur.text;
        p_advance(p);

        /* `math.abs(...)` / `math.Vec { ... }` — a module member, not a field access. */
        const char *path = alias_path(p, name);
        if (path && p_check(p, TK_DOT)) {
            p_advance(p);
            Token member = p_expect(p, TK_IDENT);
            char *key = qual_key(path, member.text);
            note_xref(p, key, member.line);
            if (p_check(p, TK_LPAREN)) {
                Expr *e = new_expr(EX_CALL, line);
                e->sval = key; e->is_qual = true;
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
                e->sval = key; e->is_qual = true;
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
            e->sval = key; e->is_qual = true;
            return e;
        }

        if (p_check(p, TK_LPAREN)) {
            /* call or variant constructor — resolved during typecheck */
            Expr *e = new_expr(EX_CALL, line);
            e->sval = name;
            e->mod = p->module;
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
            e->sval = resolve_written(p, name, line);
            e->is_qual = true;
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
        e->mod = p->module;
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
            if (p_check(p, TK_LPAREN)) {
                /* `x.f(a)` is `f(x, a)`. Resolution happens in typecheck, because it
                   depends on the receiver's type. */
                Expr *m = new_expr(EX_METHOD, f.line);
                m->lhs = e;
                m->sval = f.text;
                m->mod = p->module;
                p_advance(p);
                bool saved = p->no_struct_lit;
                p->no_struct_lit = false;
                while (!p_check(p, TK_RPAREN)) {
                    VEC_PUSH_PTR(&m->args, parse_expr(p));
                    if (!p_match(p, TK_COMMA)) break;
                }
                p->no_struct_lit = saved;
                p_expect(p, TK_RPAREN);
                e = m;
                continue;
            }
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
        } else if (p_check(p, TK_LPAREN)) {
            /* Calling whatever an expression produced: ops["add"](3, 4), f()(). */
            int line = p->cur.line;
            p_advance(p);
            bool saved = p->no_struct_lit;
            p->no_struct_lit = false;
            Expr *call = new_expr(EX_CALL, line);
            call->lhs = e;
            while (!p_check(p, TK_RPAREN)) {
                VEC_PUSH_PTR(&call->args, parse_expr(p));
                if (!p_match(p, TK_COMMA)) break;
            }
            p->no_struct_lit = saved;
            p_expect(p, TK_RPAREN);
            e = call;
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
    if (p_check(p, TK_UNSAFE)) {
        p_advance(p);
        Stmt *s = new_stmt(ST_BLOCK, line);
        s->is_unsafe = true;
        s->body = parse_block(p);
        return s;
    }
    if (p_match(p, TK_BREAK)) return new_stmt(ST_BREAK, line);
    if (p_match(p, TK_CONTINUE)) return new_stmt(ST_CONTINUE, line);

    /* Anything else is an expression; a trailing '=' turns it into an assignment.
       Validating that the left side is actually assignable happens in typecheck. */
    Expr *e = parse_expr(p);
    if (p_match(p, TK_EQ)) {
        Stmt *s = new_stmt(ST_ASSIGN, line);
        s->target = e;
        s->expr = parse_expr(p);
        return s;
    }
    /* `x += v` is exactly `x = x + v`. The target is evaluated twice, so keep
       side effects out of the index of a compound assignment. */
    {
        const char *op = NULL;
        if (p_check(p, TK_PLUSEQ)) op = "+";
        else if (p_check(p, TK_MINUSEQ)) op = "-";
        else if (p_check(p, TK_STAREQ)) op = "*";
        else if (p_check(p, TK_SLASHEQ)) op = "/";
        else if (p_check(p, TK_PERCENTEQ)) op = "%";
        if (op) {
            p_advance(p);
            Stmt *s = new_stmt(ST_ASSIGN, line);
            s->target = e;
            Expr *bin = new_expr(EX_BINARY, line);
            bin->op = strdup(op);
            bin->lhs = e;
            bin->rhs = parse_expr(p);
            s->expr = bin;
            return s;
        }
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

/* p->cur is the opening '{', so p->lx already sits just past it. */
static char *scan_js_body(Parser *p, int open_line) {
    char *body = take_js_body(&p->lx, open_line);
    p_advance(p);                 /* move past the body to whatever follows */
    return body;
}

/* The last segment of a module path is its default alias: "std/math" -> "math". */
static const char *path_tail(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void parse_program(Parser *p) {
    ModuleInfo *mi = vec_push(&g_modules);
    mi->module = p->module;
    vec_init(&mi->imports, sizeof(char *));
    /* Imports come first, so an alias is always in scope before it is used. */
    while (p_check(p, TK_IMPORT)) {
        int line = p->cur.line;
        p_advance(p);
        if (!p_check(p, TK_STRING)) fail(line, "expected a quoted module path after 'import'");
        char *path = p->cur.text;
        if (p->cur.parts) fail(line, "a module path cannot contain '${...}'");
        p_advance(p);
        for (const char *c = path; *c; c++) {
            if (!isalnum((unsigned char)*c) && *c != '_' && *c != '/' && *c != '-')
                fail(line, "'%s' is not a valid module path — use letters, digits, '_', '-' and '/'", path);
        }
        if (!*path) fail(line, "the module path is empty");
        const char *alias = p_match(p, TK_AS) ? p_expect(p, TK_IDENT).text : path_tail(path);
        if (alias_path(p, alias))
            fail(line, "'%s' is already imported — use 'as' to give this one a different name", alias);
        Alias *a = vec_push(&p->aliases);
        a->alias = (char *)alias; a->path = path;
        VEC_PUSH_PTR(&p->imports, path);
        VEC_PUSH_PTR(&mi->imports, path);
    }

    while (!p_check(p, TK_EOF)) {
        if (p_check(p, TK_IMPORT))
            fail(p->cur.line, "'import' must appear before any declaration");
        bool is_pub = p_match(p, TK_PUB);
        bool is_extern = false, is_unsafe = false, is_js = false, is_export = false;

        /* `js` and `export` are contextual: only a `js fn` / `export fn` means
           anything, so neither becomes a word you cannot use as a name. */
        if (p_check(p, TK_IDENT) &&
            (strcmp(p->cur.text, "js") == 0 || strcmp(p->cur.text, "export") == 0)) {
            Lexer save_lx = p->lx;
            Token save_cur = p->cur;
            bool js = strcmp(p->cur.text, "js") == 0;
            p_advance(p);
            if (p_check(p, TK_FN)) {
                if (js) { is_js = true; is_unsafe = true; }   /* Klang cannot vouch for JS */
                else is_export = true;
            } else { p->lx = save_lx; p->cur = save_cur; }
        }

        /* `extern header "..."` and `extern link "..."` say what the generated C
           needs; they are build information, not declarations. */
        if (p_check(p, TK_EXTERN)) {
            Lexer save_lx = p->lx;
            Token save_cur = p->cur;
            p_advance(p);
            if (p_check(p, TK_IDENT) &&
                (strcmp(p->cur.text, "header") == 0 || strcmp(p->cur.text, "link") == 0)) {
                bool is_header = strcmp(p->cur.text, "header") == 0;
                int line = p->cur.line;
                p_advance(p);
                if (!p_check(p, TK_STRING))
                    fail(line, "expected a quoted name after 'extern %s'",
                         is_header ? "header" : "link");
                VEC_PUSH_PTR(is_header ? &g_c_headers : &g_c_links, p->cur.text);
                p_advance(p);
                continue;
            }
            if (p_check(p, TK_TYPE)) {
                p_advance(p);
                StructDecl *sd = calloc(1, sizeof(StructDecl));
                sd->name = p_expect(p, TK_IDENT).text;
                sd->module = p->module; sd->is_pub = is_pub; sd->is_opaque = true;
                sd->key = qual_key(p->module, sd->name);
                vec_init(&sd->type_params, sizeof(char *));
                vec_init(&sd->fields, sizeof(Field));
                Decl *d = vec_push(&g_decls);
                memset(d, 0, sizeof *d);
                d->kind = DECL_STRUCT; d->s = sd;
                continue;
            }
            p->lx = save_lx; p->cur = save_cur;   /* it was `extern fn` */
            p_advance(p);
            is_extern = true;
            is_unsafe = true;      /* every C call is unsafe by construction */
        }
        if (p_match(p, TK_UNSAFE)) is_unsafe = true;

        if (p_check(p, TK_STRUCT)) {
            p_advance(p);
            StructDecl *sd = calloc(1, sizeof(StructDecl));
            sd->name = p_expect(p, TK_IDENT).text;
            sd->module = p->module; sd->is_pub = is_pub;
            sd->key = qual_key(p->module, sd->name);
            sd->type_params = parse_type_params(p);
            p->tparams = &sd->type_params;
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
            p->tparams = NULL;
            Decl *d = vec_push(&g_decls);
            memset(d, 0, sizeof *d);
            d->kind = DECL_STRUCT; d->s = sd;
        } else if (p_check(p, TK_ENUM)) {
            p_advance(p);
            EnumDecl *ed = calloc(1, sizeof(EnumDecl));
            ed->name = p_expect(p, TK_IDENT).text;
            ed->module = p->module; ed->is_pub = is_pub;
            ed->key = qual_key(p->module, ed->name);
            ed->type_params = parse_type_params(p);
            p->tparams = &ed->type_params;
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
            p->tparams = NULL;
            Decl *d = vec_push(&g_decls);
            memset(d, 0, sizeof *d);
            d->kind = DECL_ENUM; d->e = ed;
        } else if (p_check(p, TK_FN)) {
            int fd_line = p->cur.line;
            p_advance(p);
            FnDecl *fd = calloc(1, sizeof(FnDecl));
            fd->name = p_expect(p, TK_IDENT).text;
            fd->module = p->module; fd->file = p->file; fd->is_pub = is_pub;
            fd->key = qual_key(p->module, fd->name);
            fd->type_params = parse_type_params(p);
            p->tparams = &fd->type_params;
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
            fd->is_extern = is_extern;
            fd->is_unsafe = is_unsafe;
            fd->is_export = is_export;
            fd->line = fd_line;
            if (is_export && fd->type_params.count != 0)
                fail(fd_line, "an exported function cannot be generic — JavaScript "
                              "would not know which instantiation to call");
            if (is_js) {
                if (fd->type_params.count != 0)
                    fail(fd_line, "a 'js fn' cannot be generic — JavaScript has no types to specialize on");
                if (!p_check(p, TK_LBRACE))
                    fail(p->cur.line, "a 'js fn' needs a JavaScript body in braces");
                fd->js_body = scan_js_body(p, fd_line);
                vec_init(&fd->body, sizeof(Stmt *));
            } else if (is_extern) {
                if (fd->type_params.count != 0)
                    fail(fd_line, "an extern function cannot be generic — C has no generics");
                /* `= "cos"` when the C symbol differs from the Klang name. */
                if (p_match(p, TK_EQ)) {
                    if (!p_check(p, TK_STRING)) fail(p->cur.line, "expected a quoted C symbol name");
                    fd->cname = p->cur.text;
                    p_advance(p);
                } else fd->cname = fd->name;
                vec_init(&fd->body, sizeof(Stmt *));
            } else {
                fd->body = parse_block(p);
            }
            p->tparams = NULL;
            Decl *d = vec_push(&g_decls);
            memset(d, 0, sizeof *d);
            d->kind = DECL_FN; d->f = fd;
        } else if (p_check(p, TK_CONST) || p_check(p, TK_LET)) {
            /* `const` never changes. `let mut` at module level is state that
               outlives main — which an event-driven program needs, since a
               callback runs long after main returned and cannot capture a local.
               A plain module-level `let` would say nothing `const` does not. */
            bool was_let = p_check(p, TK_LET);
            int decl_line = p->cur.line;
            p_advance(p);
            bool is_mut = p_match(p, TK_MUT);
            if (was_let && !is_mut)
                fail(decl_line, "a module-level 'let' that never changes is a 'const' — "
                                "write 'const', or 'let mut' if it does change");
            ConstDecl *cd = calloc(1, sizeof(ConstDecl));
            cd->is_mut = is_mut;
            cd->line = p->cur.line;
            cd->name = p_expect(p, TK_IDENT).text;
            cd->module = p->module; cd->file = p->file; cd->is_pub = is_pub;
            cd->key = qual_key(p->module, cd->name);
            cd->mangled = key_mangle(cd->key);
            if (p_match(p, TK_COLON)) { cd->has_type = true; cd->type = parse_type(p); }
            p_expect(p, TK_EQ);
            cd->value = parse_expr(p);
            Decl *d = vec_push(&g_decls);
            memset(d, 0, sizeof *d);
            d->kind = DECL_CONST; d->c = cd;
        } else if (is_pub) {
            fail(p->cur.line, "'pub' must be followed by 'fn', 'struct', 'enum', 'const' or 'let mut'");
        } else {
            fail(p->cur.line, "expected 'fn', 'struct', 'enum', 'const' or 'import' at top level");
        }
    }
}

/* ───────────────────────── generic lookups ───────────────────────── */

/* Declarations are addressed by key, so all three lookups are exact matches. */
static StructDecl *find_struct(const char *key) {
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_STRUCT && strcmp(d->s->key, key) == 0) return d->s;
    }
    return NULL;
}
static EnumDecl *find_enum(const char *key) {
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_ENUM && strcmp(d->e->key, key) == 0) return d->e;
    }
    return NULL;
}
static ConstDecl *find_const(const char *key) {
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_CONST && strcmp(d->c->key, key) == 0) return d->c;
    }
    return NULL;
}
static FnDecl *find_fn(const char *key) {
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_FN && strcmp(d->f->key, key) == 0) return d->f;
    }
    return NULL;
}
static int variant_index(const EnumDecl *ed, const char *name) {
    for (int i = 0; i < ed->variants.count; i++)
        if (strcmp(((Variant *)vec_get(&ed->variants, i))->name, name) == 0) return i;
    return -1;
}

static StructDecl *find_mono_struct(const char *mangled);
static EnumDecl *find_mono_enum(const char *mangled);
/* What may cross a thread boundary. Immutable things (numbers, strings) are shared
   as they are; anything mutable is deep-copied, so no two threads ever reach the
   same object. Returns why a type cannot cross, or NULL when it can. */
static const char *crossing_problem(const Type *t) {
    switch (t->kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING: return NULL;
        case TY_ARRAY: return crossing_problem(ty_elem(t));
        case TY_MAP: {
            const char *k = crossing_problem(ty_key(t));
            return k ? k : crossing_problem(ty_val(t));
        }
        case TY_FN:
            return "a closure carries captured state, which would be shared — "
                   "pass the values it needs instead";
        case TY_TASK:
            return "a task handle belongs to the thread that created it";
        case TY_NAMED: {
            StructDecl *sd = find_mono_struct(ty_mangle(t));
            if (sd && sd->is_opaque)
                return "an extern handle points at something C owns, which Klang "
                       "cannot copy or reason about";
            if (sd) {
                for (int i = 0; i < sd->fields.count; i++) {
                    const char *w = crossing_problem(((Field *)vec_get(&sd->fields, i))->type);
                    if (w) return w;
                }
                return NULL;
            }
            EnumDecl *ed = find_mono_enum(ty_mangle(t));
            if (ed) {
                for (int i = 0; i < ed->variants.count; i++) {
                    Variant *v = vec_get(&ed->variants, i);
                    for (int j = 0; j < v->payload.count; j++) {
                        const char *w = crossing_problem(VEC_PTR(&v->payload, j, Type));
                        if (w) return w;
                    }
                }
                return NULL;
            }
            return NULL;
        }
        default: return "this type cannot cross a thread boundary";
    }
}

/* Which module's body is being type-checked; drives unqualified name resolution. */
static const char *g_cur_module = MOD_ROOT;

/* Which modules a variant name may come from without being qualified: your own,
   the prelude, and anything you imported. `Some` and `Ok` already work this way,
   so a `pub enum` you imported should too. Two enums offering the same variant
   name is caught and reported rather than silently picking one. */
static bool visible_unqualified(const char *module) {
    if (strcmp(module, g_cur_module) == 0 || strcmp(module, MOD_PRELUDE) == 0) return true;
    for (int i = 0; i < g_modules.count; i++) {
        ModuleInfo *mi = vec_get(&g_modules, i);
        if (strcmp(mi->module, g_cur_module) != 0) continue;
        for (int j = 0; j < mi->imports.count; j++)
            if (strcmp(VEC_PTR(&mi->imports, j, char), module) == 0) return true;
    }
    return false;
}

/* Variants are written unqualified (`Some(x)`, not `Option::Some(x)`), so find the
   enum declaring this variant name among the ones the current module can see. */
static EnumDecl *find_enum_by_variant(const char *vname, int line) {
    EnumDecl *found = NULL;
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind != DECL_ENUM) continue;
        if (!visible_unqualified(d->e->module)) continue;
        if (variant_index(d->e, vname) < 0) continue;
        if (found) fail(line, "variant '%s' is declared by both '%s' and '%s' — rename one to disambiguate",
                        vname, key_show(found->key), key_show(d->e->key));
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
    if (t->kind == TY_MAP) {
        Type *k = subst_type(ty_key(t), subst), *v = subst_type(ty_val(t), subst);
        return (k == ty_key(t) && v == ty_val(t)) ? t : ty_map(k, v);
    }
    if (t->kind == TY_FN) {
        Type *r = ty_fn();
        for (int i = 0; i < t->args.count; i++)
            VEC_PUSH_PTR(&r->args, subst_type(VEC_PTR(&t->args, i, Type), subst));
        return r;
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
    if (pat->kind == TY_ARRAY || pat->kind == TY_TASK) return unify(ty_elem(pat), ty_elem(actual), subst);
    if (pat->kind == TY_MAP)
        return unify(ty_key(pat), ty_key(actual), subst) && unify(ty_val(pat), ty_val(actual), subst);
    if (pat->kind == TY_FN) {
        if (pat->args.count != actual->args.count) return false;
        for (int i = 0; i < pat->args.count; i++)
            if (!unify(VEC_PTR(&pat->args, i, Type), VEC_PTR(&actual->args, i, Type), subst)) return false;
        return true;
    }
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
    c->sval = e->sval; c->op = e->op; c->is_qual = e->is_qual; c->mod = e->mod;
    c->is_block = e->is_block;
    for (int i = 0; i < e->params.count; i++) {
        Field *src = vec_get(&e->params, i);
        Field *dst = vec_push(&c->params);
        dst->name = src->name;
        dst->is_mut = src->is_mut;
        dst->type = src->type ? subst_type(src->type, subst) : NULL;
    }
    c->body = clone_stmts(&e->body, subst);
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
    c->is_unsafe = s->is_unsafe;
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

static Vec g_mono_maps;     /* Vec<Type*> — one entry per distinct (key, value) pair */
static Vec g_mono_fntypes;  /* Vec<Type*> — one entry per distinct signature */
static Vec g_mono_tasks;    /* Vec<Type*> — one entry per distinct result type */
static bool g_uses_threads; /* set when the program spawns, gating all thread code */

static bool array_registered(const char *mangled) {
    for (int i = 0; i < g_mono_arrays.count; i++)
        if (strcmp(ty_mangle(VEC_PTR(&g_mono_arrays, i, Type)), mangled) == 0) return true;
    return false;
}
static bool map_registered(const char *mangled) {
    for (int i = 0; i < g_mono_maps.count; i++)
        if (strcmp(ty_mangle(VEC_PTR(&g_mono_maps, i, Type)), mangled) == 0) return true;
    return false;
}
static bool fntype_registered(const char *mangled) {
    for (int i = 0; i < g_mono_fntypes.count; i++)
        if (strcmp(ty_mangle(VEC_PTR(&g_mono_fntypes, i, Type)), mangled) == 0) return true;
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
    if (t->kind == TY_FN) {
        for (int i = 0; i < t->args.count; i++) request_type(VEC_PTR(&t->args, i, Type), line);
        char *fm = ty_mangle(t);
        if (!fntype_registered(fm)) VEC_PUSH_PTR(&g_mono_fntypes, t);
        free(fm);
        return;
    }
    if (t->kind == TY_TASK) {
        request_type(ty_elem(t), line);
        char *tm = ty_mangle(t);
        bool seen = false;
        for (int i = 0; i < g_mono_tasks.count; i++)
            if (strcmp(ty_mangle(VEC_PTR(&g_mono_tasks, i, Type)), tm) == 0) { seen = true; break; }
        if (!seen) VEC_PUSH_PTR(&g_mono_tasks, t);
        free(tm);
        return;
    }
    if (t->kind == TY_MAP) {
        TyKind kk = ty_key(t)->kind;
        if (kk != TY_INT && kk != TY_STRING && kk != TY_BOOL)
            fail(line, "%s cannot be a map key — keys must be int, string or bool",
                 ty_str(ty_key(t)));
        request_type(ty_key(t), line);
        request_type(ty_val(t), line);
        /* keys(...) and values(...) hand back arrays, so those exist for every map */
        request_type(ty_array(ty_key(t)), line);
        request_type(ty_array(ty_val(t)), line);
        char *mm = ty_mangle(t);
        if (!map_registered(mm)) VEC_PUSH_PTR(&g_mono_maps, t);
        free(mm);
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
        sd->name = gs->name; sd->key = gs->key; sd->module = gs->module; sd->is_pub = gs->is_pub;
        sd->is_opaque = gs->is_opaque;
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
        ed->name = ge->name; ed->key = ge->key; ed->module = ge->module; ed->is_pub = ge->is_pub;
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
    sb_append(&sb, key_mangle(generic->key));
    for (int i = 0; i < type_args->count; i++) {
        sb_append(&sb, "_");
        ty_mangle_into(VEC_PTR(type_args, i, Type), &sb);
    }
    char *mangled = sb.data;
    /* `main` is emitted as klang_main; the real C main is a wrapper that anchors
       the collector's stack scan below every Klang frame. */
    if (strcmp(generic->name, "main") == 0) mangled = strdup("klang_main");
    /* An extern is called by its C symbol, and nothing is emitted for it. */
    if (generic->is_extern) mangled = strdup(generic->cname);
    if (find_mono_fn(mangled)) return mangled;

    if (g_mono_fns.count > MAX_INSTANCES)
        fail(line, "too many generic function instantiations — is a generic function "
                   "calling itself with an ever-growing type?");

    Vec subst = make_subst(&generic->type_params, type_args, "function", key_show(generic->key), line);
    FnDecl *fd = calloc(1, sizeof(FnDecl));
    fd->name = generic->name; fd->key = generic->key; fd->module = generic->module;
    fd->file = generic->file; fd->is_pub = generic->is_pub;
    fd->is_extern = generic->is_extern; fd->is_unsafe = generic->is_unsafe;
    fd->cname = generic->cname;
    fd->js_body = generic->js_body; fd->is_export = generic->is_export;
    fd->line = generic->line;
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
static Var *scope_lookup_at(Scope *sc, const char *name, int *depth) {
    for (int i = sc->scopes.count - 1; i >= 0; i--) {
        Vec *scope = vec_get(&sc->scopes, i);
        for (int j = scope->count - 1; j >= 0; j--) {
            Var *v = vec_get(scope, j);
            if (strcmp(v->name, name) == 0) { if (depth) *depth = i; return v; }
        }
    }
    return NULL;
}
static Var *scope_lookup(Scope *sc, const char *name) { return scope_lookup_at(sc, name, NULL); }

/* Closures being type-checked, innermost last. `boundary` is the scope depth the
   closure's own parameters live at, so anything found below it is a capture. */
typedef struct { Expr *lam; int boundary; } LamFrame;
static Vec g_lams;
static Vec g_fnrefs;   /* Vec<Expr*> — plain functions used as closure values */

static void note_capture(Expr *lam, const char *name, Type *type, bool is_mut) {
    for (int i = 0; i < lam->captures.count; i++)
        if (strcmp(((Field *)vec_get(&lam->captures, i))->name, name) == 0) return;
    Field *f = vec_push(&lam->captures);
    f->name = strdup(name); f->type = type; f->is_mut = is_mut;
}

/* Resolve a variable, recording it as a capture in every closure that sits between
   the use and the declaration — the inner ones need it passed through the outer. */
static Var *lookup_capturing(Scope *sc, const char *name) {
    int depth = 0;
    Var *v = scope_lookup_at(sc, name, &depth);
    if (!v) return NULL;
    for (int i = 0; i < g_lams.count; i++) {
        LamFrame *lf = vec_get(&g_lams, i);
        if (lf->boundary > depth) note_capture(lf->lam, name, v->type, v->is_mut);
    }
    return v;
}

/* ───────────────────────── typecheck ───────────────────────── */

static Type *g_cur_ret;  /* return type of the function being checked (for `?`) */
static int g_unsafe_depth;     /* inside an unsafe fn or unsafe block */
static int g_loop_depth;       /* 0 outside any loop, so break/continue can be checked */
static bool g_ret_inferring;   /* inside a block closure with no declared result */
static Type *g_ret_found;      /* what its `return` statements settled on */

static Type *tc_expr(Expr *e, Scope *sc, Type *expected);
static Type *tc_call(Expr *e, Scope *sc, Type *expected);
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
        /* Strings order lexicographically by byte, so text can be sorted. */
        if (lt->kind == TY_STRING && rt->kind == TY_STRING) return ty_bool();
        if (!ty_numeric(lt) || !ty_numeric(rt) || !ty_eq(lt, rt))
            fail(e->line, "operator '%s' needs matching numeric or string operands, got %s and %s",
                 op, ty_str(lt), ty_str(rt));
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
    if (expected && expected->kind == TY_NAMED && strcmp(expected->name, ge->key) == 0 &&
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

    Type *result = ty_named(ge->key);
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
        if (!visible_unqualified(d->e->module)) continue;
        if (variant_index(d->e, vname) < 0) continue;
        if (found) return NULL;
        found = d->e;
    }
    return found;
}
/* True when this expression cannot determine its own type — a variant like `None`
   whose payload doesn't mention every type parameter of its enum. */
static bool needs_expected(Expr *e, Scope *sc) {
    /* A closure with unannotated parameters can only be checked once something
       tells it what they are, so other arguments should be resolved first. */
    if (e->kind == EX_LAMBDA) {
        for (int i = 0; i < e->params.count; i++)
            if (!((Field *)vec_get(&e->params, i))->type) return true;
        return false;
    }
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
        /* A partly-resolved function type is still worth passing down: `fn(int) -> U`
           tells a closure what its parameters are even though the result is open. */
        bool useful = !ty_has_var(want) || want->kind == TY_FN;
        Type *got = tc_expr(arg, sc, useful ? want : NULL);
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
    /* EX_CONSTREF is what an EX_IDENT naming module state becomes once it has been
       type-checked, and lvalue_root is called from both sides of that. */
    if (e->kind != EX_IDENT && e->kind != EX_CONSTREF) return NULL;
    Var *v = lookup_capturing(sc, e->sval);
    if (v) return v;
    /* Module-level state is not in any scope, but it is still assignable — and
       carries its own `mut`, so the same rules apply to it as to a local. */
    ConstDecl *cd = find_const(e->is_qual ? e->sval
                               : qual_key(e->mod ? e->mod : g_cur_module, e->sval));
    if (!cd || !cd->type) return NULL;
    static Var global;
    global.name = cd->name; global.type = cd->type; global.is_mut = cd->is_mut;
    return &global;
}

/* Calling a closure value: the callee is in e->lhs and already typed. */
static Type *tc_indirect(Expr *e, Type *ft, Scope *sc, const char *what) {
    if (ft->kind != TY_FN)
        fail(e->line, "%s is %s, which is not something you can call", what, ty_str(ft));
    if (e->args.count != ty_nparams(ft))
        fail(e->line, "%s takes %d argument(s), got %d", what, ty_nparams(ft), e->args.count);
    for (int i = 0; i < e->args.count; i++) {
        Expr *a = VEC_PTR(&e->args, i, Expr);
        Type *want = ty_param(ft, i);
        Type *at = tc_expr(a, sc, want);
        if (!ty_eq(at, want))
            fail(a->line, "argument %d to %s: expected %s, got %s",
                 i + 1, what, ty_str(want), ty_str(at));
    }
    return ty_ret(ft);
}

/* `x.f(a)` means `f(x, a)`. The function is looked for in the current module, the
   prelude, and each imported module — taking only `pub` ones from elsewhere. The
   first parameter must accept the receiver. Exactly one candidate must match. */
static Type *tc_method(Expr *e, Scope *sc, Type *expected) {
    Type *recv = tc_expr(e->lhs, sc, NULL);

    /* A struct field holding a closure wins: it is a real member, not a free function. */
    if (recv->kind == TY_NAMED) {
        StructDecl *sd = find_mono_struct(ty_mangle(recv));
        if (sd) {
            for (int i = 0; i < sd->fields.count; i++) {
                Field *f = vec_get(&sd->fields, i);
                if (strcmp(f->name, e->sval) != 0) continue;
                if (f->type->kind != TY_FN)
                    fail(e->line, "field '%s' of %s is %s, which is not something you can call",
                         e->sval, ty_str(recv), ty_str(f->type));
                Expr *fe = new_expr(EX_FIELD, e->line);
                fe->lhs = e->lhs; fe->sval = e->sval; fe->type = f->type;
                e->kind = EX_CALL;
                e->lhs = fe;
                return tc_indirect(e, f->type, sc, labelf("'.%s'", e->sval));
            }
        }
    }

    /* Builtins take part too, so `xs.len()` and `m.has(k)` read the same way. */
    static const char *builtins[] = {
        "len", "push", "has", "remove", "keys", "values", "get",
        "substr", "byteAt", "indexOf", "toString", "print", "println", NULL
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(builtins[i], e->sval) != 0) continue;
        Vec bargs; vec_init(&bargs, sizeof(Expr *));
        VEC_PUSH_PTR(&bargs, e->lhs);
        for (int j = 0; j < e->args.count; j++) VEC_PUSH_PTR(&bargs, VEC_PTR(&e->args, j, Expr));
        e->kind = EX_CALL;
        e->is_qual = false;
        e->lhs = NULL;
        e->args = bargs;
        return tc_call(e, sc, expected);
    }

    const char *from = e->mod ? e->mod : g_cur_module;
    Vec cands; vec_init(&cands, sizeof(char *));
    Vec search; vec_init(&search, sizeof(char *));
    VEC_PUSH_PTR(&search, (char *)from);
    VEC_PUSH_PTR(&search, (char *)MOD_PRELUDE);
    for (int i = 0; i < g_modules.count; i++) {
        ModuleInfo *mi = vec_get(&g_modules, i);
        if (strcmp(mi->module, from) != 0) continue;
        for (int j = 0; j < mi->imports.count; j++)
            VEC_PUSH_PTR(&search, VEC_PTR(&mi->imports, j, char));
    }

    for (int i = 0; i < search.count; i++) {
        const char *mod = VEC_PTR(&search, i, char);
        char *key = qual_key(mod, e->sval);
        FnDecl *fn = find_fn(key);
        if (!fn) continue;
        if (strcmp(mod, from) != 0 && strcmp(mod, MOD_PRELUDE) != 0 && !fn->is_pub) continue;
        if (fn->params.count == 0) continue;
        Vec probe; vec_init(&probe, sizeof(Binding));
        if (!unify(((Field *)vec_get(&fn->params, 0))->type, recv, &probe)) continue;
        bool seen = false;
        for (int j = 0; j < cands.count; j++)
            if (strcmp(VEC_PTR(&cands, j, char), key) == 0) { seen = true; break; }
        if (!seen) VEC_PUSH_PTR(&cands, key);
    }

    if (cands.count == 0)
        fail(e->line, "no function '%s' takes %s as its first argument — "
                      "'x.%s(...)' means '%s(x, ...)'", e->sval, ty_str(recv), e->sval, e->sval);
    if (cands.count > 1)
        fail(e->line, "'%s' is ambiguous for %s: %s and %s both match — call it directly "
                      "to say which you mean", e->sval, ty_str(recv),
             key_show(VEC_PTR(&cands, 0, char)), key_show(VEC_PTR(&cands, 1, char)));

    /* Rewrite into an ordinary call with the receiver as the first argument, so
       generics, inference and codegen all take their usual path. */
    Vec args; vec_init(&args, sizeof(Expr *));
    VEC_PUSH_PTR(&args, e->lhs);
    for (int i = 0; i < e->args.count; i++) VEC_PUSH_PTR(&args, VEC_PTR(&e->args, i, Expr));
    e->kind = EX_CALL;
    e->sval = VEC_PTR(&cands, 0, char);
    e->is_qual = true;
    e->lhs = NULL;
    e->args = args;
    return tc_call(e, sc, expected);
}

static Type *tc_call(Expr *e, Scope *sc, Type *expected) {
    /* The parser already produced a callee expression, as in ops["add"](3, 4). */
    if (e->lhs) {
        Type *ft = tc_expr(e->lhs, sc, NULL);
        return tc_indirect(e, ft, sc, "this");
    }
    /* A local holding a function is called through, and shadows any builtin. */
    if (!e->is_qual) {
        Var *v = lookup_capturing(sc, e->sval);
        if (v) {
            Expr *callee = new_expr(EX_IDENT, e->line);
            callee->sval = e->sval;
            callee->type = v->type;
            e->lhs = callee;               /* marks this as an indirect call */
            return tc_indirect(e, v->type, sc, labelf("'%s'", e->sval));
        }
    }
    /* A function you defined wins over a builtin of the same name: your own module
       is the nearer scope, and a builtin should never make your code uncallable. */
    bool shadowed = !e->is_qual &&
        find_fn(qual_key(e->mod ? e->mod : g_cur_module, e->sval)) != NULL;

    if (!shadowed && (strcmp(e->sval, "println") == 0 || strcmp(e->sval, "print") == 0)) {
        if (e->args.count != 1) fail(e->line, "'%s' takes exactly 1 argument", e->sval);
        tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        return ty_void();
    }
    if (!shadowed && strcmp(e->sval, "assert") == 0) {
        if (e->args.count != 2)
            fail(e->line, "'assert' takes 2 arguments: the condition and a message");
        Type *ct = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, ty_bool());
        if (ct->kind != TY_BOOL) fail(e->line, "'assert' needs a bool condition, got %s", ty_str(ct));
        Type *mt = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, ty_string());
        if (mt->kind != TY_STRING) fail(e->line, "'assert' needs a string message, got %s", ty_str(mt));
        return ty_void();
    }
    if (!shadowed && (strcmp(e->sval, "toInt") == 0 || strcmp(e->sval, "toFloat") == 0)) {
        bool toInt = strcmp(e->sval, "toInt") == 0;
        if (e->args.count != 1) fail(e->line, "'%s' takes exactly 1 argument", e->sval);
        Type *at = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, toInt ? ty_float() : ty_int());
        if (toInt && at->kind != TY_FLOAT)
            fail(e->line, "'toInt' converts a float, got %s", ty_str(at));
        if (!toInt && at->kind != TY_INT)
            fail(e->line, "'toFloat' converts an int, got %s", ty_str(at));
        return toInt ? ty_int() : ty_float();
    }
    if (!shadowed && (strcmp(e->sval, "wrapAdd") == 0 || strcmp(e->sval, "wrapSub") == 0 ||
                      strcmp(e->sval, "wrapMul") == 0)) {
        if (e->args.count != 2) fail(e->line, "'%s' takes 2 int arguments", e->sval);
        for (int i = 0; i < 2; i++) {
            Type *at = tc_expr(VEC_PTR(&e->args, i, Expr), sc, ty_int());
            if (at->kind != TY_INT) fail(e->line, "'%s' needs ints, got %s", e->sval, ty_str(at));
        }
        return ty_int();
    }
    /* Reading and writing raw bytes through a C pointer. This is the sharp edge the
       `unsafe` keyword exists for: it is how a Klang program builds a C struct — a
       sockaddr, say — that no safe construct can express. */
    if (!shadowed && (strcmp(e->sval, "pokeByte") == 0 || strcmp(e->sval, "peekByte") == 0)) {
        bool poking = strcmp(e->sval, "pokeByte") == 0;
        int want = poking ? 3 : 2;
        if (e->args.count != want)
            fail(e->line, "'%s' takes %d arguments: the pointer, an offset%s",
                 e->sval, want, poking ? ", and the byte" : "");
        if (g_unsafe_depth == 0)
            fail(e->line, "'%s' reads or writes raw memory, so it is unsafe — "
                          "wrap it in 'unsafe { ... }' inside a function that knows "
                          "the pointer and offset are valid", e->sval);
        Type *pt = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        StructDecl *sd = pt->kind == TY_NAMED ? find_mono_struct(ty_mangle(pt)) : NULL;
        if (!sd || !sd->is_opaque)
            fail(e->line, "'%s' needs an 'extern type' pointer, got %s", e->sval, ty_str(pt));
        for (int i = 1; i < want; i++) {
            Type *at = tc_expr(VEC_PTR(&e->args, i, Expr), sc, ty_int());
            if (at->kind != TY_INT) fail(e->line, "'%s' needs int offsets and bytes", e->sval);
        }
        return poking ? ty_void() : ty_int();
    }
    if (!shadowed && strcmp(e->sval, "isNull") == 0) {
        if (e->args.count != 1) fail(e->line, "'isNull' takes exactly 1 argument");
        Type *at = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        StructDecl *sd = at->kind == TY_NAMED ? find_mono_struct(ty_mangle(at)) : NULL;
        if (!sd || !sd->is_opaque)
            fail(e->line, "'isNull' only applies to an 'extern type' handle, got %s", ty_str(at));
        return ty_bool();
    }
    if (!shadowed && strcmp(e->sval, "gcCollect") == 0) {
        if (e->args.count != 0) fail(e->line, "'gc_collect' takes no arguments");
        return ty_void();
    }
    if (!shadowed && strcmp(e->sval, "gcHeap") == 0) {
        if (e->args.count != 0) fail(e->line, "'gc_heap' takes no arguments");
        return ty_int();
    }
    if (!shadowed && strcmp(e->sval, "len") == 0) {
        if (e->args.count != 1) fail(e->line, "'len' takes exactly 1 argument");
        Type *at = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        if (at->kind != TY_ARRAY && at->kind != TY_STRING && at->kind != TY_MAP)
            fail(e->line, "'len' needs an array, string or map, got %s", ty_str(at));
        return ty_int();
    }
    /* The string kernel. Everything else — split, trim, case conversion, parsing —
       is written in Klang on top of these, in std/string. */
    if (!shadowed && strcmp(e->sval, "substr") == 0) {
        if (e->args.count != 3)
            fail(e->line, "'substr' takes 3 arguments: the string, start, and end");
        Type *st = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, ty_string());
        if (st->kind != TY_STRING) fail(e->line, "'substr' needs a string, got %s", ty_str(st));
        for (int i = 1; i < 3; i++) {
            Type *it = tc_expr(VEC_PTR(&e->args, i, Expr), sc, ty_int());
            if (it->kind != TY_INT) fail(e->line, "'substr' bounds must be int, got %s", ty_str(it));
        }
        return ty_string();
    }
    if (!shadowed && strcmp(e->sval, "byteAt") == 0) {
        if (e->args.count != 2) fail(e->line, "'byte_at' takes 2 arguments: the string and an index");
        Type *st = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, ty_string());
        if (st->kind != TY_STRING) fail(e->line, "'byte_at' needs a string, got %s", ty_str(st));
        Type *it = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, ty_int());
        if (it->kind != TY_INT) fail(e->line, "'byte_at' index must be int, got %s", ty_str(it));
        return ty_int();
    }
    if (!shadowed && strcmp(e->sval, "fromByte") == 0) {
        if (e->args.count != 1) fail(e->line, "'from_byte' takes exactly 1 argument");
        Type *it = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, ty_int());
        if (it->kind != TY_INT) fail(e->line, "'from_byte' needs an int, got %s", ty_str(it));
        return ty_string();
    }
    if (!shadowed && strcmp(e->sval, "indexOf") == 0) {
        if (e->args.count != 2)
            fail(e->line, "'index_of' takes 2 arguments: the string and what to look for");
        for (int i = 0; i < 2; i++) {
            Type *st = tc_expr(VEC_PTR(&e->args, i, Expr), sc, ty_string());
            if (st->kind != TY_STRING) fail(e->line, "'index_of' needs strings, got %s", ty_str(st));
        }
        return ty_int();
    }
    if (!shadowed && (strcmp(e->sval, "has") == 0 || strcmp(e->sval, "remove") == 0)) {
        bool removing = strcmp(e->sval, "remove") == 0;
        if (e->args.count != 2)
            fail(e->line, removing ? "'remove' takes 2 arguments: the map and the key, "
                                     "or the array and the index"
                                   : "'has' takes 2 arguments: the map and the key");
        Expr *m = VEC_PTR(&e->args, 0, Expr);
        Type *mt = tc_expr(m, sc, NULL);
        /* `remove` also takes an array and an index, because a list that can only
           grow is not a list. It keeps order; `has` stays map-only, since asking
           whether an array has an index is just a comparison with its length. */
        bool from_array = removing && mt->kind == TY_ARRAY;
        if (!from_array && mt->kind != TY_MAP)
            fail(e->line, removing ? "'remove' needs a map or an array, got %s"
                                   : "'has' needs a map, got %s", ty_str(mt));
        if (removing) {
            Var *root = lvalue_root(m, sc);
            if (!root) fail(e->line, "'remove' needs a variable to remove from");
            if (!root->is_mut)
                fail(e->line, "cannot remove from '%s' because it is immutable — "
                          "declare it 'let mut' to allow this", root->name);
        }
        if (from_array) {
            Type *it = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, ty_int());
            if (it->kind != TY_INT)
                fail(e->line, "removing from an array needs an index, got %s", ty_str(it));
            return ty_void();
        }
        Type *kt = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, ty_key(mt));
        if (!ty_eq(kt, ty_key(mt)))
            fail(e->line, "this map is keyed by %s, got %s", ty_str(ty_key(mt)), ty_str(kt));
        return removing ? ty_void() : ty_bool();
    }
    if (!shadowed && (strcmp(e->sval, "keys") == 0 || strcmp(e->sval, "values") == 0)) {
        if (e->args.count != 1) fail(e->line, "'%s' takes exactly 1 argument", e->sval);
        Type *mt = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        if (mt->kind != TY_MAP) fail(e->line, "'%s' needs a map, got %s", e->sval, ty_str(mt));
        return ty_array(strcmp(e->sval, "keys") == 0 ? ty_key(mt) : ty_val(mt));
    }
    if (!shadowed && strcmp(e->sval, "get") == 0) {
        if (e->args.count != 2)
            fail(e->line, "'get' takes 2 arguments: the map and the key");
        Type *mt = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        if (mt->kind != TY_MAP) fail(e->line, "'get' needs a map, got %s", ty_str(mt));
        Type *kt = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, ty_key(mt));
        if (!ty_eq(kt, ty_key(mt)))
            fail(e->line, "this map is keyed by %s, got %s", ty_str(ty_key(mt)), ty_str(kt));
        Type *opt = ty_named(PRELUDE_OPTION);
        VEC_PUSH_PTR(&opt->args, ty_val(mt));
        return opt;
    }
    if (!shadowed && strcmp(e->sval, "push") == 0) {
        if (e->args.count != 2) fail(e->line, "'push' takes exactly 2 arguments: the array and the value");
        Expr *arr = VEC_PTR(&e->args, 0, Expr);
        Type *at = tc_expr(arr, sc, NULL);
        if (at->kind != TY_ARRAY) fail(e->line, "'push' needs an array, got %s", ty_str(at));
        Var *root = lvalue_root(arr, sc);
        if (!root)
            fail(e->line, "'push' needs a variable to push into");
        if (!root->is_mut)
            fail(e->line, "cannot push to '%s' because it is immutable — "
                          "declare it 'let mut' to allow this", root->name);
        Type *vt = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, ty_elem(at));
        if (!ty_eq(vt, ty_elem(at)))
            fail(e->line, "cannot push %s into %s", ty_str(vt), ty_str(at));
        return ty_void();
    }
    if (!shadowed && strcmp(e->sval, "toString") == 0) {
        if (e->args.count != 1) fail(e->line, "'to_string' takes exactly 1 argument");
        Type *at = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, NULL);
        if (at->kind == TY_NAMED)
            fail(e->line, "'to_string' does not support %s — match on it and build the string yourself", ty_str(at));
        if (at->kind == TY_VOID) fail(e->line, "'to_string' needs a value, got void");
        return ty_string();
    }
    FnDecl *fn;
    const char *shown;
    if (e->is_qual) {
        fn = find_fn(e->sval);
        shown = key_show(e->sval);
        if (!fn) fail(e->line, "module '%.*s' has no function '%s'",
                      (int)(strrchr(e->sval, ':') - e->sval), e->sval, key_name(e->sval));
    } else {
        EnumDecl *ge = find_enum_by_variant(e->sval, e->line);
        if (ge) return tc_variant(e, sc, expected, ge);
        char *key = qual_key(e->mod ? e->mod : g_cur_module, e->sval);
        fn = find_fn(key);
        shown = e->sval;
        if (!fn) fail(e->line, "call to undefined function '%s'", e->sval);
    }
    /* The whole point of the FFI safety story: a C call, or anything declared
       unsafe, can only be reached from a context that has taken responsibility. */
    if (fn->is_unsafe && g_unsafe_depth == 0)
        fail(e->line, fn->js_body
                 ? "'%s' is a JavaScript function, so calling it is unsafe — Klang "
                   "cannot check what JavaScript does. Wrap the call in 'unsafe { ... }', "
                   "or expose it through a safe function, the way std/dom does"
               : fn->is_extern
                 ? "'%s' is a C function, so calling it is unsafe — wrap the call in "
                   "'unsafe { ... }', or expose it through a safe function that checks "
                   "what C cannot"
                 : "'%s' is unsafe — call it inside 'unsafe { ... }', or mark the "
                   "calling function 'unsafe' to pass the obligation on",
             shown);
    if (e->args.count != fn->params.count)
        fail(e->line, "'%s' expects %d argument(s), got %d", shown, fn->params.count, e->args.count);

    Vec subst; vec_init(&subst, sizeof(Binding));
    Vec wants; vec_init(&wants, sizeof(Type *));
    Vec labels; vec_init(&labels, sizeof(char *));
    for (int i = 0; i < fn->params.count; i++) {
        VEC_PUSH_PTR(&wants, ((Field *)vec_get(&fn->params, i))->type);
        VEC_PUSH_PTR(&labels, labelf("argument %d to '%s'", i + 1, shown));
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
        if (!bound) fail(e->line, "cannot infer type '%s' for call to '%s'", tp, shown);
        VEC_PUSH_PTR(&type_args, bound);
    }
    e->resolved = request_fn(fn, &type_args, e->line);
    return subst_type(fn->ret_type, &subst);
}

static Type *tc_struct_lit(Expr *e, Scope *sc, Type *expected) {
    StructDecl *gs = find_struct(e->sval);
    if (!gs) fail(e->line, "unknown struct '%s'", key_show(e->sval));
    if (e->fields.count != gs->fields.count)
        fail(e->line, "struct '%s' has %d field(s), got %d", key_show(e->sval), gs->fields.count, e->fields.count);

    Vec subst; vec_init(&subst, sizeof(Binding));
    if (expected && expected->kind == TY_NAMED && strcmp(expected->name, gs->key) == 0 &&
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
        if (!df) fail(e->line, "struct '%s' has no field '%s'", key_show(e->sval), fi->name);
        VEC_PUSH_PTR(&wants, df->type);
        VEC_PUSH_PTR(&vals, fi->value);
        VEC_PUSH_PTR(&labels, labelf("field '%s'", fi->name));
    }
    tc_args(&wants, &vals, &labels, &subst, sc);

    Type *result = ty_named(gs->key);
    for (int i = 0; i < gs->type_params.count; i++) {
        char *tp = VEC_PTR(&gs->type_params, i, char);
        Type *bound = NULL;
        for (int j = 0; j < subst.count; j++) {
            Binding *b = vec_get(&subst, j);
            if (strcmp(b->name, tp) == 0) { bound = b->type; break; }
        }
        if (!bound) fail(e->line, "cannot infer type '%s' of struct '%s'", tp, key_show(gs->key));
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
        if (n) fail(e->line, "match on %s is not exhaustive, missing %s — add those arms, or a '_' arm",
                    ty_str(st), missing.data);
    }
    free(covered);
    if (any_block) return ty_void();
    return result ? result : ty_void();
}

static Type *tc_try(Expr *e, Scope *sc) {
    Type *it = tc_expr(e->lhs, sc, NULL);
    if (it->kind != TY_NAMED || it->args.count == 0 ||
        (strcmp(it->name, PRELUDE_RESULT) != 0 && strcmp(it->name, PRELUDE_OPTION) != 0))
        fail(e->line, "'?' works on Result or Option, got %s", ty_str(it));
    if (!g_cur_ret || g_cur_ret->kind != TY_NAMED || strcmp(g_cur_ret->name, it->name) != 0)
        fail(e->line, "'?' on %s requires the enclosing function to return %s too, but it returns %s",
             ty_str(it), key_show(it->name), g_cur_ret ? ty_str(g_cur_ret) : "nothing");
    if (strcmp(it->name, PRELUDE_RESULT) == 0) {
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
            Var *v = lookup_capturing(sc, e->sval);
            if (v) { t = v->type; break; }
            ConstDecl *cd = find_const(e->is_qual ? e->sval
                                       : qual_key(e->mod ? e->mod : g_cur_module, e->sval));
            if (cd) {
                if (!cd->type)
                    fail(e->line, "'%s' is used before it is defined — constants are set up "
                                  "in the order they are written", cd->name);
                e->kind = EX_CONSTREF;
                e->resolved = cd->mangled;
                t = cd->type;
                break;
            }
            EnumDecl *ge = find_enum_by_variant(e->sval, e->line);
            if (ge) { t = tc_variant(e, sc, expected, ge); break; }
            /* A plain function name used as a value becomes a closure with no
               captures, so `map(xs, double)` works without wrapping it in `|x| ...`. */
            FnDecl *fn = e->is_qual ? find_fn(e->sval)
                                    : find_fn(qual_key(e->mod ? e->mod : g_cur_module, e->sval));
            if (fn) {
                if (fn->type_params.count != 0)
                    fail(e->line, "'%s' is generic, so it cannot be used as a value here — "
                                  "wrap it, as in '|x| %s(x)'", key_show(fn->key), key_show(fn->key));
                Type *ft = ty_fn();
                for (int i = 0; i < fn->params.count; i++)
                    VEC_PUSH_PTR(&ft->args, ((Field *)vec_get(&fn->params, i))->type);
                VEC_PUSH_PTR(&ft->args, fn->ret_type);
                Vec none; vec_init(&none, sizeof(Type *));
                if (fn->is_unsafe)
                    fail(e->line, "'%s' is unsafe, so it cannot be handed around as a value "
                                  "— wrap it in a safe function first", key_show(fn->key));
                e->kind = EX_FNREF;
                e->resolved = request_fn(fn, &none, e->line);
                e->type = ft;
                VEC_PUSH_PTR(&g_fnrefs, e);
                t = ft;
                break;
            }
            fail(e->line, "undefined variable '%s'", e->sval);
            return NULL;
        }
        case EX_LAMBDA: {
            Type *want = expected && expected->kind == TY_FN &&
                         ty_nparams(expected) == e->params.count ? expected : NULL;
            for (int i = 0; i < e->params.count; i++) {
                Field *pm = vec_get(&e->params, i);
                if (pm->type) continue;
                if (!want || ty_has_var(ty_param(want, i)))
                    fail(e->line, "cannot tell what type '%s' is — annotate it, as in "
                                  "'|%s: int| ...'", pm->name, pm->name);
                pm->type = ty_param(want, i);
            }
            /* The result may still be an open type variable; infer it from the body. */
            if (want && ty_has_var(ty_ret(want))) want = NULL;
            scope_push(sc);
            int boundary = sc->scopes.count - 1;
            for (int i = 0; i < e->params.count; i++) {
                Field *pm = vec_get(&e->params, i);
                request_type(pm->type, e->line);
                scope_declare(sc, pm->name, pm->type, pm->is_mut);
            }
            LamFrame *lf = vec_push(&g_lams);
            lf->lam = e; lf->boundary = boundary;
            int saved_loops = g_loop_depth;
            g_loop_depth = 0;   /* an outer loop is not breakable from inside a closure */
            int saved_unsafe = g_unsafe_depth;
            g_unsafe_depth = 0; /* a closure body is its own context, not the caller's */

            Type *saved_ret = g_cur_ret;
            bool saved_inferring = g_ret_inferring;
            Type *saved_found = g_ret_found;
            Type *ret;
            if (e->is_block && want) {
                ret = ty_ret(want);
                g_cur_ret = ret;
                g_ret_inferring = false;
                tc_block(&e->body, sc);
            } else if (e->is_block) {
                /* No expected type, so take the result from what the body returns. */
                g_cur_ret = ty_void();
                g_ret_inferring = true;
                g_ret_found = NULL;
                tc_block(&e->body, sc);
                ret = g_ret_found ? g_ret_found : ty_void();
            } else {
                g_ret_inferring = false;
                g_cur_ret = want ? ty_ret(want) : NULL;
                Type *bt = tc_expr(e->lhs, sc, want ? ty_ret(want) : NULL);
                ret = want ? ty_ret(want) : bt;
                if (want && !ty_eq(bt, ret))
                    fail(e->line, "this closure must return %s but its body gives %s",
                         ty_str(ret), ty_str(bt));
            }
            g_cur_ret = saved_ret;
            g_loop_depth = saved_loops;
            g_unsafe_depth = saved_unsafe;
            g_ret_inferring = saved_inferring;
            g_ret_found = saved_found;
            g_lams.count--;
            scope_pop(sc);

            Type *ft = ty_fn();
            for (int i = 0; i < e->params.count; i++)
                VEC_PUSH_PTR(&ft->args, ((Field *)vec_get(&e->params, i))->type);
            VEC_PUSH_PTR(&ft->args, ret);
            t = ft;
            break;
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
        case EX_METHOD: t = tc_method(e, sc, expected); break;
        /* These are rewrites of EX_IDENT produced earlier in this same pass. A
           receiver is looked at once by the method call and again as argument
           zero, so re-checking one has to be a no-op. */
        case EX_CONSTREF: case EX_FNREF: t = e->type; break;
        case EX_SPAWN: {
            g_uses_threads = true;
            Type *ft = tc_expr(e->lhs, sc, NULL);          /* the implicit closure */
            Type *res = ty_ret(ft);
            if (res->kind == TY_VOID)
                fail(e->line, "'spawn' needs an expression that produces a value — "
                              "have the task return something, even just a count");
            /* Nothing mutable is shared across the boundary, so each captured value
               is copied. Only things that cannot be copied meaningfully are refused. */
            for (int i = 0; i < e->lhs->captures.count; i++) {
                Field *c = vec_get(&e->lhs->captures, i);
                const char *why = crossing_problem(c->type);
                if (why)
                    fail(e->line, "'%s' cannot cross into a task: %s", c->name, why);
            }
            const char *why = crossing_problem(res);
            if (why) fail(e->line, "a task cannot return this: %s", why);
            request_type(res, e->line);
            t = ty_task(res);
            break;
        }
        case EX_AWAIT: {
            Type *tt = tc_expr(e->lhs, sc, NULL);
            if (tt->kind != TY_TASK)
                fail(e->line, "'await' needs a task, got %s", ty_str(tt));
            t = ty_elem(tt);
            break;
        }
        case EX_UNSAFE:
            g_unsafe_depth++;
            t = tc_expr(e->lhs, sc, expected);
            g_unsafe_depth--;
            break;
        case EX_MATCH: t = tc_match(e, sc, expected); break;
        case EX_IF: {
            Type *ct = tc_expr(e->lhs, sc, ty_bool());
            if (ct->kind != TY_BOOL)
                fail(e->lhs->line, "an 'if' condition must be bool, got %s", ty_str(ct));
            Type *a = tc_expr(e->rhs, sc, expected);
            Type *b = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, expected ? expected : a);
            if (a->kind == TY_VOID || b->kind == TY_VOID)
                fail(e->line, "this 'if' is used as a value, so both branches have to "
                              "produce one");
            if (!ty_eq(a, b))
                fail(e->line, "the branches of this 'if' disagree: one gives %s, the "
                              "other %s", ty_str(a), ty_str(b));
            t = a;
            break;
        }
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
            if (base->kind == TY_MAP) {
                Type *kt = tc_expr(e->rhs, sc, ty_key(base));
                if (!ty_eq(kt, ty_key(base)))
                    fail(e->rhs->line, "this map is keyed by %s, got %s",
                         ty_str(ty_key(base)), ty_str(kt));
                t = ty_val(base);
                break;
            }
            if (base->kind != TY_ARRAY)
                fail(e->line, "cannot index %s — only arrays and maps support '[...]'", ty_str(base));
            Type *ix = tc_expr(e->rhs, sc, ty_int());
            if (ix->kind != TY_INT) fail(e->rhs->line, "an array index must be int, got %s", ty_str(ix));
            t = ty_elem(base);
            break;
        }
        case EX_MAP_LIT: {
            Type *wk = expected && expected->kind == TY_MAP ? ty_key(expected) : NULL;
            Type *wv = expected && expected->kind == TY_MAP ? ty_val(expected) : NULL;
            if (e->args.count == 0) {
                if (!wk)
                    fail(e->line, "cannot tell what this empty map holds — "
                                  "annotate it, as in 'let m: {string: int} = {}'");
                t = ty_map(wk, wv);
                break;
            }
            Type *kt = tc_expr(VEC_PTR(&e->args, 0, Expr), sc, wk);
            Type *vt = tc_expr(VEC_PTR(&e->args, 1, Expr), sc, wv);
            if (vt->kind == TY_VOID) fail(e->line, "a map cannot hold void values");
            for (int i = 2; i < e->args.count; i += 2) {
                Expr *k = VEC_PTR(&e->args, i, Expr);
                Expr *v = VEC_PTR(&e->args, i + 1, Expr);
                Type *at = tc_expr(k, sc, kt);
                if (!ty_eq(at, kt))
                    fail(k->line, "map key %d is %s but the first is %s — "
                                  "every key must have the same type", i / 2 + 1, ty_str(at), ty_str(kt));
                Type *bt = tc_expr(v, sc, vt);
                if (!ty_eq(bt, vt))
                    fail(v->line, "map value %d is %s but the first is %s — "
                                  "every value must have the same type", i / 2 + 1, ty_str(bt), ty_str(vt));
            }
            t = ty_map(kt, vt);
            break;
        }
        default: t = ty_unknown(); break;
    }
    if (t->kind == TY_NAMED || t->kind == TY_ARRAY || t->kind == TY_MAP ||
        t->kind == TY_FN || t->kind == TY_TASK)
        request_type(t, e->line);
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
                fail(s->line, "cannot assign to '%s' because it is immutable — "
                          "declare it 'let mut' to allow this", root->name);
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
            g_loop_depth++;
            tc_block(&s->body, sc);
            g_loop_depth--;
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
            g_loop_depth++;
            tc_block(&s->body, sc);
            g_loop_depth--;
            scope_pop(sc);
            break;
        }
        case ST_RETURN:
            if (g_ret_inferring) {
                if (s->expr) {
                    Type *rt = tc_expr(s->expr, sc, g_ret_found);
                    if (!g_ret_found) g_ret_found = rt;
                    else if (!ty_eq(rt, g_ret_found))
                        fail(s->line, "this closure returns %s here but %s elsewhere — "
                                      "every 'return' must agree", ty_str(rt), ty_str(g_ret_found));
                }
                break;
            }
            if (s->expr) {
                if (g_cur_ret->kind == TY_VOID) fail(s->line, "this function returns nothing, but 'return' has a value");
                Type *rt = tc_expr(s->expr, sc, g_cur_ret);
                if (!ty_eq(rt, g_cur_ret))
                    fail(s->line, "returning %s but the function declares %s", ty_str(rt), ty_str(g_cur_ret));
            } else if (g_cur_ret->kind != TY_VOID) {
                fail(s->line, "the function returns %s but 'return' has no value", ty_str(g_cur_ret));
            }
            break;
        case ST_BREAK:
            if (g_loop_depth == 0) fail(s->line, "'break' only works inside a loop");
            break;
        case ST_CONTINUE:
            if (g_loop_depth == 0) fail(s->line, "'continue' only works inside a loop");
            break;
        case ST_EXPR: tc_expr(s->expr, sc, NULL); break;
        case ST_BLOCK:
            if (s->is_unsafe) g_unsafe_depth++;
            tc_block(&s->body, sc);
            if (s->is_unsafe) g_unsafe_depth--;
            break;
    }
}

static void tc_block(Vec *body, Scope *sc) {
    scope_push(sc);
    for (int i = 0; i < body->count; i++) tc_stmt(VEC_PTR(body, i, Stmt), sc);
    scope_pop(sc);
}

/* JavaScript has numbers, booleans and strings, and Klang's heap objects mean
   nothing on the other side of that boundary. Rather than silently marshalling
   something half-way, say so. */
static bool js_can_cross(const Type *t) {
    switch (t->kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING: case TY_VOID: return true;
        default: return false;
    }
}

static void tc_js_signature(FnDecl *fd) {
    if (fd->file) g_filename = fd->file;
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        if (!js_can_cross(pm->type) || pm->type->kind == TY_VOID)
            fail(fd->line, "'%s' cannot cross into JavaScript: %s is %s, and only "
                           "int, float, bool and string have a meaning on both sides",
                 fd->name, pm->name, ty_str(pm->type));
    }
    if (!js_can_cross(fd->ret_type))
        fail(fd->line, "'%s' cannot return %s to Klang: only int, float, bool and "
                       "string cross back from JavaScript",
             fd->name, ty_str(fd->ret_type));
}

static void tc_fn(FnDecl *fd) {
    if (fd->js_body) { tc_js_signature(fd); return; }
    if (fd->is_export) {
        if (fd->file) g_filename = fd->file;
        for (int i = 0; i < fd->params.count; i++) {
            Field *pm = vec_get(&fd->params, i);
            if (!js_can_cross(pm->type) || pm->type->kind == TY_VOID)
                fail(fd->line, "'%s' is exported to JavaScript, so %s cannot be %s — "
                               "only int, float, bool and string cross that boundary",
                     fd->name, pm->name, ty_str(pm->type));
        }
        if (!js_can_cross(fd->ret_type))
            fail(fd->line, "'%s' is exported to JavaScript, so it cannot return %s — "
                           "only int, float, bool and string cross that boundary",
                 fd->name, ty_str(fd->ret_type));
    }
    Scope sc; scope_init(&sc); scope_push(&sc);
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        scope_declare(&sc, pm->name, pm->type, pm->is_mut);
    }
    g_cur_ret = fd->ret_type;
    g_loop_depth = 0;
    /* An unsafe function is itself an unsafe context: its callers took on the
       obligation, so it need not repeat `unsafe` inside. */
    g_unsafe_depth = fd->is_unsafe ? 1 : 0;
    g_cur_module = fd->module ? fd->module : MOD_ROOT;
    if (fd->file) g_filename = fd->file;   /* so errors name the right file */
    tc_block(&fd->body, &sc);
    scope_pop(&sc);
}

/* Qualified names are the only way to reach another module, so checking the
   references the parser recorded covers every cross-module access. */
static void check_visibility(void) {
    for (int i = 0; i < g_xrefs.count; i++) {
        XRef *x = vec_get(&g_xrefs, i);
        const char *colon = strrchr(x->key, ':');
        size_t modlen = (size_t)(colon - x->key);
        if (strncmp(x->key, x->from, modlen) == 0 && x->from[modlen] == 0) continue;

        bool found = false, is_pub = false;
        const char *what = "item";
        for (int j = 0; j < g_decls.count; j++) {
            Decl *d = vec_get(&g_decls, j);
            const char *k = d->kind == DECL_STRUCT ? d->s->key
                          : d->kind == DECL_ENUM   ? d->e->key
                          : d->kind == DECL_CONST  ? d->c->key : d->f->key;
            if (strcmp(k, x->key) != 0) continue;
            found = true;
            is_pub = d->kind == DECL_STRUCT ? d->s->is_pub
                   : d->kind == DECL_ENUM   ? d->e->is_pub
                   : d->kind == DECL_CONST  ? d->c->is_pub : d->f->is_pub;
            what = d->kind == DECL_STRUCT ? "struct"
                 : d->kind == DECL_ENUM   ? "enum"
                 : d->kind == DECL_CONST  ? "constant" : "function";
            break;
        }
        g_filename = x->file;
        if (!found)
            fail(x->line, "module '%.*s' has no '%s'", (int)modlen, x->key, key_name(x->key));
        if (!is_pub)
            fail(x->line, "%s '%s' is private to module '%.*s' — mark it 'pub' to use it "
                          "from another module", what, key_name(x->key), (int)modlen, x->key);
    }
}

/* Constants are checked before any function body, so a body can refer to one
   whatever order the file puts them in. */
static void tc_consts(void) {
    Scope sc; scope_init(&sc); scope_push(&sc);
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind != DECL_CONST) continue;
        ConstDecl *cd = d->c;
        g_cur_module = cd->module ? cd->module : MOD_ROOT;
        g_cur_ret = ty_void();
        g_loop_depth = 0;
        if (cd->file) g_filename = cd->file;
        if (cd->has_type) request_type(cd->type, cd->line);
        Type *vt = tc_expr(cd->value, &sc, cd->has_type ? cd->type : NULL);
        if (vt->kind == TY_VOID) fail(cd->line, "constant '%s' cannot hold a void value", cd->name);
        if (cd->has_type) {
            if (!ty_eq(vt, cd->type))
                fail(cd->line, "constant '%s' is declared %s but set to %s",
                     cd->name, ty_str(cd->type), ty_str(vt));
        } else cd->type = vt;
    }
    scope_pop(&sc);
}

static void monomorphize_and_check(void) {
    check_visibility();

    FnDecl *main_fn = find_fn(MOD_ROOT ":main");
    if (!main_fn) fail(0, "no 'main' function defined");
    if (main_fn->params.count != 0) fail(0, "'main' must take no arguments");
    if (main_fn->ret_type->kind != TY_VOID) fail(0, "'main' must not declare a return type");
    if (main_fn->type_params.count != 0) fail(0, "'main' must not be generic");

    tc_consts();

    /* Seed with every non-generic function so unused ones are still checked. */
    Vec empty; vec_init(&empty, sizeof(Type *));
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_FN && !d->f->is_extern && d->f->type_params.count == 0)
            request_fn(d->f, &empty, 0);
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
        case TY_NAMED: case TY_ARRAY: case TY_MAP: case TY_FN: case TY_TASK: return ty_mangle(t);
        default: return "void*";
    }
}
/* Every expression is emitted fully parenthesized, so a condition arrives as
   "(a == b)" and `if ((a == b))` is what comes out — which clang flags as a
   possible typo'd assignment. The outer pair is redundant inside `if (...)`, so
   drop it when it wraps the whole expression. */
static char *unwrap_cond(const char *s) {
    size_t n = strlen(s);
    if (n < 2 || s[0] != '(' || s[n - 1] != ')') return (char *)s;
    int depth = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '(') depth++;
        else if (s[i] == ')' && --depth == 0 && i != n - 1) return (char *)s;
    }
    char *inner = malloc(n - 1);
    memcpy(inner, s + 1, n - 2);
    inner[n - 2] = '\0';
    return inner;
}

static char *fresh_tmp(void) {
    char *s = malloc(24);
    snprintf(s, 24, "_k%d", g_tmp++);
    return s;
}
static void indent_to(SB *sb, int n) { for (int i = 0; i < n; i++) sb_append(sb, "    "); }

static void cg_expr(Expr *e, SB *out, SB *pre, int ind);
static void cg_stmts(Vec *body, SB *sb, int ind);
static void copy_expr(const Type *t, const char *val, SB *out);

/* Does a value of this type contain anything the collector owns? Only such
   values need a root slot; an int or a bool never does. String literals live in
   .rodata rather than the heap, but rooting one is harmless — the collector
   ignores an address that is not one of its objects. */
static bool type_has_gc(const Type *t) {
    switch (t->kind) {
        case TY_STRING: case TY_ARRAY: case TY_MAP: case TY_FN: case TY_TASK:
            return true;
        case TY_NAMED: {
            StructDecl *sd = find_mono_struct(ty_mangle(t));
            if (sd) {
                if (sd->is_opaque) return false;   /* a pointer C owns */
                for (int i = 0; i < sd->fields.count; i++)
                    if (type_has_gc(((Field *)vec_get(&sd->fields, i))->type)) return true;
                return false;
            }
            EnumDecl *ed = find_mono_enum(ty_mangle(t));
            if (ed) {
                for (int i = 0; i < ed->variants.count; i++) {
                    Variant *v = vec_get(&ed->variants, i);
                    for (int j = 0; j < v->payload.count; j++)
                        if (type_has_gc(VEC_PTR(&v->payload, j, Type))) return true;
                }
                return false;
            }
            return false;
        }
        default: return false;
    }
}

/* Slots used by the function currently being emitted. The prologue is written
   after its body, so the count is known by then. */
static int g_slot_count = 0;

/* Register a named local, parameter or temporary as a root. */
static void root_local(SB *sb, int ind, const Type *t, const char *name) {
    if (!type_has_gc(t)) return;
    indent_to(sb, ind);
    sb_appendf(sb, "_kr[%d].addr = &%s; _kr[%d].size = sizeof %s;\n",
               g_slot_count, name, g_slot_count, name);
    g_slot_count++;
}

/* C leaves the order of evaluation between operands unspecified, but Klang
   promises left to right. Anything that could observe the difference — a call —
   forces earlier operands into temporaries so they run first. */
static bool has_call(Expr *e) {
    if (!e) return false;
    /* EX_IF and EX_MATCH are here not because they call, but because they lower to
       statements: whatever came before them has to be in a temporary already, or it
       would be evaluated after their branches run. */
    if (e->kind == EX_CALL || e->kind == EX_MATCH || e->kind == EX_IF || e->kind == EX_TRY)
        return true;
    if (has_call(e->lhs) || has_call(e->rhs)) return true;
    for (int i = 0; i < e->args.count; i++) if (has_call(VEC_PTR(&e->args, i, Expr))) return true;
    for (int i = 0; i < e->fields.count; i++)
        if (has_call(((FieldInit *)vec_get(&e->fields, i))->value)) return true;
    return false;
}

/* Emit an operand, optionally pinning it to a temporary so its side effects
   happen before whatever is emitted next. */
static void cg_operand(Expr *e, SB *out, SB *pre, int ind, bool pin) {
    /* A collectable intermediate always goes through a rooted temporary: while it
       waits to be used it may be the only reference, and on WASM a value held in a
       local is in no memory the collector can see. */
    if (type_has_gc(e->type)) pin = true;
    if (!pin || e->type->kind == TY_VOID) { cg_expr(e, out, pre, ind); return; }
    SB val; sb_init(&val);
    cg_expr(e, &val, pre, ind);
    char *tmp = fresh_tmp();
    indent_to(pre, ind);
    sb_appendf(pre, "%s %s = %s;\n", c_type(e->type), tmp, val.data);
    root_local(pre, ind, e->type, tmp);
    sb_append(out, tmp);
}

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

/* Lower an if-expression into a C if that assigns to `dest`. Recursive, because
   the parser leaves `else if` as a nested if-expression in the else slot. */
static void cg_if_expr(Expr *e, SB *pre, int ind, const char *dest) {
    SB cpre; sb_init(&cpre);
    SB cval; sb_init(&cval);
    cg_expr(e->lhs, &cval, &cpre, ind);
    sb_append(pre, cpre.data);
    indent_to(pre, ind);
    sb_appendf(pre, "if (%s) {\n", unwrap_cond(cval.data));

    SB tpre; sb_init(&tpre);
    SB tval; sb_init(&tval);
    cg_expr(e->rhs, &tval, &tpre, ind + 1);
    sb_append(pre, tpre.data);
    indent_to(pre, ind + 1);
    sb_appendf(pre, "%s = %s;\n", dest, tval.data);

    indent_to(pre, ind);
    sb_append(pre, "} else {\n");
    Expr *els = VEC_PTR(&e->args, 0, Expr);
    if (els->kind == EX_IF) {
        cg_if_expr(els, pre, ind + 1, dest);
    } else {
        SB epre; sb_init(&epre);
        SB eval; sb_init(&eval);
        cg_expr(els, &eval, &epre, ind + 1);
        sb_append(pre, epre.data);
        indent_to(pre, ind + 1);
        sb_appendf(pre, "%s = %s;\n", dest, eval.data);
    }
    indent_to(pre, ind);
    sb_append(pre, "}\n");
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

    /* An if-chain rather than a switch, so a `break` written in an arm belongs to
       the enclosing loop instead of silently escaping the switch. The catch-all is
       emitted last whatever its position, which is exactly what a `default` did. */
    int n = e->arms.count;
    int *order = malloc(sizeof(int) * (size_t)n);
    int k = 0;
    for (int i = 0; i < n; i++) if (i != default_idx) order[k++] = i;
    order[k++] = default_idx;

    for (int slot = 0; slot < n; slot++) {
        MatchArm *arm = vec_get(&e->arms, order[slot]);
        indent_to(pre, ind);
        if (n == 1) sb_append(pre, "{\n");
        else if (slot == 0) sb_appendf(pre, "if (%s.tag == %s_TAG_%s) {\n", scrut, em, arm->pat.variant);
        else if (slot == n - 1) sb_append(pre, "else {\n");
        else sb_appendf(pre, "else if (%s.tag == %s_TAG_%s) {\n", scrut, em, arm->pat.variant);

        if (arm->pat.variant) {
            int vi = variant_index(ed, arm->pat.variant);
            Variant *v = vec_get(&ed->variants, vi);
            for (int j = 0; j < arm->pat.binds.count; j++) {
                const char *bind = VEC_PTR(&arm->pat.binds, j, char);
                indent_to(pre, ind + 1);
                sb_appendf(pre, "%s %s = %s.data.%s._%d; (void)%s;\n",
                           c_type(VEC_PTR(&v->payload, j, Type)),
                           bind, scrut, arm->pat.variant, j, bind);
            }
        }
        if (arm->is_block) {
            cg_stmts(&arm->body, pre, ind + 1);
        } else {
            SB val_pre; sb_init(&val_pre);
            SB val; sb_init(&val);
            cg_expr(arm->value, &val, &val_pre, ind + 1);
            sb_append(pre, val_pre.data);
            indent_to(pre, ind + 1);
            if (result_var) sb_appendf(pre, "%s = %s;\n", result_var, val.data);
            else if (arm->value->type->kind != TY_VOID) sb_appendf(pre, "(void)(%s);\n", val.data);
            else sb_appendf(pre, "%s;\n", val.data);
        }
        indent_to(pre, ind);
        sb_append(pre, "}\n");
    }
    free(order);
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
    root_local(pre, ind, it, tmp);

    char *om = ty_mangle(g_cg_ret);
    bool is_result = strcmp(it->name, PRELUDE_RESULT) == 0;
    const char *bad = is_result ? "Err" : "None";
    indent_to(pre, ind);
    sb_appendf(pre, "if (%s.tag == %s_TAG_%s) {\n", tmp, im, bad);
    indent_to(pre, ind + 1);
    sb_append(pre, "k_frames = _kf.prev;\n");   /* `?` leaves the function early */
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
            /* Anything that is not plainly printable goes out as an octal escape,
               so no byte can terminate or corrupt the C literal. */
            sb_append(out, "\"");
            for (const char *p = e->sval; *p; p++) {
                unsigned char ch = (unsigned char)*p;
                if (ch == '"' || ch == '\\') sb_appendf(out, "\\%c", ch);
                else if (ch == '\n') sb_append(out, "\\n");
                else if (ch == '\r') sb_append(out, "\\r");
                else if (ch == '\t') sb_append(out, "\\t");
                else if (ch < 0x20 || ch == 0x7f) sb_appendf(out, "\\%03o", ch);
                else { char b[2] = {(char)ch, 0}; sb_append(out, b); }
            }
            sb_append(out, "\"");
            break;
        case EX_IDENT: sb_append(out, e->sval); break;
        case EX_VARIANT: cg_variant_value(e->type, e->sval, &e->args, out, pre, ind); break;
        case EX_UNARY:
            if (strcmp(e->op, "-") == 0 && e->type->kind == TY_INT) {
                sb_append(out, "klang_neg(");     /* -INT64_MIN overflows */
                cg_expr(e->lhs, out, pre, ind);
                sb_append(out, ")");
                break;
            }
            sb_appendf(out, "(%s", e->op);
            cg_expr(e->lhs, out, pre, ind);
            sb_append(out, ")");
            break;
        case EX_BINARY:
            /* && and || must not evaluate their right side unless they have to,
               and the right side may need statements, so lower them to an if. */
            if ((strcmp(e->op, "&&") == 0 || strcmp(e->op, "||") == 0) && has_call(e->rhs)) {
                bool is_and = strcmp(e->op, "&&") == 0;
                SB lv; sb_init(&lv);
                cg_expr(e->lhs, &lv, pre, ind);
                char *tmp = fresh_tmp();
                indent_to(pre, ind);
                sb_appendf(pre, "bool %s = %s;\n", tmp, lv.data);
                indent_to(pre, ind);
                sb_appendf(pre, "if (%s%s) {\n", is_and ? "" : "!", tmp);
                SB rv; sb_init(&rv);
                SB rpre; sb_init(&rpre);
                cg_expr(e->rhs, &rv, &rpre, ind + 1);
                sb_append(pre, rpre.data);
                indent_to(pre, ind + 1);
                sb_appendf(pre, "%s = %s;\n", tmp, rv.data);
                indent_to(pre, ind);
                sb_append(pre, "}\n");
                sb_append(out, tmp);
                break;
            }
            {
                /* Left before right, whatever order C would have picked. */
                bool pin = has_call(e->rhs);
                bool str = e->lhs->type->kind == TY_STRING;
                const char *op = e->op;

                /* Integer arithmetic goes through checked helpers: signed overflow
                   is undefined in C, and a wrong answer is worse than a stop. */
                if (e->lhs->type->kind == TY_INT && e->type->kind == TY_INT) {
                    const char *fn = strcmp(op, "+") == 0 ? "klang_add"
                                   : strcmp(op, "-") == 0 ? "klang_sub"
                                   : strcmp(op, "*") == 0 ? "klang_mul"
                                   : strcmp(op, "/") == 0 ? "klang_div"
                                   : strcmp(op, "%") == 0 ? "klang_mod" : NULL;
                    /* Dividing by a literal that is neither 0 nor -1 cannot trap, so
                       the check is dropped and the compiler keeps its usual
                       strength reduction. */
                    bool divlike = fn && (strcmp(op, "/") == 0 || strcmp(op, "%") == 0);
                    if (divlike && e->rhs->kind == EX_INT && e->rhs->ival != 0 && e->rhs->ival != -1)
                        fn = NULL;
                    if (fn) {
                        sb_appendf(out, "%s(", fn);
                        cg_operand(e->lhs, out, pre, ind, pin);
                        sb_append(out, ", ");
                        cg_expr(e->rhs, out, pre, ind);
                        sb_append(out, ")");
                        break;
                    }
                }
                if (str && strcmp(op, "+") == 0) sb_append(out, "klang_str_concat(");
                else if (str && strcmp(op, "==") == 0) sb_append(out, "klang_str_eq(");
                else if (str && strcmp(op, "!=") == 0) sb_append(out, "(!klang_str_eq(");
                else if (str) sb_append(out, "(strcmp(");
                else sb_append(out, "(");

                cg_operand(e->lhs, out, pre, ind, pin);
                if (str) sb_append(out, ", ");
                else sb_appendf(out, " %s ", op);
                cg_expr(e->rhs, out, pre, ind);

                if (str && strcmp(op, "+") == 0) sb_append(out, ")");
                else if (str && strcmp(op, "==") == 0) sb_append(out, ")");
                else if (str && strcmp(op, "!=") == 0) sb_append(out, "))");
                else if (str) sb_appendf(out, ") %s 0)", op);
                else sb_append(out, ")");
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
        case EX_IF: {
            /* A C conditional expression would be shorter, but a branch may need
               statements of its own — a call that pins its operands, say — and those
               have to land inside the branch that uses them. */
            char *tmp = fresh_tmp();
            indent_to(pre, ind);
            sb_appendf(pre, "%s %s;\n", c_type(e->type), tmp);
            if (type_has_gc(e->type)) {
                indent_to(pre, ind);
                sb_appendf(pre, "memset(&%s, 0, sizeof %s);\n", tmp, tmp);
            }
            root_local(pre, ind, e->type, tmp);
            cg_if_expr(e, pre, ind, tmp);
            sb_append(out, tmp);
            break;
        }
        case EX_MATCH: {
            char *tmp = fresh_tmp();
            indent_to(pre, ind);
            sb_appendf(pre, "%s %s;\n", c_type(e->type), tmp);
            if (type_has_gc(e->type)) {
                indent_to(pre, ind);   /* rooted before it is assigned, so zero it */
                sb_appendf(pre, "memset(&%s, 0, sizeof %s);\n", tmp, tmp);
            }
            root_local(pre, ind, e->type, tmp);
            cg_match(e, pre, ind, tmp);
            sb_append(out, tmp);
            break;
        }
        case EX_ARRAY_LIT: {
            char *m = ty_mangle(e->type);
            char *tmp = fresh_tmp();
            indent_to(pre, ind);
            sb_appendf(pre, "%s %s = %s_new(%d);\n", m, tmp, m, e->args.count);
            root_local(pre, ind, e->type, tmp);
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
        case EX_LAMBDA: {
            /* Allocate the environment, copy the captures in, hand back { fn, env }. */
            char *env = fresh_tmp();
            indent_to(pre, ind);
            if (e->captures.count > 0) {
                sb_appendf(pre, "_klam%d_env *%s = klang_gc_alloc(sizeof(_klam%d_env));\n",
                           e->lam_id, env, e->lam_id);
                for (int i = 0; i < e->captures.count; i++) {
                    Field *c = vec_get(&e->captures, i);
                    indent_to(pre, ind);
                    sb_appendf(pre, "%s->%s = %s;\n", env, c->name, c->name);
                }
            } else {
                sb_appendf(pre, "void *%s = NULL;\n", env);
            }
            sb_appendf(out, "(%s){ .fn = _klam%d, .env = %s }", ty_mangle(e->type), e->lam_id, env);
            break;
        }
        case EX_FNREF:
            sb_appendf(out, "(%s){ .fn = _kref_%s, .env = NULL }", ty_mangle(e->type), e->resolved);
            break;
        case EX_CONSTREF:
            sb_append(out, e->resolved);
            break;
        case EX_METHOD:   /* rewritten into EX_CALL during typecheck */
            break;
        case EX_UNSAFE:   /* purely a promise to the compiler; no code of its own */
            cg_expr(e->lhs, out, pre, ind);
            break;
        case EX_SPAWN: {
            /* Build the closure's environment from *copies*, then hand it to a
               thread. The parent keeps its originals; nothing is shared. */
            Expr *lam = e->lhs;
            char *env = fresh_tmp();
            indent_to(pre, ind);
            if (lam->captures.count > 0) {
                sb_appendf(pre, "_klam%d_env *%s = klang_gc_alloc(sizeof(_klam%d_env));\n",
                           lam->lam_id, env, lam->lam_id);
                for (int i = 0; i < lam->captures.count; i++) {
                    Field *c = vec_get(&lam->captures, i);
                    SB cp; sb_init(&cp);
                    copy_expr(c->type, c->name, &cp);
                    indent_to(pre, ind);
                    sb_appendf(pre, "%s->%s = %s;\n", env, c->name, cp.data);
                }
            } else {
                sb_appendf(pre, "void *%s = NULL;\n", env);
            }
            sb_appendf(out, "%s_spawn(_klam%d, %s)", ty_mangle(e->type), lam->lam_id, env);
            break;
        }
        case EX_AWAIT:
            sb_appendf(out, "%s_await(", ty_mangle(e->lhs->type));
            cg_expr(e->lhs, out, pre, ind);
            sb_append(out, ")");
            break;
        case EX_MAP_LIT: {
            char *m = ty_mangle(e->type);
            char *tmp = fresh_tmp();
            indent_to(pre, ind);
            sb_appendf(pre, "%s %s = %s_new();\n", m, tmp, m);
            root_local(pre, ind, e->type, tmp);
            for (int i = 0; i < e->args.count; i += 2) {
                SB kv; sb_init(&kv);
                SB vv; sb_init(&vv);
                cg_expr(VEC_PTR(&e->args, i, Expr), &kv, pre, ind);
                cg_expr(VEC_PTR(&e->args, i + 1, Expr), &vv, pre, ind);
                indent_to(pre, ind);
                sb_appendf(pre, "%s_put(%s, %s, %s);\n", m, tmp, kv.data, vv.data);
            }
            sb_append(out, tmp);
            break;
        }
        case EX_TRY: cg_try(e, out, pre, ind); break;
        case EX_CALL: {
            if (e->lhs) {          /* indirect: call through a closure value */
                SB cb; sb_init(&cb);
                cg_expr(e->lhs, &cb, pre, ind);
                char *cv = fresh_tmp();
                indent_to(pre, ind);
                sb_appendf(pre, "%s %s = %s;\n", c_type(e->lhs->type), cv, cb.data);
                root_local(pre, ind, e->lhs->type, cv);
                sb_appendf(out, "%s.fn(%s.env", cv, cv);
                for (int i = 0; i < e->args.count; i++) {
                    sb_append(out, ", ");
                    cg_expr(VEC_PTR(&e->args, i, Expr), out, pre, ind);
                }
                sb_append(out, ")");
                break;
            }
            /* Only a call the typechecker left unresolved can be a builtin; anything
               it resolved is a real function, even if it shares a builtin name. */
            if (!e->resolved) {
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
            if (strcmp(e->sval, "toInt") == 0 || strcmp(e->sval, "toFloat") == 0 ||
                strcmp(e->sval, "wrapAdd") == 0 || strcmp(e->sval, "wrapSub") == 0 ||
                strcmp(e->sval, "wrapMul") == 0) {
                const char *cn = strcmp(e->sval, "toInt") == 0   ? "klang_to_int"
                               : strcmp(e->sval, "toFloat") == 0 ? "klang_to_float"
                               : strcmp(e->sval, "wrapAdd") == 0 ? "klang_wrap_add"
                               : strcmp(e->sval, "wrapSub") == 0 ? "klang_wrap_sub"
                                                                 : "klang_wrap_mul";
                sb_appendf(out, "%s(", cn);
                for (int i = 0; i < e->args.count; i++) {
                    if (i) sb_append(out, ", ");
                    cg_expr(VEC_PTR(&e->args, i, Expr), out, pre, ind);
                }
                sb_append(out, ")");
                break;
            }
            if (strcmp(e->sval, "pokeByte") == 0 || strcmp(e->sval, "peekByte") == 0) {
                sb_appendf(out, "klang_%s(", strcmp(e->sval, "pokeByte") == 0 ? "poke" : "peek");
                for (int i = 0; i < e->args.count; i++) {
                    if (i) sb_append(out, ", ");
                    cg_expr(VEC_PTR(&e->args, i, Expr), out, pre, ind);
                }
                sb_append(out, ")");
                break;
            }
            if (strcmp(e->sval, "isNull") == 0) {
                sb_append(out, "((");
                cg_expr(VEC_PTR(&e->args, 0, Expr), out, pre, ind);
                sb_append(out, ") == NULL)");
                break;
            }
            if (strcmp(e->sval, "gcCollect") == 0) { sb_append(out, "klang_gc_collect()"); break; }
            if (strcmp(e->sval, "gcHeap") == 0) { sb_append(out, "klang_gc_heap()"); break; }
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
            if (strcmp(e->sval, "substr") == 0 || strcmp(e->sval, "byteAt") == 0 ||
                strcmp(e->sval, "fromByte") == 0 || strcmp(e->sval, "indexOf") == 0) {
                const char *cname = strcmp(e->sval, "substr") == 0   ? "klang_substr"
                                  : strcmp(e->sval, "byteAt") == 0   ? "klang_byte_at"
                                  : strcmp(e->sval, "fromByte") == 0 ? "klang_from_byte"
                                                                     : "klang_index_of";
                sb_appendf(out, "%s(", cname);
                for (int i = 0; i < e->args.count; i++) {
                    if (i) sb_append(out, ", ");
                    cg_expr(VEC_PTR(&e->args, i, Expr), out, pre, ind);
                }
                sb_append(out, ")");
                break;
            }
            if (strcmp(e->sval, "has") == 0 || strcmp(e->sval, "remove") == 0 ||
                strcmp(e->sval, "keys") == 0 || strcmp(e->sval, "values") == 0) {
                Expr *m = VEC_PTR(&e->args, 0, Expr);
                sb_appendf(out, "%s_%s(", ty_mangle(m->type), e->sval);
                cg_expr(m, out, pre, ind);
                if (e->args.count > 1) {
                    sb_append(out, ", ");
                    cg_expr(VEC_PTR(&e->args, 1, Expr), out, pre, ind);
                }
                sb_append(out, ")");
                break;
            }
            if (strcmp(e->sval, "get") == 0) {
                /* Some(v) when present, None otherwise — evaluated into a temp so the
                   map and key are each read once. */
                Expr *m = VEC_PTR(&e->args, 0, Expr);
                Expr *k = VEC_PTR(&e->args, 1, Expr);
                char *mm = ty_mangle(m->type);
                char *mv = fresh_tmp(), *kv = fresh_tmp(), *rv = fresh_tmp();
                SB mb; sb_init(&mb);
                SB kb; sb_init(&kb);
                cg_expr(m, &mb, pre, ind);
                cg_expr(k, &kb, pre, ind);
                char *om = ty_mangle(e->type);
                indent_to(pre, ind);
                sb_appendf(pre, "%s %s = %s;\n", c_type(m->type), mv, mb.data);
                indent_to(pre, ind);
                sb_appendf(pre, "%s %s = %s;\n", c_type(ty_key(m->type)), kv, kb.data);
                indent_to(pre, ind);
                sb_appendf(pre, "%s %s = %s_has(%s, %s)\n", om, rv, mm, mv, kv);
                indent_to(pre, ind);
                sb_appendf(pre, "    ? (%s){ .tag = %s_TAG_Some, .data.Some._0 = *%s_at(%s, %s) }\n",
                           om, om, mm, mv, kv);
                indent_to(pre, ind);
                sb_appendf(pre, "    : (%s){ .tag = %s_TAG_None };\n", om, om);
                sb_append(out, rv);
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
            if (strcmp(e->sval, "toString") == 0) {
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
            }
            sb_appendf(out, "%s(", e->resolved ? e->resolved : e->sval);
            for (int i = 0; i < e->args.count; i++) {
                if (i) sb_append(out, ", ");
                bool pin = false;
                for (int j = i + 1; j < e->args.count && !pin; j++)
                    pin = has_call(VEC_PTR(&e->args, j, Expr));
                cg_operand(VEC_PTR(&e->args, i, Expr), out, pre, ind, pin);
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
            root_local(sb, ind, s->decl_type, s->name);
            break;
        case ST_ASSIGN: {
            /* `m[k] = v` on a map inserts, so it cannot go through the read path
               (which aborts on a missing key). */
            if (s->target->kind == EX_INDEX && s->target->lhs->type->kind == TY_MAP) {
                SB base; sb_init(&base);
                SB key; sb_init(&key);
                cg_expr(s->target->lhs, &base, &pre, ind);
                cg_expr(s->target->rhs, &key, &pre, ind);
                cg_expr(s->expr, &line, &pre, ind);
                flush(sb, &pre);
                indent_to(sb, ind);
                sb_appendf(sb, "%s_put(%s, %s, %s);\n",
                           ty_mangle(s->target->lhs->type), base.data, key.data, line.data);
                break;
            }
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
                    sb_appendf(sb, "if (%s) {\n", unwrap_cond(cval.data));
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
                sb_appendf(sb, "while (%s) {\n", unwrap_cond(cval.data));
            { indent_to(sb, ind + 1); sb_append(sb, "K_POLL();\n"); }
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
                { indent_to(sb, ind + 1); sb_append(sb, "K_POLL();\n"); }
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
                root_local(sb, ind, s->expr->type, av);
                indent_to(sb, ind);
                sb_appendf(sb, "for (int64_t %s = 0; %s < %s->len; %s++) {\n", idx, idx, av, idx);
                indent_to(sb, ind + 1);
                sb_appendf(sb, "%s %s = %s->data[%s]; (void)%s;\n",
                           c_type(ty_elem(s->expr->type)), s->name, av, idx, s->name);
            }
            { indent_to(sb, ind + 1); sb_append(sb, "K_POLL();\n"); }
            cg_stmts(&s->body, sb, ind + 1);
            indent_to(sb, ind);
            sb_append(sb, "}\n");
            break;
        }
        case ST_RETURN:
            if (s->expr) {
                cg_expr(s->expr, &line, &pre, ind);
                flush(sb, &pre);
                /* The value is computed and copied into a local while the frame is
                   still current, so nothing it points at can be collected between
                   being built and being handed back. */
                indent_to(sb, ind);
                sb_appendf(sb, "{ %s _kret = %s; k_frames = _kf.prev; return _kret; }\n",
                           c_type(g_cg_ret), line.data);
            } else {
                indent_to(sb, ind);
                sb_append(sb, "k_frames = _kf.prev;\n");
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
        case ST_BREAK:
            indent_to(sb, ind);
            sb_append(sb, "break;\n");
            break;
        case ST_CONTINUE:
            indent_to(sb, ind);
            sb_append(sb, "continue;\n");
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
    for (int i = 0; i < body->count; i++) {
        /* A statement boundary is a safepoint: everything the previous statement
           produced is now in a named local, and so in this function's root frame.
           Nothing is half-built here, which is what makes collecting safe. */
        if (i > 0) { indent_to(sb, ind); sb_append(sb, "K_POLL();\n"); }
        cg_stmt(VEC_PTR(body, i, Stmt), sb, ind);
    }
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
/* Threads, behind one tiny interface so the rest of the runtime never sees the
   platform difference. Emitted only when the program actually spawns. */
static const char *THREAD_RUNTIME =
    "/* ── threads ───────────────────────────────────────────────────────── */\n"
    "#if defined(_WIN32)\n"
    "  #include <windows.h>\n"
    "  typedef HANDLE k_thread_t;\n"
    "  typedef CRITICAL_SECTION k_mutex_t;\n"
    "  typedef CONDITION_VARIABLE k_cond_t;\n"
    "  #define K_MUTEX_INIT(m)   InitializeCriticalSection(m)\n"
    "  #define K_LOCK(m)         EnterCriticalSection(m)\n"
    "  #define K_UNLOCK(m)       LeaveCriticalSection(m)\n"
    "  #define K_COND_INIT(c)    InitializeConditionVariable(c)\n"
    "  #define K_WAIT(c, m)      SleepConditionVariableCS(c, m, INFINITE)\n"
    "  #define K_SIGNAL(c)       WakeConditionVariable(c)\n"
    "  #define K_BROADCAST(c)    WakeAllConditionVariable(c)\n"
    "  typedef DWORD k_ret_t;\n"
    "  #define K_THREAD_CALL WINAPI\n"
    "  static int k_thread_start(k_thread_t *t, k_ret_t (K_THREAD_CALL *fn)(void *), void *arg) {\n"
    "      *t = CreateThread(NULL, 0, fn, arg, 0, NULL);\n"
    "      return *t != NULL;\n"
    "  }\n"
    "  static void k_thread_join(k_thread_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }\n"
    "#else\n"
    "  #include <pthread.h>\n"
    "  typedef pthread_t k_thread_t;\n"
    "  typedef pthread_mutex_t k_mutex_t;\n"
    "  typedef pthread_cond_t k_cond_t;\n"
    "  #define K_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)\n"
    "  #define K_LOCK(m)         pthread_mutex_lock(m)\n"
    "  #define K_UNLOCK(m)       pthread_mutex_unlock(m)\n"
    "  #define K_COND_INIT(c)    pthread_cond_init(c, NULL)\n"
    "  #define K_WAIT(c, m)      pthread_cond_wait(c, m)\n"
    "  #define K_SIGNAL(c)       pthread_cond_signal(c)\n"
    "  #define K_BROADCAST(c)    pthread_cond_broadcast(c)\n"
    "  typedef void *k_ret_t;\n"
    "  #define K_THREAD_CALL\n"
    "  static int k_thread_start(k_thread_t *t, k_ret_t (*fn)(void *), void *arg) {\n"
    "      return pthread_create(t, NULL, fn, arg) == 0;\n"
    "  }\n"
    "  static void k_thread_join(k_thread_t t) { pthread_join(t, NULL); }\n"
    "#endif\n\n";

/* Roots the generated code hands over, rather than the collector guessing.
 *
 * Each function that holds anything collectable keeps a small array of memory
 * ranges — one per parameter, local and temporary — and links it into a per-thread
 * list. The collector walks that list instead of the machine stack.
 *
 * A slot is a range rather than a single pointer so that a struct holding three
 * arrays needs one slot, not three: the range is still scanned conservatively, but
 * *which* memory to scan is now exact. That is the part WASM makes non-negotiable,
 * since a value sitting in a WASM local is in no memory the collector can reach.
 */
static const char *ROOT_RUNTIME =
    "/* ── roots ─────────────────────────────────────────────────────────── */\n"
    "/* Build with -DK_PRECISE_ONLY=1 to switch the stack scan off and rely on the\n"
    "   root frames alone — which is what a WASM build does whether it asks to or\n"
    "   not, and so is how the rooting gets tested on a machine that has a stack. */\n"
    "#ifndef K_PRECISE_ONLY\n"
    "  #define K_PRECISE_ONLY 0\n"
    "#endif\n"
    "/* -DK_GC_STRESS=1 collects at every single allocation. Ruinously slow, and the\n"
    "   only way to be sure a root is never missed for the one window it matters. */\n"
    "#ifndef K_GC_STRESS\n"
    "  #define K_GC_STRESS 0\n"
    "#endif\n"
    "typedef struct { void *addr; size_t size; } KRootSlot;\n"
    "typedef struct KFrame { struct KFrame *prev; int n; KRootSlot *slots; } KFrame;\n"
    "#if defined(_MSC_VER)\n"
    "  #define K_TLS __declspec(thread)\n"
    "#elif defined(__GNUC__) || defined(__clang__)\n"
    "  #define K_TLS __thread\n"
    "#else\n"
    "  #define K_TLS   /* single-threaded builds only */\n"
    "#endif\n"
    "#if KLANG_THREADS\n"
    "K_TLS KFrame *k_frames = NULL;\n"
    "#else\n"
    "KFrame *k_frames = NULL;\n"
    "#endif\n"
    "/* Constants live for the whole program and belong to no thread, so they get\n"
    "   their own list that every collection walks. */\n"
    "KFrame *k_global_frames = NULL;\n\n";

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
    "size_t k_live = 0, k_limit = K_GC_STRESS ? 0 : (4u << 20);\n"
    "void *k_stack_bottom = NULL;\n"
    "KObj **k_gray = NULL; size_t k_gray_n = 0, k_gray_cap = 0;\n"
    "size_t k_collections = 0;\n"
    "\n"
    "void k_oom(void) { fprintf(stderr, \"klang: out of memory\\n\"); exit(1); }\n"
    "\n"
    "size_t k_hash(void *p) {\n"
    /* Mixing needs 64 bits: uintptr_t is only 32 on wasm32, where shifting by
       33 would be undefined and the mixing would collapse. */
    "    uint64_t x = (uint64_t)(uintptr_t)p;\n"
    "    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;\n"
    "    x ^= x >> 29; x *= 0xc4ceb9fe1a85ec53ULL;\n"
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
    "\n";

/* With more than one thread the collector must see every thread's stack, and no
   thread may be mutating the heap while it sweeps. Threads reach agreement at
   safepoints: allocation, loop back-edges, and any blocking call. A thread that
   parks records where its stack currently ends and spills its registers, so the
   collector can scan it exactly as it scans its own. */
static const char *GC_THREADS =
    "/* ── stopping the world ────────────────────────────────────────────── */\n"
    "#define K_MAX_THREADS 256\n"
    "typedef struct {\n"
    "    void *bottom;     /* where this thread's stack begins */\n"
    "    void *top;        /* how far it had grown when it parked */\n"
    "    jmp_buf regs;     /* its callee-saved registers, spilled */\n"
    "    KFrame *frames;   /* this thread's root list, captured when it parks */\n"
    "    int in_use, parked;\n"
    "} KThread;\n"
    "KThread k_threads[K_MAX_THREADS];\n"
    "int k_nthreads = 0, k_nparked = 0, k_stop = 0;\n"
    "k_mutex_t k_gc_mutex;\n"
    "k_cond_t k_cond_parked, k_cond_resume;\n"
    "#if defined(_WIN32)\n"
    "  DWORD k_self_key;\n"
    "  #define K_SELF_GET() ((KThread *)TlsGetValue(k_self_key))\n"
    "  #define K_SELF_SET(p) TlsSetValue(k_self_key, p)\n"
    "  #define K_SELF_INIT() (k_self_key = TlsAlloc())\n"
    "#else\n"
    "  pthread_key_t k_self_key;\n"
    "  #define K_SELF_GET() ((KThread *)pthread_getspecific(k_self_key))\n"
    "  #define K_SELF_SET(p) pthread_setspecific(k_self_key, p)\n"
    "  #define K_SELF_INIT() pthread_key_create(&k_self_key, NULL)\n"
    "#endif\n"
    "\n"
    "KThread *klang_thread_register(void *bottom) {\n"
    "    K_LOCK(&k_gc_mutex);\n"
    "    KThread *t = NULL;\n"
    "    for (int i = 0; i < K_MAX_THREADS; i++)\n"
    "        if (!k_threads[i].in_use) { t = &k_threads[i]; break; }\n"
    "    if (!t) { fprintf(stderr, \"klang: too many threads\\n\"); exit(1); }\n"
    "    t->in_use = 1; t->parked = 0; t->bottom = bottom; t->top = bottom;\n"
    "    k_nthreads++;\n"
    "    K_SELF_SET(t);\n"
    "    K_UNLOCK(&k_gc_mutex);\n"
    "    return t;\n"
    "}\n"
    "void klang_thread_unregister(KThread *t) {\n"
    "    K_LOCK(&k_gc_mutex);\n"
    "    t->in_use = 0;\n"
    "    k_nthreads--;\n"
    "    K_SIGNAL(&k_cond_parked);   /* one fewer thread for a collector to wait on */\n"
    "    K_UNLOCK(&k_gc_mutex);\n"
    "}\n"
    "\n"
    "/* Park until the collector is done. Must be called with the GC mutex held. */\n"
    "void k_stop_the_world_and_collect(void);\n"
    "void k_park_locked(KThread *self, void *top) {\n"
    "    while (k_stop) {\n"
    "        if (!self->parked) {\n"
    "            self->top = top; self->frames = k_frames; self->parked = 1; k_nparked++;\n"
    "            K_SIGNAL(&k_cond_parked);\n"
    "        }\n"
    "        K_WAIT(&k_cond_resume, &k_gc_mutex);\n"
    "    }\n"
    "    if (self->parked) { self->parked = 0; k_nparked--; }\n"
    "}\n"
    /* The only place a collection can begin: between statements and at loop
       back-edges, where every live value is reachable from a root frame. It is
       also where a thread yields to a collection somebody else started. */
    "void klang_safepoint(void) {\n"
    "    KThread *self = K_SELF_GET();\n"
    "    if (!self) return;\n"
    "    jmp_buf regs;\n"
    "    memset(&regs, 0, sizeof regs);\n"
    "    setjmp(regs);\n"
    "    memcpy(self->regs, regs, sizeof regs);\n"
    "    K_LOCK(&k_gc_mutex);\n"
    "    k_park_locked(self, &regs);\n"
    "    if (k_live > k_limit) k_stop_the_world_and_collect();\n"
    "    K_UNLOCK(&k_gc_mutex);\n"
    "}\n"
    "/* Around a blocking call: the thread holds no live registers the collector\n"
    "   cannot already see on its stack, so it can be scanned while it waits. */\n"
    "void klang_gc_block_enter(void) {\n"
    "    KThread *self = K_SELF_GET();\n"
    "    if (!self) return;\n"
    "    jmp_buf regs;\n"
    "    memset(&regs, 0, sizeof regs);\n"
    "    setjmp(regs);\n"
    "    memcpy(self->regs, regs, sizeof regs);\n"
    "    K_LOCK(&k_gc_mutex);\n"
    "    self->top = &regs; self->frames = k_frames; self->parked = 1; k_nparked++;\n"
    "    K_SIGNAL(&k_cond_parked);\n"
    "    K_UNLOCK(&k_gc_mutex);\n"
    "}\n"
    "void klang_gc_block_exit(void) {\n"
    "    KThread *self = K_SELF_GET();\n"
    "    if (!self) return;\n"
    "    K_LOCK(&k_gc_mutex);\n"
    "    if (self->parked) { self->parked = 0; k_nparked--; }\n"
    "    k_park_locked(self, &self);   /* a collection may have started meanwhile */\n"
    "    K_UNLOCK(&k_gc_mutex);\n"
    "}\n\n";

/* Emitted after the thread machinery, because collecting has to know about it. */
static const char *GC_COLLECT =
    "/* Every range the generated code registered, up the whole frame list. */\n"
    "void k_scan_frames(KFrame *f) {\n"
    "    for (; f; f = f->prev)\n"
    "        for (int i = 0; i < f->n; i++)\n"
    "            if (f->slots[i].addr)\n"
    "                k_scan(f->slots[i].addr, (char *)f->slots[i].addr + f->slots[i].size);\n"
    "}\n"
    "/* Mark from every root, sweep, rebuild the address set. The caller has already\n"
    "   made sure no other thread is running Klang code. */\n"
    "void k_collect_core(void) {\n"
    "    jmp_buf regs;\n"
    "    memset(&regs, 0, sizeof regs);\n"
    "    setjmp(regs);                  /* force callee-saved registers onto the stack */\n"
    "    for (KObj *o = k_objs; o; o = o->next) o->mark = 0;\n"
    "    k_gray_n = 0;\n"
    "#if !K_PRECISE_ONLY\n"
    "    k_scan(&regs, (char *)&regs + sizeof regs);\n"
    "#endif\n"
    /* The roots generated code declared. This alone is enough to be correct; the
       stack scan below is belt and braces on targets that have a real stack. */
    "    k_scan_frames(k_frames);\n"
    "    k_scan_frames(k_global_frames);\n"
    "#if KLANG_THREADS\n"
    "    {\n"
    "        KThread *self = K_SELF_GET();\n"
    "#if !K_PRECISE_ONLY\n"
    "        k_scan((void *)&regs, self ? self->bottom : k_stack_bottom);\n"
    "#endif\n"
    "        for (int i = 0; i < K_MAX_THREADS; i++) {\n"
    "            KThread *t = &k_threads[i];\n"
    "            if (!t->in_use || t == self || !t->parked) continue;\n"
    "            k_scan_frames(t->frames);\n"
    "#if !K_PRECISE_ONLY\n"
    "            k_scan(t->top, t->bottom);\n"
    "            k_scan(t->regs, (char *)t->regs + sizeof t->regs);\n"
    "#endif\n"
    "        }\n"
    "    }\n"
    "#elif !defined(__wasm__) && !K_PRECISE_ONLY\n"
    "    k_scan((void *)&regs, k_stack_bottom);\n"
    "#endif\n"
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
    "#if K_GC_STRESS\n"
    "    k_limit = 0;                   /* collect again at the very next allocation */\n"
    "#else\n"
    "    k_limit = live * 2 < (4u << 20) ? (4u << 20) : live * 2;\n"
    "#endif\n"
    "    k_collections++;\n"
    "}\n"
    "\n"
    "#if KLANG_THREADS\n"
    "/* Bring every other thread to a stop, collect, let them go. */\n"
    "void k_stop_the_world_and_collect(void) {\n"
    "    k_stop = 1;\n"
    "    while (k_nparked < k_nthreads - 1) K_WAIT(&k_cond_parked, &k_gc_mutex);\n"
    "    k_collect_core();\n"
    "    k_stop = 0;\n"
    "    K_BROADCAST(&k_cond_resume);\n"
    "}\n"
    "void klang_gc_collect(void) {\n"
    "    KThread *self = K_SELF_GET();\n"
    "    K_LOCK(&k_gc_mutex);\n"
    "    if (self) k_park_locked(self, &self);   /* someone else may be collecting */\n"
    "    k_stop_the_world_and_collect();\n"
    "    K_UNLOCK(&k_gc_mutex);\n"
    "}\n"
    "#else\n"
    "void klang_gc_collect(void) {\n"
    "    if (!k_stack_bottom) return;   /* before main() set the anchor */\n"
    "    k_collect_core();\n"
    "}\n"
    "#endif\n"
    "\n"
    "void *k_alloc_raw(size_t n) {\n"
    "    KObj *o = malloc(sizeof(KObj) + n);\n"
    "    if (!o) k_oom();\n"
    "    o->size = n; o->mark = 0; o->next = k_objs; k_objs = o;\n"
    "    void *pl = KPAY(o);\n"
    "    if (!k_lo || (char *)pl < k_lo) k_lo = pl;\n"
    "    if (!k_hi || (char *)pl > k_hi) k_hi = pl;\n"
    "    k_tab_add(pl);\n"
    "    k_live += n;\n"
    "    return pl;\n"
    "}\n"
    /* Allocation never collects.
     *
     * It is tempting to collect here, since it is the one place that knows the
     * heap has grown — and that is what this collector used to do. But a runtime
     * helper like `arr_new` allocates a header and then allocates its buffer, and
     * between the two the header exists only in a C local of that helper, which is
     * in no root frame. Collecting inside the second allocation frees it.
     *
     * So collection happens only at safepoints, which generated code emits between
     * statements and at loop back-edges — places where every live value is in a
     * frame by construction. Allocation just records the growth. */
    "void *klang_gc_alloc(size_t n) {\n"
    "#if KLANG_THREADS\n"
    "    K_LOCK(&k_gc_mutex);\n"
    "    void *pl = k_alloc_raw(n);\n"
    "    K_UNLOCK(&k_gc_mutex);\n"
    "    return pl;\n"
    "#else\n"
    "    return k_alloc_raw(n);\n"
    "#endif\n"
    "}\n"
    "void *klang_gc_grow(void *p, size_t n) {\n"
    "    void *q = klang_gc_alloc(n);\n"
    "    if (p) { size_t old = KHDR(p)->size; memcpy(q, p, old < n ? old : n); }\n"
    "    return q;\n"
    "}\n"
    "void klang_gc_init(void *bottom) {\n"
    "    k_stack_bottom = bottom;\n"
    "#if KLANG_THREADS\n"
    "    K_MUTEX_INIT(&k_gc_mutex);\n"
    "    K_COND_INIT(&k_cond_parked);\n"
    "    K_COND_INIT(&k_cond_resume);\n"
    "    K_SELF_INIT();\n"
    "    klang_thread_register(bottom);\n"
    "#endif\n"
    "}\n"
    "int64_t klang_gc_heap(void) { return (int64_t)k_live; }\n"
    "#if !KLANG_THREADS\n"
    "void klang_safepoint(void) { if (k_live > k_limit) klang_gc_collect(); }\n"
    "#define K_POLL() do { if (k_live > k_limit) klang_safepoint(); } while (0)\n"
    "#else\n"
    "#define K_POLL() do { if (k_live > k_limit || k_stop) klang_safepoint(); } while (0)\n"
    "#endif\n"
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
    "uint64_t klang_hash_int(int64_t v) {\n"
    "    uint64_t x = (uint64_t)v;\n"
    "    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;\n"
    "    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;\n"
    "    return x ^ (x >> 33);\n"
    "}\n"
    "uint64_t klang_hash_str(const char *s) {\n"
    "    uint64_t h = 1469598103934665603ULL;\n"
    "    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }\n"
    "    return h;\n"
    "}\n"
    /* Signed overflow is undefined behaviour in C, so it cannot be left to chance:
       every int operation is checked. GCC and Clang expose the hardware's own
       overflow flag, which costs a single not-taken branch; the portable fallback
       tests the operands first so any C99 compiler still builds this. */
    "void klang_overflow(const char *op) {\n"
    "    fprintf(stderr, \"klang: integer overflow in '%s'\\n\", op);\n"
    "    exit(1);\n"
    "}\n"
    "void klang_divzero(const char *op) {\n"
    "    fprintf(stderr, \"klang: %s by zero\\n\", op);\n"
    "    exit(1);\n"
    "}\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "  #define KLANG_CHK(fn, builtin, opname)                                      \\\n"
    "    int64_t fn(int64_t a, int64_t b) {                                        \\\n"
    "        int64_t r;                                                            \\\n"
    "        if (builtin(a, b, &r)) klang_overflow(opname);                        \\\n"
    "        return r;                                                             \\\n"
    "    }\n"
    "KLANG_CHK(klang_add, __builtin_add_overflow, \"+\")\n"
    "KLANG_CHK(klang_sub, __builtin_sub_overflow, \"-\")\n"
    "KLANG_CHK(klang_mul, __builtin_mul_overflow, \"*\")\n"
    "#else\n"
    "int64_t klang_add(int64_t a, int64_t b) {\n"
    "    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) klang_overflow(\"+\");\n"
    "    return a + b;\n"
    "}\n"
    "int64_t klang_sub(int64_t a, int64_t b) {\n"
    "    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) klang_overflow(\"-\");\n"
    "    return a - b;\n"
    "}\n"
    "int64_t klang_mul(int64_t a, int64_t b) {\n"
    "    if (a != 0 && b != 0) {\n"
    "        if (a > 0 ? (b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a)\n"
    "                  : (b > 0 ? a < INT64_MIN / b : a < INT64_MAX / b)) klang_overflow(\"*\");\n"
    "    }\n"
    "    return a * b;\n"
    "}\n"
    "#endif\n"
    "int64_t klang_neg(int64_t a) {\n"
    "    if (a == INT64_MIN) klang_overflow(\"-\");\n"
    "    return -a;\n"
    "}\n"
    /* INT64_MIN / -1 overflows, and division by zero traps on most hardware; both
       become a message rather than a signal. */
    "int64_t klang_div(int64_t a, int64_t b) {\n"
    "    if (b == 0) klang_divzero(\"division\");\n"
    "    if (a == INT64_MIN && b == -1) klang_overflow(\"/\");\n"
    "    return a / b;\n"
    "}\n"
    "int64_t klang_mod(int64_t a, int64_t b) {\n"
    "    if (b == 0) klang_divzero(\"remainder\");\n"
    "    if (a == INT64_MIN && b == -1) return 0;\n"
    "    return a % b;\n"
    "}\n"
    "int64_t klang_wrap_add(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }\n"
    "int64_t klang_wrap_sub(int64_t a, int64_t b) { return (int64_t)((uint64_t)a - (uint64_t)b); }\n"
    "int64_t klang_wrap_mul(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }\n"
    /* Out-of-range and NaN conversions are undefined in C, so they saturate here. */
    "int64_t klang_to_int(double d) {\n"
    "    if (d != d) return 0;\n"
    "    if (d >= 9223372036854775808.0) return INT64_MAX;\n"
    "    if (d <= -9223372036854775808.0) return INT64_MIN;\n"
    "    return (int64_t)d;\n"
    "}\n"
    "double klang_to_float(int64_t v) { return (double)v; }\n"
    "void klang_bounds(int64_t i, int64_t len);\n"
    "/* Byte offsets, clamped rather than trusted, so slicing never reads out of range. */\n"
    "char *klang_substr(const char *s, int64_t start, int64_t end) {\n"
    "    int64_t n = (int64_t)strlen(s);\n"
    "    if (start < 0) start = 0;\n"
    "    if (end > n) end = n;\n"
    "    if (start >= end) return klang_strdup(\"\");\n"
    "    size_t len = (size_t)(end - start);\n"
    "    char *r = klang_gc_alloc(len + 1);\n"
    "    memcpy(r, s + start, len); r[len] = 0;\n"
    "    return r;\n"
    "}\n"
    "int64_t klang_byte_at(const char *s, int64_t i) {\n"
    "    int64_t n = (int64_t)strlen(s);\n"
    "    if (i < 0 || i >= n) klang_bounds(i, n);\n"
    "    return (int64_t)(unsigned char)s[i];\n"
    "}\n"
    "char *klang_from_byte(int64_t code) {\n"
    "    char *r = klang_gc_alloc(2);\n"
    "    r[0] = (char)(code & 0xff); r[1] = 0;\n"
    "    return r;\n"
    "}\n"
    "int64_t klang_index_of(const char *s, const char *needle) {\n"
    "    if (!*needle) return 0;\n"
    "    const char *hit = strstr(s, needle);\n"
    "    return hit ? (int64_t)(hit - s) : -1;\n"
    "}\n"
    "void klang_poke(void *p, int64_t off, int64_t v) { ((unsigned char *)p)[off] = (unsigned char)v; }\n"
    "int64_t klang_peek(void *p, int64_t off) { return (int64_t)((unsigned char *)p)[off]; }\n"
    "void klang_no_key(void) {\n"
    "    fprintf(stderr, \"klang: no such key in map\\n\");\n"
    "    exit(1);\n"
    "}\n"
    "void klang_bounds(int64_t i, int64_t len) {\n"
    "    fprintf(stderr, \"klang: index %\" PRId64 \" is out of bounds for an array of length %\" PRId64 \"\\n\", i, len);\n"
    "    exit(1);\n"
    "}\n";

/* ── the JavaScript boundary ──────────────────────────────────────────────
 *
 * JavaScript has one number type, a float64, so an int has to be handed across
 * as a double. That is lossy above 2^53, and a language that checks integer
 * overflow has no business quietly rounding here instead: both directions are
 * checked and stop with a message rather than lying.
 *
 * Strings cross by copying. JavaScript's own string lives in the JS heap and its
 * UTF-8 form is malloc'd by `stringToNewUTF8`, so the wrapper copies that into
 * GC memory and frees it — a Klang string always belongs to the collector.
 */
static const char *JS_RUNTIME =
    "/* ── JavaScript boundary ───────────────────────────────────────────── */\n"
    "#ifdef __EMSCRIPTEN__\n"
    "#include <emscripten.h>\n"
    "#else\n"
    "#define EMSCRIPTEN_KEEPALIVE\n"
    "#endif\n"
    "#define K_JS_EXACT 9007199254740992.0   /* 2^53: above this, doubles skip integers */\n"
    "double klang_js_num(int64_t v, const char *fn) {\n"
    "    if ((double)v > K_JS_EXACT || (double)v < -K_JS_EXACT) {\n"
    "        fprintf(stderr, \"klang: %\" PRId64 \" cannot be passed to JavaScript by '%s' \"\n"
    "                        \"without losing precision — JavaScript numbers are exact \"\n"
    "                        \"only up to 2^53\\n\", v, fn);\n"
    "        exit(1);\n"
    "    }\n"
    "    return (double)v;\n"
    "}\n"
    "int64_t klang_js_int(double d, const char *fn) {\n"
    "    if (!(d == d) || d > K_JS_EXACT || d < -K_JS_EXACT || d != (double)(int64_t)d) {\n"
    "        fprintf(stderr, \"klang: '%s' returned %g, which is not an int JavaScript \"\n"
    "                        \"can represent exactly\\n\", fn, d);\n"
    "        exit(1);\n"
    "    }\n"
    "    return (int64_t)d;\n"
    "}\n"
    /* An exported function's string arguments arrive in memory JavaScript owns —
       Module.ccall allocates them on the stack and releases them the moment the
       call returns. A Klang string has to outlive that, since the callee may well
       store it, so it is copied in on the way. */
    "char *klang_js_own(const char *raw) {\n"
    "    if (!raw) return klang_gc_alloc(1);\n"
    "    size_t n = strlen(raw) + 1;\n"
    "    char *out = klang_gc_alloc(n);\n"
    "    memcpy(out, raw, n);\n"
    "    return out;\n"
    "}\n"
    "/* Takes ownership of a malloc'd UTF-8 string and hands back a GC-owned copy. */\n"
    "char *klang_js_str(char *raw) {\n"
    "    if (!raw) return klang_gc_alloc(1);\n"
    "    size_t n = strlen(raw) + 1;\n"
    "    char *out = klang_gc_alloc(n);\n"
    "    memcpy(out, raw, n);\n"
    "    free(raw);\n"
    "    return out;\n"
    "}\n"
    "#ifndef __EMSCRIPTEN__\n"
    "void klang_no_js(const char *fn) {\n"
    "    fprintf(stderr, \"klang: '%s' is a JavaScript function, and this build has no \"\n"
    "                    \"JavaScript host — compile it with emcc\\n\", fn);\n"
    "    exit(1);\n"
    "}\n"
    "#endif\n\n";

/* Arrays are heap objects behind a pointer, so pushing through one binding is
   visible through every other binding — no surprise copies. */
/* An array is a pointer, so naming one needs nothing at all — this is what lets a
   type contain an array of itself. */
static void emit_array_decl(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "typedef struct %s_s *%s;\n", m, m);
}
static void emit_array_def(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "struct %s_s { int64_t len; int64_t cap; %s *data; };\n", m, c_type(ty_elem(t)));
}
static void emit_array(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    const char *e = c_type(ty_elem(t));
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
                   "    a->data[a->len++] = v;\n}\n", m, m, e, e, e);
    /* Removal keeps order, because a list that reshuffles itself when you delete
       from it is a surprise nobody wants. The vacated slot is cleared so the
       collector does not keep the last element alive through the tail. */
    sb_appendf(sb, "void %s_remove(%s a, int64_t i) {\n"
                   "    if (i < 0 || i >= a->len) klang_bounds(i, a->len);\n"
                   "    for (int64_t j = i; j + 1 < a->len; j++) a->data[j] = a->data[j + 1];\n"
                   "    a->len--;\n"
                   "    memset(&a->data[a->len], 0, sizeof(%s));\n}\n\n", m, m, e);
}

/* Deep-copies whatever crosses a thread boundary. Numbers and strings are
   immutable, so they are shared as they are; arrays, maps, structs and enums are
   rebuilt, which is what makes shared mutable state impossible rather than merely
   discouraged. Emitted per type, so the copy is direct with no dispatch. */
static void copy_expr(const Type *t, const char *val, SB *out) {
    switch (t->kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING:
            sb_append(out, val);        /* immutable: sharing is safe */
            return;
        case TY_ARRAY: case TY_MAP: case TY_NAMED:
            sb_appendf(out, "%s_copy(%s)", ty_mangle(t), val);
            return;
        default:
            sb_append(out, val);
            return;
    }
}

static void emit_array_copy(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "%s %s_copy(%s a) {\n"
                   "    %s c = %s_new(a->len);\n"
                   "    for (int64_t i = 0; i < a->len; i++) c->data[i] = ", m, m, m, m, m);
    copy_expr(ty_elem(t), "a->data[i]", sb);
    sb_append(sb, ";\n    return c;\n}\n");
}
static void emit_map_copy(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "%s %s_copy(%s d) {\n"
                   "    %s c = %s_new();\n"
                   "    for (int64_t i = 0; i < d->cap; i++) {\n"
                   "        if (d->slots[i].state != 1) continue;\n"
                   "        %s_put(c, ", m, m, m, m, m, m);
    copy_expr(ty_key(t), "d->slots[i].key", sb);
    sb_append(sb, ", ");
    copy_expr(ty_val(t), "d->slots[i].val", sb);
    sb_append(sb, ");\n    }\n    return c;\n}\n");
}
static void emit_struct_copy(StructDecl *sd, SB *sb) {
    sb_appendf(sb, "%s %s_copy(%s v) {\n    %s c;\n", sd->mangled, sd->mangled, sd->mangled, sd->mangled);
    if (sd->fields.count == 0) sb_append(sb, "    c._empty = v._empty;\n");
    for (int i = 0; i < sd->fields.count; i++) {
        Field *f = vec_get(&sd->fields, i);
        char field[128];
        snprintf(field, sizeof field, "v.%s", f->name);
        sb_appendf(sb, "    c.%s = ", f->name);
        copy_expr(f->type, field, sb);
        sb_append(sb, ";\n");
    }
    sb_append(sb, "    return c;\n}\n");
}
static void emit_enum_copy(EnumDecl *ed, SB *sb) {
    sb_appendf(sb, "%s %s_copy(%s v) {\n    %s c = v;\n", ed->mangled, ed->mangled, ed->mangled, ed->mangled);
    for (int i = 0; i < ed->variants.count; i++) {
        Variant *var = vec_get(&ed->variants, i);
        if (var->payload.count == 0) continue;
        sb_appendf(sb, "    if (v.tag == %s_TAG_%s) {\n", ed->mangled, var->name);
        for (int j = 0; j < var->payload.count; j++) {
            char field[160];
            snprintf(field, sizeof field, "v.data.%s._%d", var->name, j);
            sb_appendf(sb, "        c.data.%s._%d = ", var->name, j);
            copy_expr(VEC_PTR(&var->payload, j, Type), field, sb);
            sb_append(sb, ";\n");
        }
        sb_append(sb, "    }\n");
    }
    sb_append(sb, "    return c;\n}\n");
}

/* One task type per result type: the thread, its result, and a join that hands
   ownership of the result to whoever awaited it. */
static void emit_task_decl(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "typedef struct %s_s *%s;\n", m, m);
}
static void emit_task(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    const char *r = c_type(ty_elem(t));
    sb_appendf(sb, "struct %s_s {\n"
                   "    k_thread_t th;\n"
                   "    %s (*fn)(void *);\n"
                   "    void *env;\n"
                   "    %s result;\n"
                   "    int joined;\n"
                   "};\n", m, r, r);
    sb_appendf(sb, "k_ret_t K_THREAD_CALL %s_run(void *arg) {\n"
                   "    %s t = arg;\n"
                   "    int anchor = 0;\n"
                   "    KThread *self = klang_thread_register(&anchor);\n"
                   "    t->result = t->fn(t->env);\n"
                   "    klang_thread_unregister(self);\n"
                   "    return 0;\n"
                   "}\n", m, m);
    sb_appendf(sb, "%s %s_spawn(%s (*fn)(void *), void *env) {\n"
                   "    %s t = klang_gc_alloc(sizeof(struct %s_s));\n"
                   "    t->fn = fn; t->env = env; t->joined = 0;\n"
                   "    memset(&t->result, 0, sizeof t->result);\n"
                   "    if (!k_thread_start(&t->th, %s_run, t)) {\n"
                   "        fprintf(stderr, \"klang: cannot start a thread\\n\");\n"
                   "        exit(1);\n"
                   "    }\n"
                   "    return t;\n"
                   "}\n", m, m, r, m, m, m);
    /* Joining blocks, so the thread parks first — otherwise a collection started
       by another thread would wait forever on one sitting in pthread_join. */
    sb_appendf(sb, "%s %s_await(%s t) {\n"
                   "    if (!t->joined) {\n"
                   "        klang_gc_block_enter();\n"
                   "        k_thread_join(t->th);\n"
                   "        klang_gc_block_exit();\n"
                   "        t->joined = 1;\n"
                   "    }\n"
                   "    return t->result;\n"
                   "}\n\n", r, m, m);
}

/* A closure is a code pointer plus an environment pointer, passed by value. The
   environment is GC-allocated, so the collector traces whatever was captured
   without needing to be told anything about it. */
static void emit_fntype_decl(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "typedef struct %s %s;\n", m, m);
}
static void emit_fntype(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "struct %s { %s (*fn)(void *", m, c_type(ty_ret(t)));
    for (int i = 0; i < ty_nparams(t); i++) sb_appendf(sb, ", %s", c_type(ty_param(t, i)));
    sb_append(sb, "); void *env; };\n\n");
}

/* Open-addressed hash map with linear probing and tombstones. One is emitted per
   distinct (key, value) pair, so lookups compare and hash concrete types with no
   indirection through function pointers. */
static void emit_map_decl(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "typedef struct %s_s *%s;\n", m, m);
}
static void emit_map_def(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    sb_appendf(sb, "struct %s_slot { %s key; %s val; unsigned char state; };\n",
               m, c_type(ty_key(t)), c_type(ty_val(t)));
    sb_appendf(sb, "struct %s_s { int64_t len; int64_t cap; struct %s_slot *slots; };\n", m, m);
}
static void emit_map(const Type *t, SB *sb) {
    char *m = ty_mangle(t);
    const Type *kt = ty_key(t);
    const char *k = c_type(kt);
    const char *v = c_type(ty_val(t));
    char *karr = ty_mangle(ty_array(ty_key(t)));
    char *varr = ty_mangle(ty_array(ty_val(t)));
    const char *hash = kt->kind == TY_STRING ? "klang_hash_str" : "klang_hash_int";
    const char *cast = kt->kind == TY_STRING ? "" : "(int64_t)";
    const char *eq   = kt->kind == TY_STRING ? "klang_str_eq(a, b)" : "a == b";

    sb_appendf(sb, "int %s_keq(%s a, %s b) { return %s; }\n", m, k, k, eq);
    sb_appendf(sb, "%s %s_new(void) {\n"
                   "    %s d = klang_gc_alloc(sizeof(struct %s_s));\n"
                   "    d->len = 0; d->cap = 8; d->slots = NULL;\n"
                   "    d->slots = klang_gc_alloc(sizeof(struct %s_slot) * 8);\n"
                   "    memset(d->slots, 0, sizeof(struct %s_slot) * 8);\n"
                   "    return d;\n}\n", m, m, m, m, m, m);
    /* Returns the slot holding `key`, or the first free slot it could go in. */
    sb_appendf(sb, "int64_t %s_probe(%s d, %s key) {\n"
                   "    int64_t mask = d->cap - 1;\n"
                   "    int64_t i = (int64_t)(%s(%skey) & (uint64_t)mask);\n"
                   "    int64_t first_free = -1;\n"
                   "    while (d->slots[i].state) {\n"
                   "        if (d->slots[i].state == 1 && %s_keq(d->slots[i].key, key)) return i;\n"
                   "        if (d->slots[i].state == 2 && first_free < 0) first_free = i;\n"
                   "        i = (i + 1) & mask;\n"
                   "    }\n"
                   "    return first_free >= 0 ? first_free : i;\n}\n", m, m, k, hash, cast, m);
    sb_appendf(sb, "void %s_put(%s d, %s key, %s val);\n", m, m, k, v);
    sb_appendf(sb, "void %s_regrow(%s d) {\n"
                   "    int64_t oldcap = d->cap;\n"
                   "    struct %s_slot *old = d->slots;\n"
                   "    struct %s_slot *ns = klang_gc_alloc(sizeof(struct %s_slot) * (size_t)(oldcap * 2));\n"
                   "    memset(ns, 0, sizeof(struct %s_slot) * (size_t)(oldcap * 2));\n"
                   "    d->slots = ns; d->cap = oldcap * 2; d->len = 0;\n"
                   "    for (int64_t i = 0; i < oldcap; i++)\n"
                   "        if (old[i].state == 1) %s_put(d, old[i].key, old[i].val);\n"
                   "}\n", m, m, m, m, m, m, m);
    sb_appendf(sb, "void %s_put(%s d, %s key, %s val) {\n"
                   "    if ((d->len + 1) * 10 >= d->cap * 7) %s_regrow(d);\n"
                   "    int64_t i = %s_probe(d, key);\n"
                   "    if (d->slots[i].state != 1) d->len++;\n"
                   "    d->slots[i].key = key; d->slots[i].val = val; d->slots[i].state = 1;\n"
                   "}\n", m, m, k, v, m, m);
    sb_appendf(sb, "int %s_has(%s d, %s key) {\n"
                   "    int64_t i = %s_probe(d, key);\n"
                   "    return d->slots[i].state == 1;\n}\n", m, m, k, m);
    sb_appendf(sb, "%s *%s_at(%s d, %s key) {\n"
                   "    int64_t i = %s_probe(d, key);\n"
                   "    if (d->slots[i].state != 1) klang_no_key();\n"
                   "    return &d->slots[i].val;\n}\n", v, m, m, k, m);
    sb_appendf(sb, "void %s_remove(%s d, %s key) {\n"
                   "    int64_t i = %s_probe(d, key);\n"
                   "    if (d->slots[i].state == 1) { d->slots[i].state = 2; d->len--; }\n"
                   "}\n", m, m, k, m);
    sb_appendf(sb, "%s %s_keys(%s d) {\n"
                   "    %s out = %s_new(0);\n"
                   "    for (int64_t i = 0; i < d->cap; i++)\n"
                   "        if (d->slots[i].state == 1) %s_push(out, d->slots[i].key);\n"
                   "    return out;\n}\n", karr, m, m, karr, karr, karr);
    sb_appendf(sb, "%s %s_values(%s d) {\n"
                   "    %s out = %s_new(0);\n"
                   "    for (int64_t i = 0; i < d->cap; i++)\n"
                   "        if (d->slots[i].state == 1) %s_push(out, d->slots[i].val);\n"
                   "    return out;\n}\n\n", varr, m, m, varr, varr, varr);
}

/* Collect the mangled names of named types a mono type embeds by value. */
/* Only types stored *by value* inside another force an ordering. Arrays, maps and
   tasks are pointers to a heap object, so holding one needs nothing but the name —
   which is why `enum Json { Arr([Json]) }` is not a cycle even though it looks like
   one. Structs, enums and closures are stored inline and must be complete. */
static void collect_deps(const Type *t, Vec *out) {
    if (t->kind != TY_NAMED && t->kind != TY_FN) return;
    VEC_PUSH_PTR(out, ty_mangle(t));
}

static void emit_struct_decl(StructDecl *sd, SB *sb) {
    if (sd->is_opaque) sb_appendf(sb, "typedef void *%s;\n", sd->mangled);
    else sb_appendf(sb, "typedef struct %s %s;\n", sd->mangled, sd->mangled);
}
static void emit_struct(StructDecl *sd, SB *sb) {
    if (sd->is_opaque) return;   /* nothing to define: it is a bare pointer */
    sb_appendf(sb, "struct %s {\n", sd->mangled);
    for (int i = 0; i < sd->fields.count; i++) {
        Field *f = vec_get(&sd->fields, i);
        sb_appendf(sb, "    %s %s;\n", c_type(f->type), f->name);
    }
    if (sd->fields.count == 0) sb_append(sb, "    char _empty;\n");
    sb_append(sb, "};\n\n");
}

static void emit_enum_decl(EnumDecl *ed, SB *sb) {
    sb_append(sb, "enum {");
    for (int i = 0; i < ed->variants.count; i++) {
        Variant *v = vec_get(&ed->variants, i);
        sb_appendf(sb, "%s %s_TAG_%s = %d", i ? "," : "", ed->mangled, v->name, i);
    }
    sb_append(sb, " };\n");
    sb_appendf(sb, "typedef struct %s %s;\n", ed->mangled, ed->mangled);
}
static void emit_enum(EnumDecl *ed, SB *sb) {
    sb_appendf(sb, "struct %s {\n    int tag;\n", ed->mangled);
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
    sb_append(sb, "};\n\n");
}

/* Types are written in three passes.
 *
 *   1. Names.  Every type gets a typedef that says nothing about its contents, so
 *      anything may refer to anything by name from here on.
 *   2. Layouts. Emitted in dependency order, where the only dependency that counts
 *      is being stored *by value*: a struct field, an enum payload, a map slot, a
 *      task's result. An array of T needs nothing but T's name, which is exactly
 *      why `enum Json { Arr([Json]) }` is legal — the recursion goes through a
 *      pointer. A type that genuinely contains itself by value is still an error.
 *   3. Functions. Everything is complete by now, so sizeof and field access work.
 */
static void emit_types(SB *sb) {
    int n_s = g_mono_structs.count, n_e = g_mono_enums.count;
    int n_a = g_mono_arrays.count, n_m = g_mono_maps.count;
    int n_f = g_mono_fntypes.count, n_t = g_mono_tasks.count;

    /* ── 1. names ── */
    for (int i = 0; i < n_s; i++) emit_struct_decl(VEC_PTR(&g_mono_structs, i, StructDecl), sb);
    for (int i = 0; i < n_e; i++) emit_enum_decl(VEC_PTR(&g_mono_enums, i, EnumDecl), sb);
    for (int i = 0; i < n_a; i++) emit_array_decl(VEC_PTR(&g_mono_arrays, i, Type), sb);
    for (int i = 0; i < n_m; i++) emit_map_decl(VEC_PTR(&g_mono_maps, i, Type), sb);
    for (int i = 0; i < n_f; i++) emit_fntype_decl(VEC_PTR(&g_mono_fntypes, i, Type), sb);
    for (int i = 0; i < n_t; i++) emit_task_decl(VEC_PTR(&g_mono_tasks, i, Type), sb);
    sb_append(sb, "\n");

    /* ── 2. layouts, in dependency order ── */
    int total = n_s + n_e + n_m + n_f + n_t;
    bool *done = calloc((size_t)(total ? total : 1), sizeof(bool));
    Vec emitted; vec_init(&emitted, sizeof(char *));

    for (int pass = 0; pass < total + 1; pass++) {
        bool progress = false;
        for (int i = 0; i < total; i++) {
            if (done[i]) continue;
            int k = i;
            StructDecl *sd = k < n_s ? VEC_PTR(&g_mono_structs, k, StructDecl) : NULL;
            k -= n_s;
            EnumDecl *ed = (!sd && k < n_e) ? VEC_PTR(&g_mono_enums, k, EnumDecl) : NULL;
            k -= n_e;
            Type *mt = (!sd && !ed && k < n_m) ? VEC_PTR(&g_mono_maps, k, Type) : NULL;
            k -= n_m;
            Type *ft = (!sd && !ed && !mt && k < n_f) ? VEC_PTR(&g_mono_fntypes, k, Type) : NULL;
            k -= n_f;
            Type *tt = (!sd && !ed && !mt && !ft) ? VEC_PTR(&g_mono_tasks, k, Type) : NULL;

            const char *mangled = sd ? sd->mangled : ed ? ed->mangled
                                : ty_mangle(mt ? mt : ft ? ft : tt);
            const char *shown = sd ? key_show(sd->key) : ed ? key_show(ed->key)
                              : ty_str(mt ? mt : ft ? ft : tt);

            Vec deps; vec_init(&deps, sizeof(char *));
            if (sd) {
                for (int j = 0; j < sd->fields.count; j++)
                    collect_deps(((Field *)vec_get(&sd->fields, j))->type, &deps);
            } else if (ed) {
                for (int j = 0; j < ed->variants.count; j++) {
                    Variant *v = vec_get(&ed->variants, j);
                    for (int q = 0; q < v->payload.count; q++)
                        collect_deps(VEC_PTR(&v->payload, q, Type), &deps);
                }
            } else if (mt) {
                collect_deps(ty_key(mt), &deps);      /* slots hold both inline */
                collect_deps(ty_val(mt), &deps);
            } else if (ft) {
                for (int j = 0; j < ft->args.count; j++)
                    collect_deps(VEC_PTR(&ft->args, j, Type), &deps);
            } else {
                collect_deps(ty_elem(tt), &deps);     /* the task holds its result */
            }

            bool ready = true;
            for (int j = 0; j < deps.count && ready; j++) {
                char *dep = VEC_PTR(&deps, j, char);
                if (strcmp(dep, mangled) == 0)
                    fail(0, "type %s contains itself by value — put the recursive part "
                            "behind an array, as in [%s], which is a reference",
                         shown, shown);
                bool found = false;
                for (int q = 0; q < emitted.count; q++)
                    if (strcmp(VEC_PTR(&emitted, q, char), dep) == 0) { found = true; break; }
                if (!found) ready = false;
            }
            if (!ready) continue;

            if (sd) emit_struct(sd, sb);
            else if (ed) emit_enum(ed, sb);
            else if (mt) emit_map_def(mt, sb);
            else if (ft) emit_fntype(ft, sb);
            else emit_task(tt, sb);
            VEC_PUSH_PTR(&emitted, (char *)mangled);
            done[i] = true;
            progress = true;
        }
        if (!progress) break;
    }
    for (int i = 0; i < total; i++) {
        if (done[i]) continue;
        int k = i;
        const char *m = k < n_s ? VEC_PTR(&g_mono_structs, k, StructDecl)->mangled
                      : (k -= n_s) < n_e ? VEC_PTR(&g_mono_enums, k, EnumDecl)->mangled
                      : (k -= n_e) < n_m ? ty_mangle(VEC_PTR(&g_mono_maps, k, Type))
                      : (k -= n_m) < n_f ? ty_mangle(VEC_PTR(&g_mono_fntypes, k, Type))
                                         : ty_mangle(VEC_PTR(&g_mono_tasks, k - n_f, Type));
        fail(0, "type '%s' is part of a by-value cycle — put one step of it behind an "
                "array, which is a reference", m);
    }
    free(done);

    /* Array layouts need only names, so they can all go out together. */
    for (int i = 0; i < n_a; i++) emit_array_def(VEC_PTR(&g_mono_arrays, i, Type), sb);
    sb_append(sb, "\n");

    /* ── 3. functions ── */
    for (int i = 0; i < n_a; i++) emit_array(VEC_PTR(&g_mono_arrays, i, Type), sb);
    for (int i = 0; i < n_m; i++) emit_map(VEC_PTR(&g_mono_maps, i, Type), sb);
}

/* ── lambda lifting ───────────────────────────────────────────────────────
 * Every closure literal becomes a top-level C function plus a GC-allocated
 * environment struct holding its captures. Collecting them up front means the
 * environments and forward declarations can be emitted before any body needs them.
 */
static Vec g_lifted;   /* Vec<Expr*> — every EX_LAMBDA, in emission order */

static void lift_stmts(Vec *body);

static void lift_expr(Expr *e) {
    if (!e) return;
    lift_expr(e->lhs);
    lift_expr(e->rhs);
    for (int i = 0; i < e->args.count; i++) lift_expr(VEC_PTR(&e->args, i, Expr));
    for (int i = 0; i < e->fields.count; i++)
        lift_expr(((FieldInit *)vec_get(&e->fields, i))->value);
    for (int i = 0; i < e->arms.count; i++) {
        MatchArm *arm = vec_get(&e->arms, i);
        lift_expr(arm->value);
        lift_stmts(&arm->body);
    }
    if (e->kind == EX_LAMBDA) {
        lift_stmts(&e->body);
        e->lam_id = g_lifted.count;
        VEC_PUSH_PTR(&g_lifted, e);
    }
}

static void lift_stmts(Vec *body) {
    for (int i = 0; i < body->count; i++) {
        Stmt *s = VEC_PTR(body, i, Stmt);
        lift_expr(s->expr);
        lift_expr(s->expr2);
        lift_expr(s->target);
        for (int j = 0; j < s->cond_blocks.count; j++) {
            CondBlock *cb = vec_get(&s->cond_blocks, j);
            lift_expr(cb->cond);
            lift_stmts(&cb->body);
        }
        lift_stmts(&s->body);
    }
}

static void lambda_signature(Expr *lam, SB *sb) {
    Type *ft = lam->type;
    sb_appendf(sb, "%s _klam%d(void *_kenvp", c_type(ty_ret(ft)), lam->lam_id);
    for (int i = 0; i < lam->params.count; i++) {
        Field *pm = vec_get(&lam->params, i);
        sb_appendf(sb, ", %s %s", c_type(pm->type), pm->name);
    }
    sb_append(sb, ")");
}

static void emit_lambda_env(Expr *lam, SB *sb) {
    sb_appendf(sb, "typedef struct _klam%d_env {\n", lam->lam_id);
    for (int i = 0; i < lam->captures.count; i++) {
        Field *c = vec_get(&lam->captures, i);
        sb_appendf(sb, "    %s %s;\n", c_type(c->type), c->name);
    }
    if (lam->captures.count == 0) sb_append(sb, "    char _empty;\n");
    sb_appendf(sb, "} _klam%d_env;\n", lam->lam_id);
}

/* How a Klang type looks on the JavaScript side of EM_JS. Everything numeric
   goes as a double, because that is the only number JavaScript has. */
static const char *js_c_type(const Type *t) {
    switch (t->kind) {
        case TY_VOID:   return "void";
        case TY_STRING: return "char*";
        case TY_BOOL:   return "int";
        default:        return "double";   /* int and float */
    }
}

/* The JavaScript half of every `js fn`, written to a companion `--js-library`
 * file rather than into the C.
 *
 * EM_JS would keep everything in one file, but it stringifies the body through
 * the C preprocessor, which collapses it onto a single line — so a body written
 * in ordinary semicolon-free style silently becomes a JavaScript syntax error in
 * generated code, which is a terrible thing to hand back to someone. A js-library
 * file keeps the author's text exactly as written.
 */
static SB g_js_lib;
static bool g_needs_js = false;   /* the program has a `js fn` or an `export fn` */
static int g_js_count = 0;

/* A `js fn` becomes a JavaScript library entry plus a Klang-facing wrapper that
   does the marshalling, so no Klang source ever mentions UTF8ToString or a heap
   pointer. */
static void emit_js_fn(FnDecl *fd, SB *sb) {
    const char *ret = c_type(fd->ret_type);
    bool has_ret = fd->ret_type->kind != TY_VOID;
    bool takes_str = false;
    for (int i = 0; i < fd->params.count; i++)
        if (((Field *)vec_get(&fd->params, i))->type->kind == TY_STRING) takes_str = true;
    bool gives_str = fd->ret_type->kind == TY_STRING;

    SB *js = &g_js_lib;
    if (takes_str || gives_str) {
        sb_appendf(js, "  _kjs_%s__deps: [", fd->mangled);
        if (takes_str) sb_append(js, "'$UTF8ToString'");
        if (takes_str && gives_str) sb_append(js, ", ");
        if (gives_str) sb_append(js, "'$stringToNewUTF8'");
        sb_append(js, "],\n");
    }
    sb_appendf(js, "  // %s.%s, from %s\n", fd->module ? fd->module : "", fd->name,
               fd->file ? fd->file : "?");
    sb_appendf(js, "  _kjs_%s: function(", fd->mangled);
    for (int i = 0; i < fd->params.count; i++)
        sb_appendf(js, "%s%s", i ? ", " : "", ((Field *)vec_get(&fd->params, i))->name);
    sb_append(js, ") {\n");
    /* Pointers become JavaScript strings, and 0/1 becomes a boolean, before the
       author's code runs — so the body reads like ordinary JavaScript. */
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        if (pm->type->kind == TY_STRING)
            sb_appendf(js, "    %s = UTF8ToString(%s);\n", pm->name, pm->name);
        else if (pm->type->kind == TY_BOOL)
            sb_appendf(js, "    %s = !!%s;\n", pm->name, pm->name);
    }
    /* The body is wrapped so its `return` produces a value this entry can convert
       on the way out, rather than returning straight to C unmarshalled. */
    if (has_ret) sb_append(js, "    var _kv = (function() {");
    sb_append(js, fd->js_body);
    if (has_ret) {
        sb_append(js, "})();\n");
        if (gives_str)
            sb_append(js, "    return (_kv === undefined || _kv === null) ? 0 : stringToNewUTF8(String(_kv));\n");
        else if (fd->ret_type->kind == TY_BOOL)
            sb_append(js, "    return _kv ? 1 : 0;\n");
        else
            sb_append(js, "    return +_kv;\n");
    }
    sb_append(js, "  },\n");
    g_js_count++;

    sb_appendf(sb, "%s _kjs_%s(", js_c_type(fd->ret_type), fd->mangled);
    if (fd->params.count == 0) sb_append(sb, "void");
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        sb_appendf(sb, "%s%s %s", i ? ", " : "", js_c_type(pm->type), pm->name);
    }
    sb_append(sb, ");\n");

    sb_appendf(sb, "%s %s(", ret, fd->mangled);
    if (fd->params.count == 0) sb_append(sb, "void");
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        sb_appendf(sb, "%s%s %s", i ? ", " : "", c_type(pm->type), pm->name);
    }
    sb_append(sb, ") {\n");
    sb_append(sb, "#ifdef __EMSCRIPTEN__\n    ");
    if (has_ret) sb_appendf(sb, "%s _kv = ", js_c_type(fd->ret_type));
    sb_appendf(sb, "_kjs_%s(", fd->mangled);
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        if (i) sb_append(sb, ", ");
        if (pm->type->kind == TY_INT)
            sb_appendf(sb, "klang_js_num(%s, \"%s\")", pm->name, fd->name);
        else sb_append(sb, pm->name);
    }
    sb_append(sb, ");\n");
    if (has_ret) {
        sb_append(sb, "    return ");
        switch (fd->ret_type->kind) {
            case TY_STRING: sb_append(sb, "klang_js_str(_kv);\n"); break;
            case TY_INT:    sb_appendf(sb, "klang_js_int(_kv, \"%s\");\n", fd->name); break;
            case TY_BOOL:   sb_append(sb, "_kv != 0;\n"); break;
            default:        sb_append(sb, "_kv;\n"); break;
        }
    }
    sb_append(sb, "#else\n");
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        sb_appendf(sb, "    (void)%s;\n", pm->name);
    }
    sb_appendf(sb, "    klang_no_js(\"%s\");\n", fd->name);
    if (has_ret) sb_appendf(sb, "    return (%s){0};\n", ret);
    sb_append(sb, "#endif\n}\n\n");
}

/* `export fn` gets a second symbol under its plain Klang name, kept alive so a
   JavaScript caller can reach it as Module._name.
 *
 * Numbers cross as doubles in both directions, checked for exactness the same way
 * `js fn` checks them — otherwise a Klang `int` would surface in JavaScript as a
 * BigInt, which is technically right and practically a trap. Strings cross as
 * pointers, which means a JavaScript caller reaches them through Module.ccall. */
static void emit_js_export(FnDecl *fd, SB *sb) {
    bool ret_int = fd->ret_type->kind == TY_INT;
    sb_appendf(sb, "EMSCRIPTEN_KEEPALIVE %s %s(",
               ret_int ? "double" : c_type(fd->ret_type), fd->name);
    if (fd->params.count == 0) sb_append(sb, "void");
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        sb_appendf(sb, "%s%s %s", i ? ", " : "",
                   pm->type->kind == TY_INT ? "double" : c_type(pm->type), pm->name);
    }
    sb_append(sb, ") {\n    ");
    if (fd->ret_type->kind != TY_VOID)
        sb_appendf(sb, "return %s", ret_int ? "klang_js_num(" : "");
    sb_appendf(sb, "%s(", fd->mangled);
    for (int i = 0; i < fd->params.count; i++) {
        Field *pm = vec_get(&fd->params, i);
        if (i) sb_append(sb, ", ");
        if (pm->type->kind == TY_INT)
            sb_appendf(sb, "klang_js_int(%s, \"%s\")", pm->name, fd->name);
        else if (pm->type->kind == TY_STRING)
            sb_appendf(sb, "klang_js_own(%s)", pm->name);
        else sb_append(sb, pm->name);
    }
    sb_append(sb, ")");
    if (ret_int) sb_appendf(sb, ", \"%s\")", fd->name);
    sb_append(sb, ";\n}\n\n");
}

/* The frame has to say how many slots it holds, and that is only known once the
   body has been emitted — so the body is built first and the prologue prepended. */
static void emit_frame_prologue(SB *sb, int slots) {
    sb_appendf(sb, "    KRootSlot _kr[%d];\n", slots > 0 ? slots : 1);
    sb_append(sb, "    KFrame _kf;\n");
    sb_append(sb, "    memset(_kr, 0, sizeof _kr);\n");
    sb_appendf(sb, "    _kf.prev = k_frames; _kf.n = %d; _kf.slots = _kr;\n", slots);
    sb_append(sb, "    k_frames = &_kf;\n");
}

static void emit_lambda_body(Expr *lam, SB *sb) {
    Type *saved = g_cg_ret;
    int saved_slots = g_slot_count;
    g_cg_ret = ty_ret(lam->type);
    g_slot_count = 0;

    SB body; sb_init(&body);
    if (lam->captures.count > 0) {
        sb_appendf(&body, "    _klam%d_env *_kenv = _kenvp;\n", lam->lam_id);
        /* Captures are copies, exactly as if they had been passed as arguments. */
        for (int i = 0; i < lam->captures.count; i++) {
            Field *c = vec_get(&lam->captures, i);
            sb_appendf(&body, "    %s %s = _kenv->%s; (void)%s;\n",
                       c_type(c->type), c->name, c->name, c->name);
            root_local(&body, 1, c->type, c->name);
        }
    } else {
        sb_append(&body, "    (void)_kenvp;\n");
    }
    for (int i = 0; i < lam->params.count; i++) {
        Field *pm = vec_get(&lam->params, i);
        root_local(&body, 1, pm->type, pm->name);
    }
    if (lam->is_block) {
        cg_stmts(&lam->body, &body, 1);
        sb_append(&body, "    k_frames = _kf.prev;\n");
    } else {
        SB pre; sb_init(&pre);
        SB val; sb_init(&val);
        cg_expr(lam->lhs, &val, &pre, 1);
        sb_append(&body, pre.data);
        if (ty_ret(lam->type)->kind == TY_VOID) {
            sb_appendf(&body, "    (void)(%s);\n", val.data);
            sb_append(&body, "    k_frames = _kf.prev;\n");
        } else {
            sb_appendf(&body, "    %s _kret = %s;\n", c_type(ty_ret(lam->type)), val.data);
            sb_append(&body, "    k_frames = _kf.prev;\n");
            sb_append(&body, "    return _kret;\n");
        }
    }

    lambda_signature(lam, sb);
    sb_append(sb, " {\n");
    emit_frame_prologue(sb, g_slot_count);
    sb_append(sb, body.data);
    sb_append(sb, "}\n\n");
    g_cg_ret = saved;
    g_slot_count = saved_slots;
}

static void codegen(SB *sb) {
    sb_init(&g_js_lib);
    sb_append(&g_js_lib, "// Generated by klangc 0.22 — the JavaScript half of this\n"
                         "// program's 'js fn' declarations. Pass it to emcc with\n"
                         "// --js-library; do not edit by hand.\n"
                         "mergeInto(LibraryManager.library, {\n");
    sb_append(sb, "/* Generated by klangc 0.22 — do not edit by hand */\n");
    sb_append(sb, "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n");
    sb_append(sb, "#include <stdbool.h>\n#include <stdint.h>\n#include <inttypes.h>\n");
    sb_append(sb, "#include <stddef.h>\n#include <setjmp.h>\n");
    /* Whatever the program's `extern header` declarations asked for. Klang emits no
       prototypes of its own for C functions — the header is the single source of
       truth for their signatures, so the two can never disagree. */
    for (int i = 0; i < g_c_headers.count; i++) {
        const char *h = VEC_PTR(&g_c_headers, i, char);
        if (h[0] == '<' || h[0] == '"') sb_appendf(sb, "#include %s\n", h);
        else sb_appendf(sb, "#include \"%s\"\n", h);
    }
    sb_append(sb, "\n");
    sb_appendf(sb, "#define KLANG_THREADS %d\n\n", g_uses_threads ? 1 : 0);
    if (g_uses_threads) sb_append(sb, THREAD_RUNTIME);
    sb_append(sb, ROOT_RUNTIME);
    sb_append(sb, GC_RUNTIME);
    if (g_uses_threads) sb_append(sb, GC_THREADS);
    sb_append(sb, GC_COLLECT);
    sb_append(sb, RUNTIME);
    /* Only a program that actually reaches into JavaScript carries the code for
       crossing that boundary. A native build should have no trace of it — not dead
       code the linker strips, but nothing emitted at all. */
    for (int i = 0; i < g_mono_fns.count; i++) {
        FnDecl *fd = VEC_PTR(&g_mono_fns, i, FnDecl);
        if (fd->js_body || fd->is_export) { g_needs_js = true; break; }
    }
    if (g_needs_js) sb_append(sb, JS_RUNTIME);
    sb_append(sb, "\n");

    emit_types(sb);

    /* Copy functions and task types, once every type they mention exists. */
    if (g_uses_threads) {
        for (int i = 0; i < g_mono_arrays.count; i++)
            sb_appendf(sb, "%s %s_copy(%s a);\n", ty_mangle(VEC_PTR(&g_mono_arrays, i, Type)),
                       ty_mangle(VEC_PTR(&g_mono_arrays, i, Type)),
                       ty_mangle(VEC_PTR(&g_mono_arrays, i, Type)));
        for (int i = 0; i < g_mono_maps.count; i++)
            sb_appendf(sb, "%s %s_copy(%s d);\n", ty_mangle(VEC_PTR(&g_mono_maps, i, Type)),
                       ty_mangle(VEC_PTR(&g_mono_maps, i, Type)),
                       ty_mangle(VEC_PTR(&g_mono_maps, i, Type)));
        for (int i = 0; i < g_mono_structs.count; i++) {
            StructDecl *sd = VEC_PTR(&g_mono_structs, i, StructDecl);
            if (!sd->is_opaque) sb_appendf(sb, "%s %s_copy(%s v);\n", sd->mangled, sd->mangled, sd->mangled);
        }
        for (int i = 0; i < g_mono_enums.count; i++) {
            EnumDecl *ed = VEC_PTR(&g_mono_enums, i, EnumDecl);
            sb_appendf(sb, "%s %s_copy(%s v);\n", ed->mangled, ed->mangled, ed->mangled);
        }
        sb_append(sb, "\n");
        for (int i = 0; i < g_mono_arrays.count; i++)
            emit_array_copy(VEC_PTR(&g_mono_arrays, i, Type), sb);
        for (int i = 0; i < g_mono_maps.count; i++)
            emit_map_copy(VEC_PTR(&g_mono_maps, i, Type), sb);
        for (int i = 0; i < g_mono_structs.count; i++) {
            StructDecl *sd = VEC_PTR(&g_mono_structs, i, StructDecl);
            if (!sd->is_opaque) emit_struct_copy(sd, sb);
        }
        for (int i = 0; i < g_mono_enums.count; i++)
            emit_enum_copy(VEC_PTR(&g_mono_enums, i, EnumDecl), sb);
        sb_append(sb, "\n");
    }

    /* Constants are globals, declared here so that anything emitted later — a
       closure body included — can refer to one. They are filled in at startup. */
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind != DECL_CONST) continue;
        sb_appendf(sb, "%s %s;\n", c_type(d->c->type), d->c->mangled);
    }
    sb_append(sb, "\n");

    /* Closures are lifted before anything is written, so their environments and
       declarations precede every body that builds or calls one. */
    vec_init(&g_lifted, sizeof(Expr *));
    for (int i = 0; i < g_mono_fns.count; i++)
        lift_stmts(&VEC_PTR(&g_mono_fns, i, FnDecl)->body);
    for (int i = 0; i < g_decls.count; i++) {
        Decl *d = vec_get(&g_decls, i);
        if (d->kind == DECL_CONST) lift_expr(d->c->value);
    }
    for (int i = 0; i < g_lifted.count; i++)
        emit_lambda_env(VEC_PTR(&g_lifted, i, Expr), sb);
    if (g_lifted.count) sb_append(sb, "\n");

    for (int i = 0; i < g_lifted.count; i++) {
        lambda_signature(VEC_PTR(&g_lifted, i, Expr), sb);
        sb_append(sb, ";\n");
    }
    if (g_lifted.count) sb_append(sb, "\n");

    for (int i = 0; i < g_mono_fns.count; i++) {
        FnDecl *fd = VEC_PTR(&g_mono_fns, i, FnDecl);
        if (fd->is_extern) continue;   /* its header declares it */
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

    /* A plain function used as a value needs a wrapper taking the closure calling
       convention, since it has no environment of its own. */
    Vec refs; vec_init(&refs, sizeof(char *));
    for (int i = 0; i < g_fnrefs.count; i++) {
        Expr *r = VEC_PTR(&g_fnrefs, i, Expr);
        bool seen = false;
        for (int j = 0; j < refs.count; j++)
            if (strcmp(VEC_PTR(&refs, j, char), r->resolved) == 0) { seen = true; break; }
        if (seen) continue;
        VEC_PUSH_PTR(&refs, r->resolved);
        Type *ft = r->type;
        sb_appendf(sb, "%s _kref_%s(void *_kenvp", c_type(ty_ret(ft)), r->resolved);
        for (int j = 0; j < ty_nparams(ft); j++)
            sb_appendf(sb, ", %s _a%d", c_type(ty_param(ft, j)), j);
        sb_append(sb, ") {\n    (void)_kenvp;\n    ");
        if (ty_ret(ft)->kind != TY_VOID) sb_append(sb, "return ");
        sb_appendf(sb, "%s(", r->resolved);
        for (int j = 0; j < ty_nparams(ft); j++) sb_appendf(sb, "%s_a%d", j ? ", " : "", j);
        sb_append(sb, ");\n}\n");
    }
    if (refs.count) sb_append(sb, "\n");

    for (int i = 0; i < g_lifted.count; i++)
        emit_lambda_body(VEC_PTR(&g_lifted, i, Expr), sb);

    /* Constants are globals filled in once at startup, so an initializer is an
       ordinary expression and may allocate. */
    int nconsts = 0;
    for (int i = 0; i < g_decls.count; i++)
        if (((Decl *)vec_get(&g_decls, i))->kind == DECL_CONST) nconsts++;
    if (nconsts) {
        /* Two frames here. The temporaries an initializer needs go in an ordinary
           frame that is popped on the way out; the constants themselves go in a
           static frame that is never popped, because they stay reachable forever. */
        SB body; sb_init(&body);
        SB roots; sb_init(&roots);
        int nglobal = 0;
        g_slot_count = 0;
        for (int i = 0; i < g_decls.count; i++) {
            Decl *d = vec_get(&g_decls, i);
            if (d->kind != DECL_CONST) continue;
            SB pre; sb_init(&pre);
            SB val; sb_init(&val);
            g_cg_ret = ty_void();
            cg_expr(d->c->value, &val, &pre, 1);
            sb_append(&body, pre.data);
            sb_appendf(&body, "    %s = %s;\n", d->c->mangled, val.data);
            if (type_has_gc(d->c->type)) {
                sb_appendf(&roots, "    _kg[%d].addr = &%s; _kg[%d].size = sizeof %s;\n",
                           nglobal, d->c->mangled, nglobal, d->c->mangled);
                nglobal++;
            }
        }
        sb_append(sb, "void _kinit_consts(void) {\n");
        sb_appendf(sb, "    static KRootSlot _kg[%d];\n", nglobal > 0 ? nglobal : 1);
        sb_append(sb, "    static KFrame _kgf;\n");
        emit_frame_prologue(sb, g_slot_count);
        /* Registered before any initializer runs: the globals start out zeroed, so
           scanning one that has not been assigned yet finds nothing and is safe,
           whereas registering afterwards would leave earlier constants unrooted
           while a later initializer allocates. */
        sb_append(sb, roots.data);
        sb_appendf(sb, "    _kgf.prev = k_global_frames; _kgf.n = %d; _kgf.slots = _kg;\n", nglobal);
        sb_append(sb, "    k_global_frames = &_kgf;\n");
        sb_append(sb, body.data);
        sb_append(sb, "    k_frames = _kf.prev;\n");
        sb_append(sb, "}\n\n");
        g_slot_count = 0;
    }

    for (int i = 0; i < g_mono_fns.count; i++) {
        FnDecl *fd = VEC_PTR(&g_mono_fns, i, FnDecl);
        if (fd->is_extern) continue;
        if (fd->js_body) { emit_js_fn(fd, sb); continue; }
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

        SB body; sb_init(&body);
        g_slot_count = 0;
        for (int j = 0; j < fd->params.count; j++) {
            Field *pm = vec_get(&fd->params, j);
            root_local(&body, 1, pm->type, pm->name);
        }
        cg_stmts(&fd->body, &body, 1);
        sb_append(&body, "    k_frames = _kf.prev;\n");
        if (is_main) sb_append(&body, "    return 0;\n");

        emit_frame_prologue(sb, g_slot_count);
        sb_append(sb, body.data);
        sb_append(sb, "}\n\n");
        g_slot_count = 0;
    }

    for (int i = 0; i < g_mono_fns.count; i++) {
        FnDecl *fd = VEC_PTR(&g_mono_fns, i, FnDecl);
        if (fd->is_export) emit_js_export(fd, sb);
    }

    /* The collector scans from its own frame up to this anchor. Taking the anchor
       here — in a frame that strictly encloses klang_main — guarantees every Klang
       local lies inside that range, whatever order the C compiler lays frames out. */
    sb_append(sb, "int main(void) {\n");
    sb_append(sb, "    int _kanchor = 0;\n");
    sb_append(sb, "    klang_gc_init(&_kanchor);\n");
    if (nconsts) sb_append(sb, "    _kinit_consts();\n");
    sb_append(sb, "    return klang_main();\n");
    sb_append(sb, "}\n");
}

/* ───────────────────────── driver ───────────────────────── */

static const char *PRELUDE =
    "enum Option<T> { None, Some(T) }\n"
    "enum Result<T, E> { Ok(T), Err(E) }\n";

static char *read_file_opt(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}
static char *read_file(const char *path) {
    char *s = read_file_opt(path);
    if (!s) { fprintf(stderr, "klangc: cannot open '%s'\n", path); exit(1); }
    return s;
}

/* ── module loading ───────────────────────────────────────────────────────
 * `import "std/math"` is resolved against, in order:
 *   1. the project root — the directory of the file named on the command line
 *   2. each directory in the KLANG_PATH environment variable
 *   3. alongside the compiler: <exe>, <exe>/.., and <exe>/../lib/klang
 * so the standard library is found without anyone configuring anything.
 *
 * Resolution deliberately does NOT depend on which file is doing the importing.
 * A path therefore always names the same module, so a module reached from two
 * different places is still one module with one set of declarations.
 *
 * A module is loaded once no matter how many times it is imported, and an
 * import cycle is reported rather than followed.
 */
static const char *g_exe_dir = ".";
static const char *g_root_dir = ".";
static Vec g_loaded;    /* Vec<char*> — module paths already parsed */
static Vec g_loading;   /* Vec<char*> — the current import chain, for cycle reports */

static bool vec_has_str(Vec *v, const char *s) {
    for (int i = 0; i < v->count; i++)
        if (strcmp(VEC_PTR(v, i, char), s) == 0) return true;
    return false;
}
/* "dir/of/importer" for a file path, or "." when it has no directory part. */
static char *dir_of(const char *path) {
    const char *slash = NULL;
    for (const char *c = path; *c; c++) if (*c == '/' || *c == '\\') slash = c;
    if (!slash) return strdup(".");
    return dupn(path, (int)(slash - path));
}
static char *join_path(const char *dir, const char *rel) {
    size_t n = strlen(dir) + strlen(rel) + 8;
    char *r = malloc(n);
    snprintf(r, n, "%s/%s.kkg", dir, rel);
    return r;
}

static void load_module(const char *path, const char *importer, int line);

/* Parse one file as `module`, then load whatever it imports. */
static void load_source(char *src, const char *file, const char *module) {
    Parser p;
    g_filename = file;
    remember_source(file, src);   /* so a later error can quote the line */
    parser_init(&p, src, module, file);
    parse_program(&p);
    for (int i = 0; i < p.imports.count; i++)
        load_module(VEC_PTR(&p.imports, i, char), file, 0);
}

static void load_module(const char *path, const char *importer, int line) {
    if (vec_has_str(&g_loaded, path)) return;
    if (vec_has_str(&g_loading, path)) {
        g_filename = importer;
        fail(line, "import cycle: module '%s' ends up importing itself", path);
    }

    char *file = join_path(g_root_dir, path);
    char *src = read_file_opt(file);
    if (!src) {
        const char *env = getenv("KLANG_PATH");
        if (env) {
            char *copy = strdup(env);
            for (char *tok = copy; tok && *tok;) {
                char *sep = strpbrk(tok, ";:");
                /* A drive letter like C:\ is not a separator. */
                while (sep && *sep == ':' && sep == tok + 1 && isalpha((unsigned char)tok[0]))
                    sep = strpbrk(sep + 1, ";:");
                if (sep) *sep = 0;
                free(file);
                file = join_path(tok, path);
                src = read_file_opt(file);
                if (src) break;
                tok = sep ? sep + 1 : NULL;
            }
            free(copy);
        }
    }
    if (!src) {
        const char *roots[3];
        char buf1[1024], buf2[1024];
        snprintf(buf1, sizeof buf1, "%s/..", g_exe_dir);
        snprintf(buf2, sizeof buf2, "%s/../lib/klang", g_exe_dir);
        roots[0] = g_exe_dir; roots[1] = buf1; roots[2] = buf2;
        for (int i = 0; i < 3 && !src; i++) {
            free(file);
            file = join_path(roots[i], path);
            src = read_file_opt(file);
        }
    }
    if (!src) {
        g_filename = importer;
        fail(line, "cannot find module '%s' — looked for '%s/%s.kkg', next to the "
                   "compiler%s", path, g_root_dir, path,
             getenv("KLANG_PATH") ? ", and in KLANG_PATH" : "");
    }

    VEC_PUSH_PTR(&g_loading, (char *)path);
    load_source(src, file, path);
    g_loading.count--;
    VEC_PUSH_PTR(&g_loaded, (char *)path);
}

/* Compiles `in_path` to `out_path`, plus the `.lib.js` half if the program uses
   `js fn`. `js_out`, if given, receives that path (or NULL). Chatty by default,
   since it is also the compiler's own command-line output; `quiet` is for when a
   larger command is doing the talking. */
static int compile_file(const char *in_path, const char *out_path, char **js_out, bool quiet) {
    if (js_out) *js_out = NULL;

    vec_init(&g_decls, sizeof(Decl));
    vec_init(&g_mono_structs, sizeof(StructDecl *));
    vec_init(&g_mono_enums, sizeof(EnumDecl *));
    vec_init(&g_mono_fns, sizeof(FnDecl *));
    vec_init(&g_mono_arrays, sizeof(Type *));
    vec_init(&g_mono_maps, sizeof(Type *));
    vec_init(&g_mono_fntypes, sizeof(Type *));
    vec_init(&g_mono_tasks, sizeof(Type *));
    vec_init(&g_lams, sizeof(LamFrame));
    vec_init(&g_fnrefs, sizeof(Expr *));
    vec_init(&g_fn_queue, sizeof(FnDecl *));
    vec_init(&g_xrefs, sizeof(XRef));
    vec_init(&g_modules, sizeof(ModuleInfo));
    vec_init(&g_c_headers, sizeof(char *));
    vec_init(&g_c_links, sizeof(char *));
    vec_init(&g_loaded, sizeof(char *));
    vec_init(&g_loading, sizeof(char *));
    g_root_dir = dir_of(in_path);

    load_source(strdup(PRELUDE), "<prelude>", MOD_PRELUDE);
    load_source(read_file(in_path), in_path, MOD_ROOT);

    monomorphize_and_check();

    SB out; sb_init(&out);
    codegen(&out);

    FILE *f = fopen(out_path, "wb");
    if (!f) { fprintf(stderr, "klangc: cannot write '%s'\n", out_path); return 1; }
    fwrite(out.data, 1, (size_t)out.len, f);
    fclose(f);

    if (!quiet) printf("klangc: compiled '%s' -> '%s'\n", in_path, out_path);

    /* A program with `js fn` declarations needs its JavaScript half alongside the
       C, and needs emcc to be told about it — so say the whole command rather
       than leaving it to be discovered. */
    if (g_js_count > 0) {
        sb_append(&g_js_lib, "});\n");
        size_t n = strlen(out_path);
        const char *dot = strrchr(out_path, '.');
        size_t base = dot ? (size_t)(dot - out_path) : n;
        char *js_path = malloc(base + 8);
        memcpy(js_path, out_path, base);
        strcpy(js_path + base, ".lib.js");
        FILE *jf = fopen(js_path, "wb");
        if (!jf) { fprintf(stderr, "klangc: cannot write '%s'\n", js_path); return 1; }
        fwrite(g_js_lib.data, 1, (size_t)g_js_lib.len, jf);
        fclose(jf);
        if (js_out) *js_out = js_path;
        if (!quiet) {
            printf("klangc: %d 'js fn' declaration(s) -> '%s'\n", g_js_count, js_path);
            printf("klangc: this is a web program — try  klangc web run %s\n", in_path);
        }
    }

    if (!quiet && (g_c_links.count || g_uses_threads)) {
        printf("klangc: link with");
        for (int i = 0; i < g_c_links.count; i++)
            printf(" -l%s", VEC_PTR(&g_c_links, i, char));
        if (g_uses_threads) printf(" -lpthread");   /* this program spawns */
        printf("\n");
    }
    return 0;
}

/* ───────────────────────── the web command ─────────────────────────
 *
 * `klangc web run` is the whole loop: find the program, compile it, build it
 * with emcc, and serve it. Everything it needs is generated, so there is no
 * scaffolding to check in and nothing to install from a registry.
 *
 * The dev server runs under Bun or Node — whichever is there, Bun first, since
 * it starts faster and is what a lot of people now have. One script covers both;
 * it is short enough that carrying two would be the sillier choice.
 */

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #define k_mkdir(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #define k_mkdir(p) mkdir(p, 0755)
#endif

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* fopen on a directory succeeds on some platforms and fails on others, so ask
   about a name that can only exist inside one. */
static bool dir_exists(const char *path) {
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/.", path);
    FILE *f = fopen(probe, "rb");
    if (f) { fclose(f); return true; }
#ifdef _WIN32
    /* MSVC's fopen refuses "dir/." — fall back to asking the OS. */
    return _access(path, 0) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static void ensure_dir(const char *path) {
    /* Make each component in turn, so a nested path works from nothing. */
    char *buf = strdup(path);
    for (char *c = buf + 1; *c; c++) {
        if (*c != '/' && *c != '\\') continue;
        char save = *c; *c = '\0';
        k_mkdir(buf);
        *c = save;
    }
    k_mkdir(buf);
    free(buf);
}

static bool has_tool(const char *name) {
    char cmd[256];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "where %s >NUL 2>NUL", name);
#else
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
#endif
    return system(cmd) == 0;
}

/* A static file server small enough to read, in the one dialect both runtimes
   understand. Bun gets Bun.serve; Node gets node:http. */
static const char *SERVE_JS =
    "// Generated by klangc - the dev server for `klangc web run`.\n"
    "const PORT = Number(process.argv[2] || 8080);\n"
    "const ROOT = process.argv[3] || '.';\n"
    "const TYPES = {\n"
    "  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',\n"
    "  '.mjs': 'text/javascript; charset=utf-8', '.wasm': 'application/wasm',\n"
    "  '.css': 'text/css; charset=utf-8', '.json': 'application/json',\n"
    "  '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg',\n"
    "  '.gif': 'image/gif', '.ico': 'image/x-icon', '.woff2': 'font/woff2',\n"
    "  '.map': 'application/json', '.txt': 'text/plain; charset=utf-8',\n"
    "};\n"
    "const nodePath = require('node:path');\n"
    "const root = nodePath.resolve(ROOT);\n"
    "\n"
    "// A dev server still has to refuse '../..' - a bug here reads the whole disk.\n"
    "function resolve(urlPath) {\n"
    "  let p = decodeURIComponent(urlPath.split('?')[0]);\n"
    "  if (p.endsWith('/')) p += 'index.html';\n"
    "  const full = nodePath.resolve(root, '.' + p);\n"
    "  if (full !== root && !full.startsWith(root + nodePath.sep)) return null;\n"
    "  return full;\n"
    "}\n"
    "function typeOf(p) {\n"
    "  return TYPES[nodePath.extname(p).toLowerCase()] || 'application/octet-stream';\n"
    "}\n"
    "// No caching: the point of this server is that a rebuild shows up on reload.\n"
    "const NOCACHE = { 'Cache-Control': 'no-store' };\n"
    "\n"
    "if (typeof Bun !== 'undefined') {\n"
    "  Bun.serve({\n"
    "    port: PORT,\n"
    "    async fetch(req) {\n"
    "      const full = resolve(new URL(req.url).pathname);\n"
    "      if (!full) return new Response('forbidden', { status: 403 });\n"
    "      const file = Bun.file(full);\n"
    "      if (!(await file.exists())) return new Response('not found', { status: 404 });\n"
    "      return new Response(file, { headers: { 'Content-Type': typeOf(full), ...NOCACHE } });\n"
    "    },\n"
    "  });\n"
    "  console.log('klang: serving ' + root + ' with bun on http://localhost:' + PORT);\n"
    "} else {\n"
    "  const http = require('node:http');\n"
    "  const fs = require('node:fs');\n"
    "  http.createServer((req, res) => {\n"
    "    const full = resolve(req.url);\n"
    "    if (!full) { res.writeHead(403); res.end('forbidden'); return; }\n"
    "    fs.readFile(full, (err, data) => {\n"
    "      if (err) { res.writeHead(404); res.end('not found'); return; }\n"
    "      res.writeHead(200, { 'Content-Type': typeOf(full), ...NOCACHE });\n"
    "      res.end(data);\n"
    "    });\n"
    "  }).listen(PORT, () => {\n"
    "    console.log('klang: serving ' + root + ' with node on http://localhost:' + PORT);\n"
    "  });\n"
    "}\n";

/* The page a project gets when it has not written one. It is deliberately plain:
   it exists so that `web run` works on the first try, not to be a template. */
static void write_default_index(const char *path, const char *title, const char *root) {
    char probe[1100];
    snprintf(probe, sizeof probe, "%s/style.css", root);
    bool has_css = file_exists(probe);

    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f,
        "<!doctype html>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>%s</title>\n", title);
    if (has_css) fprintf(f, "<link rel=\"stylesheet\" href=\"style.css\">\n");
    fprintf(f,
        "\n"
        "<!-- Generated by klangc, and regenerated whenever it is missing.\n"
        "\n"
        "     Nothing needs to go in here. The markup comes from std/html and the\n"
        "     styling from std/css, both mounted by the program:\n"
        "\n"
        "         dom.useStyle(css.sheet(styles()))\n"
        "         dom.mount(\"#app\", view())\n"
        "\n"
        "     Write your own index.html beside the program if you want control of\n"
        "     the <head> - a font link, an extra <meta> - and it will be used\n"
        "     instead of this one. -->\n"
        "<div id=\"app\"></div>\n"
        "\n"
        "<script src=\"app.js\"></script>\n");
    fclose(f);
}

/* The program to run: what was asked for, else the conventional names. */
static const char *find_entry(const char *given) {
    if (given) {
        if (file_exists(given)) return given;
        fprintf(stderr, "klangc: cannot find '%s'\n", given);
        exit(1);
    }
    static const char *tries[] = { "main.kkg", "src/main.kkg", "app.kkg", "web.kkg" };
    for (size_t i = 0; i < sizeof tries / sizeof *tries; i++)
        if (file_exists(tries[i])) return tries[i];
    fprintf(stderr,
        "klangc: no program to run here\n"
        "       | help: `klangc web run` looks for main.kkg, src/main.kkg, app.kkg\n"
        "       |       or web.kkg - or name the file: `klangc web run page.kkg`\n");
    exit(1);
}

/* "examples/web.kkg" -> "web" */
static char *stem_of(const char *path) {
    const char *slash = NULL;
    for (const char *c = path; *c; c++) if (*c == '/' || *c == '\\') slash = c;
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    return dupn(base, dot ? (int)(dot - base) : (int)strlen(base));
}

/* Stylesheets.
 *
 * A plain .css file needs nothing from klangc: it is a static file, and the dev
 * server already sends it with the right type. Tailwind is different, because it
 * has to be compiled — and compiling it means knowing where the class names are,
 * which for Klang is inside string literals in .kkg files. That is not somewhere
 * Tailwind would look on its own, so the scaffold points it there with @source.
 *
 * The rule here is narrow on purpose: an input.css that mentions Tailwind gets the
 * Tailwind CLI run over it. Anything else is left alone, because guessing at
 * someone's build tool is worse than not having one. */
static int build_css(const char *root, bool minify) {
    char in[1100];
    snprintf(in, sizeof in, "%s/input.css", root);
    char *src = read_file_opt(in);
    if (!src) return 0;
    if (!strstr(src, "tailwind")) return 0;   /* someone else's pipeline; not ours */

    bool bun = has_tool("bun");
    if (!bun && !has_tool("npm")) {
        fprintf(stderr,
            "klangc: '%s' is a Tailwind stylesheet, and building it needs the Tailwind CLI\n"
            "       | help: install bun (https://bun.sh) or node, then try again\n"
            "       |       or build the CSS yourself and write it to %s/style.css\n", in, root);
        return 1;
    }

    char cmd[2048];
    /* `@import \"tailwindcss\"` is resolved from the project, not from the CLI, so
       the package has to actually be installed. Doing it here rather than leaving
       an error for the reader is the point of `web run` existing. */
    if (!file_exists("node_modules/tailwindcss/package.json")) {
        if (!file_exists("package.json")) {
            fprintf(stderr,
                "klangc: '%s' wants Tailwind, but there is no package.json to install it from\n"
                "       | help: `klangc new <name> --css tailwind` writes one, or add\n"
                "       |       tailwindcss and @tailwindcss/cli as devDependencies yourself\n", in);
            return 1;
        }
        snprintf(cmd, sizeof cmd, bun ? "bun install" : "npm install");
        printf("klangc: %s   (Tailwind is not installed yet)\n", cmd);
        if (system(cmd) != 0) {
            fprintf(stderr, "klangc: installing Tailwind failed\n");
            return 1;
        }
    }

    snprintf(cmd, sizeof cmd, "%s @tailwindcss/cli -i %s/input.css -o %s/style.css%s",
             bun ? "bunx" : "npx", root, root, minify ? " --minify" : "");
    printf("klangc: %s\n", cmd);
    if (system(cmd) != 0) {
        fprintf(stderr, "klangc: the Tailwind CLI failed\n");
        return 1;
    }
    return 0;
}

static int cmd_web(int argc, char **argv) {
    bool build_only = false, open_browser = true;
    const char *entry_arg = NULL;
    int port = 8080;

    if (argc < 1) {
        fprintf(stderr, "klangc: 'web' needs a subcommand - 'run' or 'build'\n");
        return 1;
    }
    if (strcmp(argv[0], "build") == 0) build_only = true;
    else if (strcmp(argv[0], "run") != 0) {
        fprintf(stderr, "klangc: unknown web subcommand '%s' - try 'run' or 'build'\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-open") == 0) open_browser = false;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "klangc: unknown option '%s'\n", argv[i]);
            return 1;
        } else entry_arg = argv[i];
    }

    const char *entry = find_entry(entry_arg);
    char *dir = dir_of(entry);
    char *stem = stem_of(entry);

    /* Where the page lives.
     *
     * A Klang project does not need an HTML file: std/html builds the markup and
     * std/css the styling, so the only thing a page has to contain is a script tag
     * and somewhere to mount into. klangc writes that shell itself.
     *
     * An index.html is still honoured when there is one — for a custom <meta>, a
     * font link, an analytics tag — and a web/ directory is honoured as the root
     * so that images and fonts have somewhere to live. Neither is required. */
    char root[512], probe[1100];
    const char *found = NULL;
    snprintf(probe, sizeof probe, "%s/index.html", dir);
    if (file_exists(probe)) { snprintf(root, sizeof root, "%s", dir); found = root; }
    if (!found) {
        /* A directory named after the program is its page directory, whether or
           not it holds an index.html — it usually will not, now that the markup
           lives in Klang; what it holds is the build output and any assets. */
        snprintf(root, sizeof root, "%s/%s", dir, stem);
        if (dir_exists(root)) found = root;
    }
    if (!found && dir_exists("web")) { snprintf(root, sizeof root, "web"); found = root; }
    if (!found) { snprintf(root, sizeof root, ".klang/%s", stem); found = root; }
    ensure_dir(root);
    snprintf(probe, sizeof probe, "%s/index.html", root);
    if (!file_exists(probe)) {
        write_default_index(probe, stem, root);
        printf("klangc: wrote '%s' - the page shell, since the markup is in Klang\n", probe);
    }

    if (build_css(root, build_only) != 0) return 1;

    ensure_dir(".klang");
    char cpath[512];
    snprintf(cpath, sizeof cpath, ".klang/%s.c", stem);

    char *lib = NULL;
    printf("klangc: compiling %s\n", entry);
    if (compile_file(entry, cpath, &lib, true) != 0) return 1;

    if (!has_tool("emcc")) {
        fprintf(stderr,
            "klangc: emcc is not on PATH, and a web build needs it\n"
            "       | help: install the Emscripten SDK - https://emscripten.org -\n"
            "       |       and source its emsdk_env script in this shell first\n");
        return 1;
    }

    char cmd[2048];
    int n = snprintf(cmd, sizeof cmd, "emcc -O2 %s", cpath);
    if (lib) n += snprintf(cmd + n, sizeof cmd - (size_t)n, " --js-library %s", lib);
    snprintf(cmd + n, sizeof cmd - (size_t)n,
             " -o %s/app.js -sALLOW_MEMORY_GROWTH -sEXPORTED_RUNTIME_METHODS=ccall", root);
    printf("klangc: %s\n", cmd);
    if (system(cmd) != 0) {
        fprintf(stderr, "klangc: emcc failed\n");
        return 1;
    }
    printf("klangc: built %s/app.js and %s/app.wasm\n", root, root);
    if (build_only) return 0;

    /* Bun first, then Node. Which one is in use gets printed: the two do differ,
       and a surprise is worse when the choice was invisible. */
    const char *js = has_tool("bun") ? "bun" : has_tool("node") ? "node" : NULL;
    if (!js) {
        fprintf(stderr,
            "klangc: no JavaScript runtime found, and the page needs one to be served\n"
            "       | help: install bun (https://bun.sh) or node, then try again.\n"
            "       |       `klangc web build` stops before this step if you would\n"
            "       |       rather serve the files yourself\n");
        return 1;
    }

    const char *serve_path = ".klang/serve.js";
    FILE *sf = fopen(serve_path, "wb");
    if (!sf) { fprintf(stderr, "klangc: cannot write '%s'\n", serve_path); return 1; }
    fputs(SERVE_JS, sf);
    fclose(sf);

    if (open_browser) {
        char open[256];
#if defined(_WIN32)
        snprintf(open, sizeof open, "start \"\" http://localhost:%d/ >NUL 2>NUL", port);
#elif defined(__APPLE__)
        snprintf(open, sizeof open, "open http://localhost:%d/ >/dev/null 2>&1", port);
#else
        snprintf(open, sizeof open, "xdg-open http://localhost:%d/ >/dev/null 2>&1 &", port);
#endif
        if (system(open) != 0) { /* no browser here; the URL is printed anyway */ }
    }

    snprintf(cmd, sizeof cmd, "%s %s %d %s", js, serve_path, port, root);
    printf("klangc: http://localhost:%d/  (ctrl-c to stop)\n", port);
    return system(cmd) == 0 ? 0 : 1;
}


/* ───────────────────────── new, and run ─────────────────────────
 *
 * `klangc new <name>` lays down a project that already builds and already runs.
 * Not a folder of empty files with TODOs in them: three kinds, each a working
 * program you can change a line of and see the change.
 *
 * The layout is the same for all three, because having one layout to learn is
 * worth more than having each kind be optimal:
 *
 *     name/
 *       src/main.kkg     the program
 *       web/index.html   the page  (web only)
 *       README.md        how to run it
 *       .gitignore       what not to commit
 */

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "klangc: cannot write '%s'\n", path); exit(1); }
    fputs(content, f);
    fclose(f);
    printf("  %s\n", path);
}

static const char *TEMPLATE_WEB =
    "// A Klang web page - all of it. There is no .html file and no .css file in\n"
    "// this project: the markup is std/html and the styling is std/css, both built\n"
    "// here as values. klangc writes the page shell at build time.\n"
    "//\n"
    "//     klangc web run\n"
    "\n"
    "import \"std/dom\"\n"
    "import \"std/html\"\n"
    "import \"std/css\"\n"
    "\n"
    "// An event handler runs long after main returned and cannot capture a local,\n"
    "// so a page's state lives at module level. `let mut` says it changes.\n"
    "let mut clicks = 0\n"
    "\n"
    "// `html.on` attaches the handler where the element is built, so the button\n"
    "// needs no id and there is no second list of wiring to keep in step.\n"
    "fn view() -> html.Node {\n"
    "    return html.div([], [\n"
    "        html.h1([], [html.text(\"Hello from Klang\")]),\n"
    "        html.p([], [\n"
    "            html.text(\"The page is yours. It is built in \"),\n"
    "            html.code([], [html.text(\"src/main.kkg\")]),\n"
    "            html.text(\".\"),\n"
    "        ]),\n"
    "        html.button([html.on(\"click\", \"onClick\")], [html.text(\"click me\")]),\n"
    "        html.span([html.id(\"count\")], [html.text(\"${clicks} clicks\")]),\n"
    "    ])\n"
    "}\n"
    "\n"
    "// Styling, also as values. The property names are functions, so a misspelled\n"
    "// one is a compile error rather than a rule the browser quietly ignores.\n"
    "fn styles() -> [css.Block] {\n"
    "    return [\n"
    "        css.rule(\":root\", [css.colorScheme(\"light dark\")]),\n"
    "        css.rule(\"body\", [\n"
    "            css.font(\"16px/1.5 system-ui, sans-serif\"),\n"
    "            css.maxWidth(\"32rem\"),\n"
    "            css.margin(\"4rem auto\"),\n"
    "            css.padding(\"0 1rem\"),\n"
    "        ]),\n"
    "        css.rule(\"button\", [\n"
    "            css.font(\"inherit\"),\n"
    "            css.padding(\".5rem 1rem\"),\n"
    "            css.border(\"1px solid #8886\"),\n"
    "            css.radius(\".4rem\"),\n"
    "            css.background(\"transparent\"),\n"
    "            css.color(\"inherit\"),\n"
    "            css.cursor(\"pointer\"),\n"
    "        ]),\n"
    "        css.rule(\"button:hover\", [css.background(\"#8882\")]),\n"
    "        css.rule(\"#count\", [css.display(\"block\"), css.marginTop(\"1rem\"),\n"
    "                            css.opacity(\".7\")]),\n"
    "    ]\n"
    "}\n"
    "\n"
    "fn render() {\n"
    "    dom.mount(\"#app\", view())\n"
    "}\n"
    "\n"
    "// JavaScript can call an `export fn` by name - which is how a button reaches\n"
    "// back into Klang. Its arguments and result have to be types JavaScript\n"
    "// understands, and the compiler holds you to that.\n"
    "export fn onClick() {\n"
    "    clicks += 1\n"
    "    render()\n"
    "}\n"
    "\n"
    "fn main() {\n"
    "    dom.setTitle(\"Hello from Klang\")\n"
    "    dom.useStyle(css.sheet(styles()))\n"
    "    // One listener on the container covers everything mounted under it, now\n"
    "    // and later, because it is not attached to the elements themselves.\n"
    "    dom.delegate(\"#app\", \"click\")\n"
    "    render()\n"
    "}\n";


/* Tailwind has to be told where the class names are, and for Klang most of them
   are inside string literals in .kkg files — somewhere it would never look. The
   @source lines are the whole trick. */
static const char *TEMPLATE_TAILWIND_CSS =
    "@import \"tailwindcss\";\n"
    "\n"
    "/* Klang builds HTML in string literals, so Tailwind is pointed at the source\n"
    "   as well as the page. Without these it would find nothing and emit nothing. */\n"
    "@source \"./index.html\";\n"
    "@source \"../src/**/*.kkg\";\n";

static const char *TEMPLATE_TAILWIND_PKG =
    "{\n"
    "  \"private\": true,\n"
    "  \"//\": \"Only Tailwind lives here. Klang itself needs no package manager.\",\n"
    "  \"devDependencies\": {\n"
    "    \"tailwindcss\": \"^4\",\n"
    "    \"@tailwindcss/cli\": \"^4\"\n"
    "  }\n"
    "}\n";

/* A Tailwind project's Klang source writes utility classes into strings, which is
   the case @source has to cover — so the template does it, and the scaffold test
   can check the class actually reached the stylesheet. */
static const char *TEMPLATE_WEB_TW =
    "// A Klang web page, styled with Tailwind.\n"
    "//\n"
    "//     klangc web run\n"
    "//\n"
    "// The markup is built here as values; index.html holds an empty <div id=\"app\">.\n"
    "// The utility classes below live in Klang string literals, and `web/input.css`\n"
    "// points Tailwind at this file with @source, which is what makes them work.\n"
    "\n"
    "import \"std/dom\"\n"
    "import \"std/html\"\n"
    "\n"
    "// An event handler runs long after main returned and cannot capture a local,\n"
    "// so a page's state lives at module level. `let mut` says it changes.\n"
    "let mut clicks = 0\n"
    "\n"
    "// `html.on` attaches the handler where the element is built, so the button\n"
    "// needs no id and there is no second list of wiring to keep in step.\n"
    "fn view() -> html.Node {\n"
    "    let tone = if clicks == 0 { \"bg-gray-500/10\" } else { \"bg-emerald-500/20\" }\n"
    "    return html.div([], [\n"
    "        html.h1([html.class(\"text-2xl font-semibold\")], [html.text(\"Hello from Klang\")]),\n"
    "        html.p([html.class(\"mt-1 text-sm opacity-70\")], [\n"
    "            html.text(\"The page is yours. It is built in \"),\n"
    "            html.code([], [html.text(\"src/main.kkg\")]),\n"
    "            html.text(\".\"),\n"
    "        ]),\n"
    "        html.button([html.on(\"click\", \"onClick\"),\n"
    "                     html.class(\"mt-6 rounded border border-gray-400/50 px-4 py-2 \"\n"
    "                              + \"hover:bg-gray-500/10\")],\n"
    "                    [html.text(\"click me\")]),\n"
    "        html.span([html.class(\"ml-3 rounded-full px-3 py-1 text-sm ${tone}\")],\n"
    "                  [html.text(\"${clicks} clicks\")]),\n"
    "    ])\n"
    "}\n"
    "\n"
    "fn render() {\n"
    "    dom.mount(\"#app\", view())\n"
    "}\n"
    "\n"
    "// JavaScript can call an `export fn` by name - which is how a button reaches\n"
    "// back into Klang. Its arguments and result have to be types JavaScript\n"
    "// understands, and the compiler holds you to that.\n"
    "export fn onClick() {\n"
    "    clicks += 1\n"
    "    render()\n"
    "}\n"
    "\n"
    "fn main() {\n"
    "    // One listener on the container covers everything mounted under it, now\n"
    "    // and later, because it is not attached to the elements themselves.\n"
    "    dom.delegate(\"#app\", \"click\")\n"
    "    render()\n"
    "}\n";

static const char *TEMPLATE_TAILWIND_HTML =
    "<!doctype html>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>%s</title>\n"
    "\n"
    "<!-- Built from input.css by `klangc web run`, which runs the Tailwind CLI. -->\n"
    "<link rel=\"stylesheet\" href=\"style.css\">\n"
    "\n"
    "<body class=\"mx-auto max-w-lg px-4 py-16 font-sans\">\n"
    "  <!-- Everything inside #app is built by src/main.kkg as Klang values, utility\n"
    "       classes included. There is no application markup here. -->\n"
    "  <div id=\"app\"></div>\n"
    "\n"
    "  <script src=\"app.js\"></script>\n"
    "</body>\n";

static const char *TEMPLATE_CLI =
    "// A Klang program.\n"
    "//\n"
    "//     klangc run src/main.kkg\n"
    "\n"
    "import \"std/list\"\n"
    "import \"std/string\" as str\n"
    "\n"
    "struct Task {\n"
    "    title: string,\n"
    "    done: bool,\n"
    "}\n"
    "\n"
    "fn describe(t: Task) -> string {\n"
    "    if t.done {\n"
    "        return \"[x] ${t.title}\"\n"
    "    }\n"
    "    return \"[ ] ${t.title}\"\n"
    "}\n"
    "\n"
    "fn main() {\n"
    "    let tasks = [\n"
    "        Task { title: \"read the spec\", done: true },\n"
    "        Task { title: \"write something\", done: false },\n"
    "    ]\n"
    "\n"
    "    for t in tasks {\n"
    "        println(describe(t))\n"
    "    }\n"
    "\n"
    "    let left = list.count(tasks, |t| !t.done)\n"
    "    println(\"${left} of ${tasks.len()} left\")\n"
    "\n"
    "    // Errors are values, and the compiler makes you handle them.\n"
    "    match str.parseInt(\"42\") {\n"
    "        Ok(n)  => println(\"parsed ${n}\")\n"
    "        Err(e) => println(\"not a number: ${e}\")\n"
    "    }\n"
    "}\n";

static const char *TEMPLATE_SERVER =
    "// A Klang HTTP server.\n"
    "//\n"
    "//     klangc run src/main.kkg\n"
    "//     curl http://127.0.0.1:8080/\n"
    "//\n"
    "// std/net does the unsafe socket work once; nothing below is unsafe, and\n"
    "// nothing below is about memory.\n"
    "\n"
    "import \"std/net\"\n"
    "import \"std/http\"\n"
    "import \"std/json\"\n"
    "\n"
    "const PORT = 8080\n"
    "\n"
    "fn handle(req: http.Request) -> http.Response {\n"
    "    if req.path == \"/\" {\n"
    "        return http.html(\"<h1>Served by Klang</h1><p>Try <a href=/health>/health</a>.\")\n"
    "    }\n"
    "    if req.path == \"/health\" {\n"
    "        return http.json(json.stringify(Obj([\n"
    "            json.field(\"ok\", Bool(true)),\n"
    "            json.field(\"heapBytes\", json.numOf(gcHeap())),\n"
    "        ])))\n"
    "    }\n"
    "    if req.path == \"/echo\" {\n"
    "        return http.ok(req.body)\n"
    "    }\n"
    "    return http.notFound()\n"
    "}\n"
    "\n"
    "fn main() {\n"
    "    match net.listenOn(PORT) {\n"
    "        Err(e) => println(\"cannot start: ${e}\")\n"
    "        Ok(listener) => {\n"
    "            println(\"listening on http://127.0.0.1:${PORT}\")\n"
    "            while true {\n"
    "                match net.acceptOne(listener) {\n"
    "                    Err(e)   => println(\"accept: ${e}\")\n"
    "                    Ok(conn) => http.serveOne(conn, handle)\n"
    "                }\n"
    "            }\n"
    "            net.closeListener(listener)\n"
    "        }\n"
    "    }\n"
    "}\n";

static const char *GITIGNORE_TEMPLATE =
    "# Build output. Everything here is generated from src/.\n"
    ".klang/\n"
    "web/app.js\n"
    "web/app.wasm\n"
    "*.o\n"
    "*.exe\n";

static int cmd_new(int argc, char **argv) {
    const char *name = NULL, *kind = "web", *css = "plain";
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--kind") == 0 && i + 1 < argc) kind = argv[++i];
        else if (strcmp(argv[i], "--css") == 0 && i + 1 < argc) css = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "klangc: unknown option '%s'\n", argv[i]);
            return 1;
        } else name = argv[i];
    }
    if (!name) {
        fprintf(stderr,
            "klangc: 'new' needs a name - `klangc new myapp`\n"
            "       | help: --kind web (the default), cli, or server\n"
            "       |       --css plain (the default) or tailwind\n");
        return 1;
    }
    bool web = strcmp(kind, "web") == 0;
    bool cli = strcmp(kind, "cli") == 0;
    bool server = strcmp(kind, "server") == 0;
    if (!web && !cli && !server) {
        fprintf(stderr, "klangc: no such kind '%s' - try web, cli or server\n", kind);
        return 1;
    }
    bool tailwind = strcmp(css, "tailwind") == 0;
    if (!tailwind && strcmp(css, "plain") != 0) {
        fprintf(stderr, "klangc: no such stylesheet '%s' - try plain or tailwind\n", css);
        return 1;
    }
    if (tailwind && !web) {
        fprintf(stderr, "klangc: --css only means something for a web project\n");
        return 1;
    }

    char path[1100];
    snprintf(path, sizeof path, "%s/src/main.kkg", name);
    if (file_exists(path)) {
        fprintf(stderr,
            "klangc: '%s' already has a src/main.kkg - refusing to overwrite it\n", name);
        return 1;
    }

    printf("klangc: creating %s (%s)\n", name, kind);
    snprintf(path, sizeof path, "%s/src", name);
    ensure_dir(path);

    snprintf(path, sizeof path, "%s/src/main.kkg", name);
    write_file(path, tailwind ? TEMPLATE_WEB_TW
                    : web     ? TEMPLATE_WEB
                    : cli     ? TEMPLATE_CLI : TEMPLATE_SERVER);

    /* No index.html and no .css: the markup comes from std/html, the styling from
       std/css, and klangc writes the page shell at build time. A plain web project
       is Klang and nothing else.

       Tailwind is the exception, because Tailwind is a separate compiler with its
       own input file — choosing it is choosing to have one. */
    if (tailwind) {
        snprintf(path, sizeof path, "%s/web", name);
        ensure_dir(path);
        snprintf(path, sizeof path, "%s/web/index.html", name);
        char *html = malloc(strlen(TEMPLATE_TAILWIND_HTML) + 2 * strlen(name) + 8);
        sprintf(html, TEMPLATE_TAILWIND_HTML, name, name);
        write_file(path, html);
        free(html);
        snprintf(path, sizeof path, "%s/web/input.css", name);
        write_file(path, TEMPLATE_TAILWIND_CSS);
        snprintf(path, sizeof path, "%s/package.json", name);
        write_file(path, TEMPLATE_TAILWIND_PKG);
    }

    snprintf(path, sizeof path, "%s/.gitignore", name);
    if (tailwind) {
        SB gi; sb_init(&gi);
        sb_append(&gi, GITIGNORE_TEMPLATE);
        sb_append(&gi, "\n# Built from web/input.css by the Tailwind CLI.\n"
                       "web/style.css\nnode_modules/\n");
        write_file(path, gi.data);
    } else write_file(path, GITIGNORE_TEMPLATE);

    /* A README that says how to run this exact project, since a reader who has
       just been handed the directory should not have to go looking. */
    SB rm; sb_init(&rm);
    sb_appendf(&rm, "# %s\n\nA Klang %s project.\n\n", name,
               web ? "web" : cli ? "command-line" : "server");
    sb_appendf(&rm, "```sh\ncd %s\n", name);
    if (web) sb_append(&rm, "klangc web run          # build, serve, open a browser\n"
                            "klangc web build        # build only\n");
    else sb_append(&rm, "klangc run src/main.kkg\n");
    sb_append(&rm, "```\n\n");
    if (web) {
        sb_append(&rm,
            "`src/main.kkg` is the whole application. `web/index.html` is the page it\n"
            "renders into; `app.js` and `app.wasm` are built next to it.\n\n");
        if (tailwind)
            sb_append(&rm,
                "Styling is Tailwind. `web/input.css` is the source and `klangc web run`\n"
                "compiles it to `web/style.css` with the Tailwind CLI, so that file is\n"
                "generated and not committed. The `@source` lines in `input.css` point\n"
                "Tailwind at `src/**/*.kkg` as well as the page, because Klang builds\n"
                "HTML in string literals and the class names live there.\n\n");
        else
            sb_append(&rm,
                "Styling is `web/style.css`, served as-is — nothing compiles it. For\n"
                "Tailwind instead, `klangc new` takes `--css tailwind`.\n\n");
        sb_append(&rm,
            "Needs [Emscripten](https://emscripten.org) to build, and\n"
            "[bun](https://bun.sh) or node to serve.\n");
    }
    else
        sb_append(&rm, "`src/main.kkg` is the whole program. Needs a C99 compiler.\n");
    snprintf(path, sizeof path, "%s/README.md", name);
    write_file(path, rm.data);

    printf("\nklangc: done. Next:\n\n    cd %s\n    %s\n\n", name,
           web ? "klangc web run" : "klangc run src/main.kkg");
    return 0;
}

/* `klangc run` for a native program: compile, build, execute. The same one-step
   convenience `web run` gives a page, so the two kinds of program are reached the
   same way and neither is the awkward one. */
/* Klang is a compiled language: the end product is a binary the operating system
 * runs, linked against nothing but the C library.
 *
 * C is the intermediate form, the way assembly is for a traditional compiler — it
 * is kept in .klang/ so it can be read, but handling it is not part of using the
 * language. Nothing here touches JavaScript; a program with no `js fn` has no
 * JavaScript-boundary code emitted into it at all.
 *
 * Returns the path to the executable, or NULL on failure.
 */
static char *build_native(const char *entry, const char *out, bool debug, bool statik,
                          bool quiet) {
    char *stem = stem_of(entry);
    ensure_dir(".klang");

    char cpath[512];
    static char exe[600];
    snprintf(cpath, sizeof cpath, ".klang/%s.c", stem);
    if (out) snprintf(exe, sizeof exe, "%s", out);
#ifdef _WIN32
    else snprintf(exe, sizeof exe, "%s.exe", stem);
#else
    else snprintf(exe, sizeof exe, "%s", stem);
#endif

    char *lib = NULL;
    if (compile_file(entry, cpath, &lib, true) != 0) return NULL;
    if (lib) {
        fprintf(stderr,
            "klangc: '%s' has `js fn` declarations, so it needs a browser to run in\n"
            "       | help: build it with `klangc web build %s`\n", entry, entry);
        return NULL;
    }

    const char *cc = has_tool("cc") ? "cc" : has_tool("gcc") ? "gcc"
                   : has_tool("clang") ? "clang" : NULL;
    if (!cc) {
        fprintf(stderr,
            "klangc: no C compiler on PATH, and Klang compiles through C the way\n"
            "        other compilers go through assembly\n"
            "       | help: install gcc or clang\n");
        return NULL;
    }

    char cmd[2048];
    int n = snprintf(cmd, sizeof cmd, "%s -std=c99 %s%s -o %s %s",
                     cc, debug ? "-O0 -g" : "-O2", statik ? " -static" : "", exe, cpath);
    for (int i = 0; i < g_c_links.count; i++)
        n += snprintf(cmd + n, sizeof cmd - (size_t)n, " -l%s", VEC_PTR(&g_c_links, i, char));
    if (g_uses_threads) n += snprintf(cmd + n, sizeof cmd - (size_t)n, " -lpthread");
    snprintf(cmd + n, sizeof cmd - (size_t)n, " -lm");
    if (!quiet) printf("klangc: %s\n", cmd);
    if (system(cmd) != 0) {
        fprintf(stderr, "klangc: the C compiler rejected the generated code\n");
        return NULL;
    }
    return exe;
}

static void parse_build_flags(int argc, char **argv, const char **entry, const char **out,
                              bool *debug, bool *statik) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) *out = argv[++i];
        else if (strcmp(argv[i], "--debug") == 0) *debug = true;
        else if (strcmp(argv[i], "--release") == 0) *debug = false;
        else if (strcmp(argv[i], "--static") == 0) *statik = true;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "klangc: unknown option '%s'\n", argv[i]);
            exit(1);
        } else *entry = argv[i];
    }
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int cmd_build(int argc, char **argv) {
    const char *arg = NULL, *out = NULL;
    bool debug = false, statik = false;
    parse_build_flags(argc, argv, &arg, &out, &debug, &statik);

    const char *entry = find_entry(arg);
    char *exe = build_native(entry, out, debug, statik, false);
    if (!exe) return 1;

    long bytes = file_size(exe);
    printf("klangc: %s -> %s", entry, exe);
    if (bytes > 0) printf("  (%ld bytes%s)", bytes, statik ? ", self-contained" : "");
    printf("\n");
    return 0;
}

static int cmd_run(int argc, char **argv) {
    const char *arg = NULL, *out = NULL;
    bool debug = false, statik = false;
    parse_build_flags(argc, argv, &arg, &out, &debug, &statik);

    const char *entry = find_entry(arg);
    char *stem = stem_of(entry);
    char tmp[600];
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, ".klang/%s.exe", stem);
#else
    snprintf(tmp, sizeof tmp, ".klang/%s", stem);
#endif
    /* `run` keeps its binary out of the way; `build` is what puts one where you
       asked for it. */
    if (!build_native(entry, out ? out : tmp, debug, statik, true)) return 1;

    /* Whatever the program exits with is what `klangc run` exits with, so this
       composes in a script the way running the binary directly would. */
    char runline[620];
#ifdef _WIN32
    snprintf(runline, sizeof runline, "%s", out ? out : tmp);
#else
    snprintf(runline, sizeof runline, "./%s", out ? out : tmp);
#endif
    int rc = system(runline);
#ifdef _WIN32
    return rc;
#else
    return rc == -1 ? 1 : (rc & 0x7f) ? 128 + (rc & 0x7f) : (rc >> 8) & 0xff;
#endif
}
int main(int argc, char **argv) {
    g_exe_dir = dir_of(argv[0]);

    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("klangc - the Klang compiler (0.22)\n\n"
               "Start something:\n"
               "  klangc new <name>                A project that already runs\n"
               "    --kind web|cli|server          What sort (default: web)\n"
               "    --css plain|tailwind           Stylesheet for a web project\n"
               "\n"
               "Build a program:\n"
               "  klangc build [file.kkg]          Compile to a native executable\n"
               "    -o <name>                      Name the executable\n"
               "    --debug                        No optimization, with symbols\n"
               "    --static                       Link everything in, so it needs no libc\n"
               "  klangc run [file.kkg]            Build and execute it\n"
               "\n"
               "Build a page:\n"
               "  klangc web run [file.kkg]        Build for the browser and serve it\n"
               "  klangc web build [file.kkg]      Build for the browser and stop\n"
               "    --port <n>                     Port to serve on (default 8080)\n"
               "    --no-open                      Do not open a browser\n"
               "\n"
               "The intermediate C, if you want to read it:\n"
               "  klangc <file.kkg>                Emit C to output.c\n"
               "  klangc <file.kkg> -o <out.c>     Emit C to a specific file\n"
               "                                   (`build` keeps its copy in .klang/)\n"
               "\n"
               "  klangc --version                 Show version\n"
               "  klangc --help                    Show this help\n"
               "\n"
               "`run` and `web run` look for main.kkg, src/main.kkg, app.kkg or web.kkg\n"
               "when no file is named. `web run` serves with bun if it is installed and\n"
               "node otherwise, and uses your index.html - beside the program, in a\n"
               "directory named after it, or in web/.\n");
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0) { printf("klangc 0.22.0\n"); return 0; }
    if (strcmp(argv[1], "new") == 0) return cmd_new(argc - 2, argv + 2);
    if (strcmp(argv[1], "build") == 0) return cmd_build(argc - 2, argv + 2);
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc - 2, argv + 2);
    if (strcmp(argv[1], "web") == 0) return cmd_web(argc - 2, argv + 2);

    const char *in_path = argv[1];
    const char *out_path = "output.c";
    if (argc >= 4 && strcmp(argv[2], "-o") == 0) out_path = argv[3];
    return compile_file(in_path, out_path, NULL, false);
}
