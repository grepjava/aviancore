#!/usr/bin/env bash
# The broadcast bus across real processes, compiled against avian_bus.c:
# writers racing while a reader checks every byte, writers killed part-way
# through a message, and wakes from one process to another.
#
#   bash scripts/bus-unit-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

${CC:-cc} -std=c11 -O1 -g -Wall -Wextra -Werror \
    -I "$ROOT/Sources/CAvian/include" \
    "$ROOT/Sources/CAvian/avian_bus.c" "$ROOT/scripts/bus_unit.c" \
    -o "$OUT/bus-unit"
"$OUT/bus-unit"
