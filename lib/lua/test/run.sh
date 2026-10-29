#!/bin/sh
# Builds and runs the sandbox tests on the desktop (no ESP-IDF needed).
set -e
cd "$(dirname "$0")/../../.."
OUT=${TMPDIR:-/tmp}/yos_test_sandbox
c++ -std=c++17 -O1 -Ilib/lua/src -Isrc lib/lua/test/test_sandbox.cpp src/script/sandbox.cpp lib/lua/src/lua_unity.cpp -o "$OUT"
"$OUT"
