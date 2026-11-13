#!/usr/bin/env bash
# Build the desktop preview of the Roast UI (Linux, g++). Uses the project's M5GFX checkout (run fetch_repos.py
# first) and cJSON from ESP-IDF ($IDF_PATH).
#   tools/preview/build.sh            -> build_preview/preview
#   build_preview/preview build_preview/shots
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build_preview}
SRC=$ROOT/components/M5GFX/src
CJSON=${IDF_PATH:?set IDF_PATH (ESP-IDF provides cJSON)}/components/json/cJSON
mkdir -p "$OUT/obj"

# C sources rarely change: compile once
for f in "$SRC"/lgfx/utility/*.c $(find "$SRC/lgfx/Fonts" -name '*.c') "$CJSON/cJSON.c"; do
    o="$OUT/obj/$(basename "$f" .c).o"
    [ -f "$o" ] || gcc -O1 -c -I"$SRC" -I"$CJSON" "$f" -o "$o"
done

APP=$ROOT/main/apps/app_roast
g++ -std=gnu++17 -O1 -DLGFX_LINUX_FB -DROAST_SD_ROOT="\"$OUT/sd\"" \
    -I"$ROOT/tools/preview/stubs" -I"$ROOT/main" -I"$ROOT/main/hal/keyboard" \
    -I"$ROOT/components/mooncake_log/src" -I"$SRC" -I"$CJSON" \
    "$ROOT/tools/preview/preview.cpp" \
    "$APP/app_roast.cpp" "$APP/app_roast_ui.cpp" "$APP/roast_curve.cpp" "$APP/roast_session.cpp" \
    "$APP/roast_link_sim.cpp" "$APP/roast_control.cpp" "$APP/fonts/roast_fonts.cpp" \
    "$SRC/lgfx/v1/LGFXBase.cpp" "$SRC/lgfx/v1/LGFX_Sprite.cpp" "$SRC/lgfx/v1/lgfx_fonts.cpp" \
    "$SRC"/lgfx/v1/misc/*.cpp "$SRC/lgfx/v1/platforms/framebuffer/common.cpp" "$SRC/lgfx/v1/panel/Panel_Device.cpp" \
    "$OUT"/obj/*.o -o "$OUT/preview"
echo "built $OUT/preview"
