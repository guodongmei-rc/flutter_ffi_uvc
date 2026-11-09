#!/bin/sh
# Builds and runs the platform-independent native code (the frame assembler)
# on the development machine. Nothing here runs on the device.
#
# Usage: sh ohos/test/run_native_tests.sh        (needs a C++17 compiler)
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -o "$OUT/assembler_test" "$HERE/native/assembler_test.cpp"
"$OUT/assembler_test"
