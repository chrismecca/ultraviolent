#!/bin/sh
# Boots an installed machine (doc/USING.adoc "Boot and use it"): the PROM images from
# assets/firmware (or $ULTRAVIOLENT_IP27_PROM and the io6prom.img beside it), the disk, NVRAM,
# and flash files from DIR, in real time, with the console on this terminal. Any further
# options go to ultraviolent.
#
#   scripts/boot.sh local/vm
#   scripts/boot.sh local/vm --cpus 2 --parallel --ethernet tap:uv0
#
# Ctrl-] then off (or SIGPWR to this process) turns the keyswitch off: IRIX shuts down and
# the machine powers off. Ctrl-] then q stops the machine at once.
set -eu

if [ $# -lt 1 ]; then
    echo "usage: scripts/boot.sh DIR [ultraviolent options...]" >&2
    exit 2
fi
dir=$1
shift
root=$(cd "$(dirname "$0")/.." && pwd)
prom=${ULTRAVIOLENT_IP27_PROM:-$root/assets/firmware/ip27prom.img}
io6prom=$(dirname "$prom")/io6prom.img
binary=$root/build/pgo/ultraviolent
[ -x "$binary" ] || binary=$root/build/release/ultraviolent

missing=
for file in "$binary" "$prom" "$io6prom" "$dir/disk.img" "$dir/nvram.bin" "$dir/flash.bin"; do
    [ -e "$file" ] || missing="$missing
  $file"
done
if [ -n "$missing" ]; then
    echo "missing:$missing" >&2
    exit 1
fi

exec "$binary" --machine ip27 --prom "$prom" --io6prom "$io6prom" \
    --disk "$dir/disk.img" --nvram "$dir/nvram.bin" --flash "$dir/flash.bin" \
    --realtime --interactive --cycles 100000000000000 "$@"
