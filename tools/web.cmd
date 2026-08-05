@echo off
rem `web run` and `web build`, as asked for.
rem
rem     web run                     the program in this directory, served
rem     web run app.kkg             a particular one
rem     web build                   built, not served
rem
rem Exactly `klangc web ...`, with the klangc left off, because running a page is
rem common enough to deserve its own word.

"%~dp0klangc.cmd" web %*
