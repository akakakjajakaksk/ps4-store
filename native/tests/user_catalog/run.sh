#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/peppy-user-catalog.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
if [ "${SANITIZE:-0}" = 1 ]; then
    "${CXX:-c++}" -std=c++11 -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror \
        -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-pie -no-pie \
        "$test_dir/user_catalog_test.cpp" "$test_dir/../../user_catalog.cpp" -o "$build_dir/user_catalog_test"
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$build_dir/user_catalog_test"
    "${CXX:-c++}" -std=c++11 -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror \
        -DPEPPY_USER_CATALOG_NATIVE_FILE_ABI=1 -g -fno-omit-frame-pointer \
        -fsanitize=address,undefined -fno-pie -no-pie \
        "$test_dir/user_catalog_test.cpp" "$test_dir/../../user_catalog.cpp" \
        "$test_dir/native_file_abi_stubs.cpp" -Wl,--wrap=fstat -Wl,--wrap=lstat \
        -o "$build_dir/user_catalog_native_abi_test"
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$build_dir/user_catalog_native_abi_test"
else
    "${CXX:-c++}" -std=c++11 -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror \
        "$test_dir/user_catalog_test.cpp" "$test_dir/../../user_catalog.cpp" -o "$build_dir/user_catalog_test"
    "$build_dir/user_catalog_test"
    "${CXX:-c++}" -std=c++11 -O2 -D_FORTIFY_SOURCE=2 -Wall -Wextra -Werror \
        -DPEPPY_USER_CATALOG_NATIVE_FILE_ABI=1 \
        "$test_dir/user_catalog_test.cpp" "$test_dir/../../user_catalog.cpp" \
        "$test_dir/native_file_abi_stubs.cpp" -Wl,--wrap=fstat -Wl,--wrap=lstat \
        -o "$build_dir/user_catalog_native_abi_test"
    "$build_dir/user_catalog_native_abi_test"
fi
