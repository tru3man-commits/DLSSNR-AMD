#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT
for platform in windows linux; do
    "${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror -I"$platform/src/core" tests/temporal_gate.cpp -o "$build/test"
    printf '%s: ' "$platform"
    "$build/test"
done
