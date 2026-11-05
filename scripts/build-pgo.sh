#!/bin/sh
# Profile-guided release build (DEVELOPMENT.adoc "Fast builds"). Builds an instrumented
# binary, trains it by running the user's IP27 PROM image from power-on once per execution
# engine, and builds build/pgo/ultraviolent with the profile and link-time optimization.
#
#   ULTRAVIOLENT_IP27_PROM=/path/to/ip27prom.img scripts/build-pgo.sh [training cycles]
#
# With ULTRAVIOLENT_PGO_MACHINE=DIR, a machine with IRIX installed (disk.img, nvram.bin,
# flash.bin, as scripts/rc-ip27-demo.py leaves them), the training also cold-boots a copy of
# it to login: (the PROM's io6prom.img beside the IP27 PROM image), so that IRIX's code paths
# are in the profile too. DIR is only read.
#
# Guest behavior is identical to the release build; only host speed changes. The profile
# stays in build/, derived from a proprietary image, and is never committed.
set -eu

prom=${ULTRAVIOLENT_IP27_PROM:-}
machine=${ULTRAVIOLENT_PGO_MACHINE:-}
cycles=${1:-600000000}
root=$(cd "$(dirname "$0")/.." && pwd)
profdata=$(command -v llvm-profdata || command -v llvm-profdata-19 || true)

if [ -z "$prom" ] || [ ! -r "$prom" ]; then
    echo "error: set ULTRAVIOLENT_IP27_PROM to a readable IP27 PROM image" >&2
    exit 1
fi
if [ -n "$machine" ]; then
    for file in "$machine/disk.img" "$machine/nvram.bin" "$machine/flash.bin" \
        "$(dirname "$prom")/io6prom.img"; do
        if [ ! -r "$file" ]; then
            echo "error: ULTRAVIOLENT_PGO_MACHINE: cannot read $file" >&2
            exit 1
        fi
    done
fi
if [ -z "$profdata" ]; then
    echo "error: llvm-profdata not found" >&2
    exit 1
fi

generate=$root/build/pgo-generate
rm -f "$generate"/*.profraw
cmake -S "$root" -B "$generate" -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_CXX_FLAGS=-fprofile-instr-generate \
    -DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate >/dev/null
cmake --build "$generate" --target ultraviolent
# Train every execution engine, so none is laid out without a profile.
for engine in reference tier0 tier1; do
    LLVM_PROFILE_FILE=$generate/training-$engine.profraw \
        "$generate/ultraviolent" --machine ip27 --prom "$prom" --cycles "$cycles" \
        --engine "$engine" >/dev/null
done
if [ -n "$machine" ]; then
    # IRIX's boot, on copies. The PROM stops at its menu (Option?) unless AutoLoad is set;
    # "1" starts the system there, or answers login: when it came first, and the run stops at
    # the next login: (or Password:). Measured 2026-11-05: boot and fs 7-11% faster.
    copy=$(mktemp -d)
    trap 'rm -rf "$copy"' EXIT
    for file in disk.img nvram.bin flash.bin; do
        cp --sparse=always "$machine/$file" "$copy/$file"
    done
    LLVM_PROFILE_FILE=$generate/training-irix.profraw \
        "$generate/ultraviolent" --machine ip27 --prom "$prom" \
        --io6prom "$(dirname "$prom")/io6prom.img" --disk "$copy/disk.img" \
        --nvram "$copy/nvram.bin" --flash "$copy/flash.bin" --cycles 60000000000 \
        --console '@expect Option?|login:' --console 1 --console '@expect login:|Password:' \
        --console @stop >/dev/null
fi
"$profdata" merge -o "$generate/ultraviolent.profdata" "$generate"/*.profraw

cmake -S "$root" -B "$root/build/pgo" -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    "-DCMAKE_CXX_FLAGS=-fprofile-instr-use=$generate/ultraviolent.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" \
    >/dev/null
# Objects built against an older profile cannot be linked with ones built against this one.
cmake --build "$root/build/pgo" --target clean
cmake --build "$root/build/pgo" --target ultraviolent
echo "built $root/build/pgo/ultraviolent"
