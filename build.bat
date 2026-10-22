@echo off
rem Build TarkovCrosshair.exe with MinGW-w64 (gcc) or llvm-mingw (clang) on PATH.
if not exist build mkdir build
windres res\app.rc -O coff -o build\app.res.o || goto :err
gcc -std=c11 -O2 -municode -mwindows src\main.c build\app.res.o -o build\TarkovCrosshair.exe -luser32 -lgdi32 -lshell32 -lcomdlg32 -lmsimg32 -s || goto :err
echo Built build\TarkovCrosshair.exe
exit /b 0
:err
echo Build failed.
exit /b 1
