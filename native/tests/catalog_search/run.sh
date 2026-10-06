#!/bin/sh
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/peppy-catalog-search.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

if [ "${SANITIZE:-0}" = 1 ]; then
    "${CXX:-c++}" -std=c++11 -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror \
        -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -no-pie \
        "$test_dir/search_test.cpp" -o "$build_dir/search_test"
else
    "${CXX:-c++}" -std=c++11 -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror \
        "$test_dir/search_test.cpp" -o "$build_dir/search_test"
fi

if [ "${SANITIZE:-0}" = 1 ]; then
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$build_dir/search_test"
else
    "$build_dir/search_test"
fi
