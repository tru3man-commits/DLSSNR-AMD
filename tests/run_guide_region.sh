#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
for platform in linux windows; do
    "${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror -I"$platform/src/core" tests/guide_region.cpp -o "$build/test"
    printf '%s: ' "$platform"
    "$build/test"
done
