#!/usr/bin/env bash
# Headless simulator sweep of the curve-follow settings (replays batch A of 2026-11-12). Needs the preview objects:
#   tools/preview/build.sh && tools/preview/build_sweep.sh && build_preview/fc_sweep
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build_preview}
SRC=$ROOT/components/M5GFX/src
CJSON=${IDF_PATH:?set IDF_PATH}/components/json/cJSON
APP=$ROOT/main/apps/app_roast
g++ -std=gnu++17 -O1 -DLGFX_LINUX_FB -I"$ROOT/tools/preview/stubs" -I"$ROOT/main" -I"$ROOT/main/hal/keyboard" \
    -I"$ROOT/components/mooncake_log/src" -I"$SRC" -I"$CJSON" \
    "$ROOT/tools/preview/fc_sweep.cpp" "$APP/roast_link_sim.cpp" "$APP/roast_control.cpp" "$APP/roast_curve.cpp" \
    "$SRC/lgfx/v1/LGFXBase.cpp" "$SRC/lgfx/v1/LGFX_Sprite.cpp" "$SRC/lgfx/v1/lgfx_fonts.cpp" "$SRC"/lgfx/v1/misc/*.cpp \
    "$SRC/lgfx/v1/platforms/framebuffer/common.cpp" "$SRC/lgfx/v1/panel/Panel_Device.cpp" "$OUT"/obj/*.o -o "$OUT/fc_sweep"
echo "built $OUT/fc_sweep"
