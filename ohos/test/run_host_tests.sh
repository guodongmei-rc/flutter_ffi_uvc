#!/bin/sh
# Runs the pure (no usbManager, no ArkUI) UVC protocol modules under Node on the
# development machine. The .ets sources are copied as .ts so Node's built-in
# type stripping can execute them unchanged; nothing here runs on the device.
#
# Usage: sh ohos/test/run_host_tests.sh        (needs Node >= 23.6)
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src/main/ets/uvc"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

mkdir -p "$OUT/uvc" "$OUT/test"
printf '{"type":"module"}\n' > "$OUT/package.json"

for f in "$SRC"/*.ets; do
  [ -e "$f" ] || continue
  name="$(basename "$f" .ets)"
  # Relative imports have no extension in ArkTS; Node's ESM loader needs one.
  sed -E "s#from '(\./[A-Za-z0-9_]+)'#from '\1.ts'#g" "$f" > "$OUT/uvc/$name.ts"
done

cp "$HERE"/*.ts "$OUT/test/"

node --test "$OUT"/test/*.test.ts
