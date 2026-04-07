#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ROWS="${1:-1000000}"
CLIENTS="${2:-4}"

cd "$ROOT"

echo "[bench] Running FlexQL unit benchmark checks..."
./bin/flexql-benchmark --unit-test

echo "[bench] Running FlexQL benchmark with ${ROWS} rows across ${CLIENTS} clients..."
./bin/flexql-benchmark "$ROWS" "$CLIENTS"
