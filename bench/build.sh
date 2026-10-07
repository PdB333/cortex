#!/bin/sh
# Build both benchmark targets: <out>/v1/bench_target.exe and <out>/v2/bench_target.exe.
# The executable has the same name in both so that anything keyed on the program
# name (a Cortex project, a notes file) carries over, as it would across a real update.
set -e
OUT="${1:-bench_build}"
CXX="${CXX:-x86_64-w64-mingw32-g++}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT/v1" "$OUT/v2"
FLAGS="-std=c++17 -O1 -static -s -Wl,--dynamicbase"
$CXX $FLAGS -o "$OUT/v1/bench_target.exe" "$HERE/bench_target/main.cpp" -lws2_32
$CXX $FLAGS -DBENCH_V2 -o "$OUT/v2/bench_target.exe" "$HERE/bench_target/main.cpp" -lws2_32
sha256sum "$OUT/v1/bench_target.exe" "$OUT/v2/bench_target.exe"
