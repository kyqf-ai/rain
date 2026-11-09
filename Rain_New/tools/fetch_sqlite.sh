#!/usr/bin/env bash
# Download the SQLite amalgamation into third_party/ (needs curl + unzip).
set -euo pipefail
cd "$(dirname "$0")/.."
VER=3450100; YEAR=2024
curl -fL -o /tmp/sqlite.zip "https://www.sqlite.org/$YEAR/sqlite-amalgamation-$VER.zip"
unzip -o -j /tmp/sqlite.zip "sqlite-amalgamation-$VER/sqlite3.c" "sqlite-amalgamation-$VER/sqlite3.h" -d third_party
echo "SQLite $VER ready in third_party/"
