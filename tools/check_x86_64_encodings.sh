#!/bin/sh
# Compares the tier-1 x86-64 encoder with GNU as. Usage: check_x86_64_encodings.sh GENERATOR
# (the x86_64_encoding_check program). Skips when as or objcopy is missing.
set -eu

generator=$1
if ! command -v as >/dev/null 2>&1 || ! command -v objcopy >/dev/null 2>&1; then
    echo "skip: as or objcopy not found"
    exit 0
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && "$generator")
as --64 "$work/check.s" -o "$work/check.o"
objcopy -O binary -j .text "$work/check.o" "$work/check.bin"

python3 - "$work/check.bin" "$work/check.expected" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
at, bad, count = 0, 0, 0
for line in open(sys.argv[2]).read().splitlines():
    ours, text = line.split(' ', 1)
    n = len(ours) // 2
    theirs = data[at:at + n].hex()
    count += 1
    if theirs != ours:
        print(f"mismatch: {text}: ours {ours}, GNU as {data[at:at + 16].hex()}")
        bad += 1
        if bad > 20:
            break
    at += n
if bad == 0 and at != len(data):
    print(f"length mismatch: ours {at} bytes, GNU as {len(data)}")
    bad += 1
print(f"{count} encodings checked, {bad} mismatches")
sys.exit(1 if bad else 0)
PY
