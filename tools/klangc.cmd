@echo off
rem klangc, for a Windows machine with no C compiler on it.
rem
rem Klang compiles through C, and its browser half needs Emscripten. Rather than
rem ask for either, this runs the real compiler inside the emscripten/emsdk image
rem and passes your arguments straight through, so `klangc web run` here means
rem what it means everywhere else.
rem
rem     tools\klangc.cmd new myapp
rem     tools\klangc.cmd web run examples\fullstack\app.kkg
rem
rem Put tools\ on PATH and it is just `klangc`.
rem
rem The repository is mounted, and your working directory inside the container is
rem the same one you are standing in, so relative paths work.

setlocal EnableDelayedExpansion

for %%I in ("%~dp0..") do set "REPO=%%~fI"

rem Where you are, relative to the repository, as a container path.
call set "REL=%%CD:%REPO%=%%"
if "%REL%"=="%CD%" (
    echo klangc: run this from inside %REPO%
    exit /b 1
)
set "REL=%REL:\=/%"

rem A server has to be reachable from your browser, so the two commands that run
rem one publish a port — and different ports, because two containers cannot both
rem claim the same one. `web run` serves the page on 8080; `run` is how a Klang
rem server is started, and the examples listen on 8099.
set "PORTS="
if /I "%~1"=="web" set "PORTS=-p 8080:8080"
if /I "%~1"=="run" set "PORTS=-p 8099:8099"

rem The shell half lives in its own file. Embedding it here would mean a shell
rem script inside a batch string, quoted twice, which is how the first attempt at
rem this failed.
rem
rem No -it: it fails whenever stdin is not a terminal, and nothing here reads
rem input. Ctrl-C still stops a running server, because docker forwards signals
rem to the container unless told otherwise.
docker run --rm %PORTS% -v "%REPO%":/w -w "/w%REL%" emscripten/emsdk sh /w/tools/klangc-in-docker.sh %*
