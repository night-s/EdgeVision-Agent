#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
cmake -S . -B build-refactor -DCMAKE_BUILD_TYPE=Release
cmake --build build-refactor -j2
# CTest bundled with this BSP does not support --test-dir.
(cd build-refactor && ctest --output-on-failure)
exec ./build-refactor/edge_agent "${1:-configs/default.json}"
