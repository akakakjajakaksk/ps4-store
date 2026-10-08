#!/bin/sh
set -eu
TASK_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
FTP_TEST_BIN=$(mktemp /tmp/peppy-ftp-tests-XXXXXX)
trap 'rm -f "$FTP_TEST_BIN"' EXIT HUP INT TERM
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
    -I"$TASK_ROOT/tests/ftp_receiver/stubs" \
    "$TASK_ROOT/tests/ftp_receiver/ftp_test.cpp" -o "$FTP_TEST_BIN"
"$FTP_TEST_BIN"
