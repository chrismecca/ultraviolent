#!/bin/sh
# Profile-guided release build (DEVELOPMENT.adoc "Fast builds"). Builds an instrumented
# binary, trains it by running the user's IP27 PROM image from power-on, and builds
# build/pgo/ultraviolent with the profile and link-time optimization.
#
#   ULTRAVIOLENT_IP27_PROM=/path/to/ip27prom.img scripts/build-pgo.sh [training cycles]
#
# Guest behavior is identical to the release build; only host speed changes. The profile
# stays in build/, derived from a proprietary image, and is never committed.
set -eu

prom=${ULTRAVIOLENT_IP27_PROM:-}
cycles=${1:-600000000}
root=$(cd "$(dirname "$0")/.." && pwd)
profdata=$(command -v llvm-profdata || command -v llvm-profdata-19 || true)

if [ -z "$prom" ] || [ ! -r "$prom" ]; then
    echo "error: set ULTRAVIOLENT_IP27_PROM to a readable IP27 PROM image" >&2
    exit 1
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
LLVM_PROFILE_FILE=$generate/training.profraw \
    "$generate/ultraviolent" --machine ip27 --prom "$prom" --cycles "$cycles" >/dev/null
"$profdata" merge -o "$generate/ultraviolent.profdata" "$generate"/*.profraw

cmake -S "$root" -B "$root/build/pgo" -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    "-DCMAKE_CXX_FLAGS=-fprofile-instr-use=$generate/ultraviolent.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" \
    >/dev/null
# Objects built against an older profile cannot be linked with ones built against this one.
cmake --build "$root/build/pgo" --target clean
cmake --build "$root/build/pgo" --target ultraviolent
echo "built $root/build/pgo/ultraviolent"
