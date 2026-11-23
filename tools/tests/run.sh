#!/usr/bin/env bash
# Host unit tests (Linux/macOS, g++). Needs ESP-IDF only for cJSON: IDF_PATH=... tools/tests/run.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build_tests}
CJSON=${IDF_PATH:?set IDF_PATH (ESP-IDF provides cJSON)}/components/json/cJSON
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
APP=$ROOT/main/apps/app_roast
g++ -std=gnu++17 -O1 -Wall -I"$ROOT/main" -I"$ROOT/tools/preview/stubs" -I"$CJSON" \
    "$ROOT/tools/tests/test_control.cpp" "$APP/roast_control.cpp" "$APP/roast_curve.cpp" "$CJSON/cJSON.c" \
    -o "$OUT/test_control"
"$OUT/test_control"
g++ -std=gnu++17 -O1 -Wall -I"$APP" -I"$ROOT/.deps/rapidjson/include" \
    "$ROOT/tools/tests/test_dropbox_page.cpp" "$APP/net/dropbox_page.cpp" -o "$OUT/test_dropbox_page"
"$OUT/test_dropbox_page"
g++ -std=gnu++17 -O1 -Wall -DROAST_SD_ROOT='"sd"' -I"$ROOT/tools/preview/stubs" -I"$ROOT/main" -I"$CJSON" \
    "$ROOT/tools/tests/test_sd_history.cpp" "$APP/roast_session.cpp" "$APP/roast_curve.cpp" "$CJSON/cJSON.c" \
    -o "$OUT/test_sd_history"
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
(cd "$TEST_DIR" && "$OUT/test_sd_history")
