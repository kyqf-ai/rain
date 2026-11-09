# Linux / MinGW (make) build. For Windows without make see build_win.bat.
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -D_GNU_SOURCE
SRC     := src/util.c src/platform.c src/log.c src/crc16.c src/proto.c src/config.c src/store.c \
           src/query.c src/collector.c src/ui_logic.c src/cli.c src/web.c src/web_page.c
MAIN    := src/main.c
UNAME   := $(shell uname -s 2>/dev/null)

ifeq ($(OS),Windows_NT)
  SRC  += src/serial_win.c
  LIBS := -lws2_32
  EXE  := .exe
else
  SRC  += src/serial_posix.c
  LIBS := -lpthread
endif

# SQLite: use third_party/sqlite3.c (amalgamation) if present, else the system library.
# STUB=1 is only for the sandbox where just libsqlite3.so exists (no headers).
ifeq ($(STUB),1)
  INC   := -Itests/sqlite_stub
  LIBS  += -l:libsqlite3.so.0
else ifneq ($(wildcard third_party/sqlite3.c),)
  INC   := -Ithird_party
  SRC   += third_party/sqlite3.c
  CFLAGS += -DSQLITE_THREADSAFE=1
  ifneq ($(OS),Windows_NT)
    LIBS += -ldl -lm
  endif
else
  LIBS += -lsqlite3
endif

all: rain$(EXE)
rain$(EXE): $(SRC) $(MAIN)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRC) $(MAIN) $(LIBS)

tests/test_proto$(EXE): tests/test_proto.c $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_proto.c $(SRC) $(LIBS)
tests/test_store$(EXE): tests/test_store.c $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_store.c $(SRC) $(LIBS)

tests/test_ui$(EXE): tests/test_ui.c $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_ui.c $(SRC) $(LIBS)
tests/test_ctl$(EXE): tests/test_ctl.c $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_ctl.c $(SRC) $(LIBS)
tests/test_config$(EXE): tests/test_config.c $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_config.c $(SRC) $(LIBS)

test: rain$(EXE) tests/test_proto$(EXE) tests/test_store$(EXE) tests/test_config$(EXE) tests/test_ui$(EXE) tests/test_ctl$(EXE)
	./tests/test_proto$(EXE)
	./tests/test_config$(EXE)
	./tests/test_ui$(EXE)
	./tests/test_store$(EXE)
	python3 tests/e2e.py

clean:
	rm -f rain rain.exe tests/test_proto tests/test_store tests/test_config tests/test_ui tests/test_ctl tests/*.exe test_*.db* test_*.ini
.PHONY: all test clean
