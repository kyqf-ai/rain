#!/usr/bin/env bash
# Same as build_win.bat, for an MSYS2 MinGW shell (used by CI).  usage: tools/build_win.sh [test]
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f third_party/sqlite3.c ] || { echo "third_party/sqlite3.c missing - run tools/fetch_sqlite.sh"; exit 1; }
CF="-std=gnu11 -O2 -Wall -D_WIN32_WINNT=0x0601 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION=1 -Ithird_party -finput-charset=UTF-8 -fexec-charset=UTF-8"
if [ "${1:-}" = "test" ]; then
  gcc $CF -o tests/test_proto.exe  tests/test_proto.c  src/proto.c src/crc16.c src/util.c src/platform.c
  gcc $CF -o tests/test_config.exe tests/test_config.c src/config.c src/log.c src/util.c src/platform.c
  gcc $CF -o tests/test_ui.exe     tests/test_ui.c     src/ui_logic.c
  gcc $CF -o tests/test_store.exe  tests/test_store.c  src/store.c src/query.c src/log.c src/util.c src/platform.c third_party/sqlite3.c
  for t in proto config ui store; do ./tests/test_$t.exe; done
  echo "all unit tests passed"
else
  mkdir -p build dist
  windres --codepage=65001 -Ires -Isrc -i res/rain.rc -o build/rain_res.o
  gcc $CF -mwindows -static -s -o dist/rain.exe \
    src/util.c src/platform.c src/log.c src/crc16.c src/proto.c src/config.c src/store.c \
    src/query.c src/web.c src/web_page.c \
    src/collector.c src/ui_logic.c src/cli.c src/serial_win.c src/gui_win.c third_party/sqlite3.c \
    build/rain_res.o \
    -lgdi32 -luser32 -lshell32 -ladvapi32 -lws2_32
  echo "built dist/rain.exe"
fi
