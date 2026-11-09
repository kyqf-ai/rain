#!/bin/sh
# ThreadSanitizer run of the GUI control path (collector thread vs a polling reader). Linux only.
set -e
cd "$(dirname "$0")/.."
S=src; T=/tmp/rain_tsan; rm -rf $T; mkdir -p $T
gcc -std=gnu11 -O1 -g -fsanitize=thread -Wall -Wextra -D_GNU_SOURCE -Itests/sqlite_stub -o $T/test_ctl tests/test_ctl.c \
  $S/util.c $S/platform.c $S/log.c $S/crc16.c $S/proto.c $S/config.c $S/store.c $S/collector.c $S/ui_logic.c $S/serial_posix.c \
  -l:libsqlite3.so.0 -lpthread
python3 tools/devsim.py > $T/pty.txt 2>/dev/null & DEV=$!
sleep 1
set +e
TSAN_OPTIONS="halt_on_error=0 exitcode=66" $T/test_ctl "$(head -1 $T/pty.txt)" $T/t.db > $T/out.txt 2> $T/err.txt
RC=$?
kill $DEV 2>/dev/null
tail -1 $T/out.txt
echo "ThreadSanitizer reports: $(grep -c 'WARNING: ThreadSanitizer' $T/err.txt)  (exit code $RC)"
[ $RC -eq 0 ]
