#!/usr/bin/env bash
# Builds and runs pid_compare (host, g++). Needs ESP-IDF only for cJSON: IDF_PATH=... tools/pid/run.sh [logs dir]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build_pid}
CJSON=${IDF_PATH:?set IDF_PATH (ESP-IDF provides cJSON)}/components/json/cJSON
mkdir -p "$OUT"
APP=$ROOT/main/apps/app_roast
g++ -std=gnu++17 -O1 -Wall -I"$ROOT/main" -I"$ROOT/tools/preview/stubs" -I"$CJSON" \
    "$ROOT/tools/pid/pid_compare.cpp" "$APP/roast_control.cpp" "$APP/roast_curve.cpp" "$CJSON/cJSON.c" \
    -o "$OUT/pid_compare"
g++ -std=gnu++17 -O1 -Wall -I"$ROOT/main" -I"$ROOT/tools/preview/stubs" -I"$CJSON" \
    "$ROOT/tools/pid/fc_crack.cpp" "$APP/roast_control.cpp" "$APP/roast_curve.cpp" "$CJSON/cJSON.c" \
    -o "$OUT/fc_crack"
OUT=$OUT "$OUT/pid_compare" "$@"
echo
OUT=$OUT "$OUT/fc_crack" "$ROOT/tools/pid"
