#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${root}/build"
if [[ ! -f "${build_dir}/build.ninja" && ! -f "${build_dir}/Makefile" ]]; then
  cmake -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${build_dir}" -j"$(nproc)"
