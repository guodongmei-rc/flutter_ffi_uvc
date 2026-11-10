#!/bin/sh
# Builds and runs the platform-independent native code (the frame assembler,
# the NAL parser) on the development machine. Nothing here runs on the device.
#
# Usage: sh ohos/test/run_native_tests.sh        (needs a C++17 compiler)
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

for name in assembler nal; do
  "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -o "$OUT/${name}_test" "$HERE/native/${name}_test.cpp"
  "$OUT/${name}_test"
done
