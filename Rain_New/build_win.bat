@echo off
setlocal
cd /d "%~dp0"
rem ------------------------------------------------------------------------------------------
rem  Windows build with MinGW-w64 (gcc must be on PATH; 32-bit gcc = 32-bit exe, 64-bit gcc = 64-bit exe).
rem    build_win.bat          build dist\rain.exe (tray GUI)
rem    build_win.bat test     build and run the portable unit tests
rem  SQLite is needed once:   powershell -ExecutionPolicy Bypass -File tools\fetch_sqlite.ps1
rem ------------------------------------------------------------------------------------------
if not exist third_party\sqlite3.c (
  echo third_party\sqlite3.c not found.
  echo Run:  powershell -ExecutionPolicy Bypass -File tools\fetch_sqlite.ps1
  exit /b 1
)
where gcc >nul 2>nul
if errorlevel 1 (
  echo gcc not found on PATH. Install MinGW-w64 ^(MSYS2, or the standalone build from winlibs.com^) and retry.
  exit /b 1
)
set CF=-std=gnu11 -O2 -Wall -D_WIN32_WINNT=0x0601 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION=1 -Ithird_party -finput-charset=UTF-8 -fexec-charset=UTF-8
if /i "%1"=="test" goto test

where windres >nul 2>nul
if errorlevel 1 (
  echo windres not found on PATH ^(it ships with MinGW-w64^).
  exit /b 1
)
if not exist build mkdir build
if not exist dist mkdir dist
windres --codepage=65001 -Ires -Isrc -i res\rain.rc -o build\rain_res.o
if errorlevel 1 ( echo RESOURCE BUILD FAILED & exit /b 1 )

gcc %CF% -mwindows -static -s -o dist\rain.exe ^
  src\util.c src\platform.c src\log.c src\crc16.c src\proto.c src\config.c src\store.c ^
  src\query.c src\web.c src\web_page.c ^
  src\collector.c src\ui_logic.c src\cli.c src\serial_win.c src\gui_win.c third_party\sqlite3.c ^
  build\rain_res.o ^
  -lgdi32 -luser32 -lshell32 -ladvapi32 -lws2_32
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo built dist\rain.exe
exit /b 0

:test
gcc %CF% -o tests\test_proto.exe  tests\test_proto.c  src\proto.c src\crc16.c src\util.c src\platform.c || exit /b 1
gcc %CF% -o tests\test_config.exe tests\test_config.c src\config.c src\log.c src\util.c src\platform.c || exit /b 1
gcc %CF% -o tests\test_ui.exe     tests\test_ui.c     src\ui_logic.c || exit /b 1
gcc %CF% -o tests\test_store.exe  tests\test_store.c  src\store.c src\query.c src\log.c src\util.c src\platform.c third_party\sqlite3.c || exit /b 1
tests\test_proto.exe  || exit /b 1
tests\test_config.exe || exit /b 1
tests\test_ui.exe     || exit /b 1
tests\test_store.exe  || exit /b 1
echo all unit tests passed
exit /b 0
