#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
# Compile the production methods against a fake Vulkan queue. No copied state
# machine: an optional source root lets the very same tests exercise an old ref.
python3 tests/extract_bridge_submit.py "${1:-.}" "$build"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -pthread -I"$build" tests/bridge_submit.cpp -o "$build/test"
timeout 20s "$build/test"
