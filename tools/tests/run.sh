#!/usr/bin/env bash
# Host unit tests (Linux/macOS, g++). Needs ESP-IDF only for cJSON: IDF_PATH=... tools/tests/run.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build_tests}
CJSON=${IDF_PATH:?set IDF_PATH (ESP-IDF provides cJSON)}/components/json/cJSON
mkdir -p "$OUT"
APP=$ROOT/main/apps/app_roast
g++ -std=gnu++17 -O1 -Wall -I"$ROOT/main" -I"$ROOT/tools/preview/stubs" -I"$CJSON" \
    "$ROOT/tools/tests/test_control.cpp" "$APP/roast_control.cpp" "$APP/roast_curve.cpp" "$CJSON/cJSON.c" \
    -o "$OUT/test_control"
"$OUT/test_control"
