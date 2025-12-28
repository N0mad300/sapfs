@echo off

set CC=cl
set CFLAGS=/W4 /O2
set OUTDIR=dist
set OUT=%OUTDIR%\audio_player.exe

set SRC= ^
    src\main.c ^
    src\audio_decoder\wave_parser.c ^
    src\audio_decoder\audio_decoder.c ^
    src\audio_output\audio_output.c ^
    src\audio_output\wasapi_output.c ^
    src\utils.c

set LIBS=ole32.lib

if not exist %OUTDIR% mkdir %OUTDIR%

%CC% %CFLAGS% ^
    /Fe:%OUT% ^
    /Fo:%OUTDIR%\ ^
    %SRC% ^
    %LIBS%
