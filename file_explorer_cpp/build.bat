@echo off
rem Build FileExplorer.exe with MinGW-w64 (MSYS2).
rem  1. Install MSYS2 from https://www.msys2.org
rem  2. In the "MSYS2 UCRT64" terminal:  pacman -S mingw-w64-ucrt-x86_64-gcc
rem  3. Add C:\msys64\ucrt64\bin to PATH (or run this from that terminal), then run build.bat
setlocal
cd /d "%~dp0"
where g++ >nul 2>nul || (
  echo g++ not found. Install MSYS2 + mingw-w64-ucrt-x86_64-gcc and add C:\msys64\ucrt64\bin to PATH.
  exit /b 1
)
if not exist build mkdir build
set FLAGS=-std=c++17 -O2 -municode -Wall -Wno-unused-parameter -Wno-missing-field-initializers
set LIBS=-lshlwapi -lshell32 -lole32 -luuid -lgdiplus -lmsimg32 -ldwmapi -luxtheme -lcomctl32 -loleaut32 -lgdi32

for %%f in (util theme gfx fs ui fileview chrome app app_actions panes quicklook dialogs transfer everything main) do (
  echo compiling %%f.cpp
  g++ %FLAGS% -c src\%%f.cpp -o build\%%f.o || exit /b 1
)
if not exist res\file_explorer.ico (
  rem no icon yet: build once without it, let the program draw it, then build again
  g++ -municode -mwindows -static build\*.o -o build\make_icon.exe %LIBS% || exit /b 1
  build\make_icon.exe icon res\file_explorer.ico
)
windres res\app.rc -O coff -o build\app_res.o || exit /b 1
g++ -municode -mwindows -static -s build\util.o build\theme.o build\gfx.o build\fs.o build\ui.o build\fileview.o ^
    build\chrome.o build\app.o build\app_actions.o build\panes.o build\quicklook.o build\dialogs.o build\transfer.o build\everything.o build\main.o ^
    build\app_res.o -o FileExplorer.exe %LIBS% || exit /b 1
echo.
echo Built FileExplorer.exe
