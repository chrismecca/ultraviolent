#!/bin/sh
# Private checkpoint: runs the user's IP27 PROM image from power-on and prints the PROM's
# progress LED sequence (PRM Table 3-1) and the first unmodeled Hub access.
#
#   ULTRAVIOLENT_IP27_PROM=/path/to/ip27prom.img scripts/ip27-prom-checkpoint.sh [cycles]
#
# Skips cleanly when no image is configured. Never commit the image.
set -eu

prom=${ULTRAVIOLENT_IP27_PROM:-}
cycles=${1:-2000000}
root=$(cd "$(dirname "$0")/.." && pwd)
binary=${ULTRAVIOLENT_BINARY:-$root/build/dev/ultraviolent}

if [ -z "$prom" ] || [ ! -r "$prom" ]; then
    echo "skip: set ULTRAVIOLENT_IP27_PROM to a readable IP27 PROM image"
    exit 0
fi
if [ ! -x "$binary" ]; then
    echo "error: $binary not built (cmake --build --preset dev)" >&2
    exit 1
fi

echo "image: $(sha256sum "$prom" | cut -d' ' -f1)"
trace=$("$binary" --machine ip27 --prom "$prom" --cycles "$cycles" --trace firmware,hub 2>&1)
echo "leds: $(printf '%s\n' "$trace" | awk '$2 == "firmware" && $3 == "led" {printf "%s ", $5}')"
echo "first unmodeled: $(printf '%s\n' "$trace" | awk '$2 == "hub" && $3 == "unmodeled" && $4 == "read" {print $5; exit}')"
printf '%s\n' "$trace" | tail -1
