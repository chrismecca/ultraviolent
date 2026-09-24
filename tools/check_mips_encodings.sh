#!/bin/sh
# Compares the test assembler's encodings with GNU as for the R10000. Optional developer check:
# it needs a MIPS cross assembler (Debian: binutils-mips64-linux-gnuabi64) and skips otherwise.
set -eu

AS=${MIPS_AS:-mips64-linux-gnuabi64-as}
OBJCOPY=${MIPS_OBJCOPY:-mips64-linux-gnuabi64-objcopy}
CXX=${CXX:-c++}

if ! command -v "$AS" >/dev/null 2>&1 || ! command -v "$OBJCOPY" >/dev/null 2>&1; then
    echo "skip: $AS or $OBJCOPY not found"
    exit 0
fi

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$CXX" -std=c++23 -I"$root/tests" "$root/tools/mips_encoding_check.cpp" -o "$work/check"
(cd "$work" && ./check)
"$AS" -march=r10000 -mabi=64 -EB "$work/check.s" -o "$work/check.o"
"$OBJCOPY" -O binary -j .text "$work/check.o" "$work/check.bin"

python3 - "$work/check.bin" "$work/check.expected" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
words = struct.unpack('>%dI' % (len(data) // 4), data)
expected = [line.split(' ', 1) for line in open(sys.argv[2]).read().splitlines()]
bad = [(text, hexword, words[i]) for i, (hexword, text) in enumerate(expected)
       if int(hexword, 16) != words[i]]
for text, ours, theirs in bad:
    print(f"mismatch: {text}: ours {ours}, GNU as {theirs:08x}")
print(f"{len(expected)} encodings checked, {len(bad)} mismatches")
sys.exit(1 if bad else 0)
PY
