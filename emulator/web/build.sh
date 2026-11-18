#!/bin/bash
# Builds XPTablet's browser emulator: one self-contained HTML page (the tablet's real screens compiled with Emscripten, WebGL2).
#   EMCC=/path/to/emcc emulator/web/build.sh <out-dir>
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-build-web}
EMCC=${EMCC:-emcc}
EMXX=${EMCC%cc}++
mkdir -p "$OUT"
# the fonts, logo, icons, time zones and outlines (as the plugin embeds them), and the aircraft shapes
cmake -DSRC="$PWD" -DOUT="$OUT/xpt_assets.c" -P cmake/embed_assets.cmake > /dev/null
python3 - "$OUT/xpt_shapes.c" assets/shapes/aircraft_shapes.txt <<'PY'
import sys
data = open(sys.argv[2], 'rb').read()
with open(sys.argv[1], 'w') as f:
    f.write('const unsigned char xpt_web_shapes[] = {' + ','.join(str(b) for b in data) + '};\n')
    f.write('const unsigned int xpt_web_shapes_size = %d;\n' % len(data))
PY
"$EMCC" -c -O2 "$OUT/xpt_assets.c" -o "$OUT/xpt_assets.o"
"$EMCC" -c -O2 "$OUT/xpt_shapes.c" -o "$OUT/xpt_shapes.o"
SRCS="emulator/web/web.cpp src/tabletui.cpp src/tabrender.cpp src/metar.cpp src/aptgeo.cpp src/tz.cpp src/simbrief.cpp src/vatsim_parse.cpp src/json_mini.cpp
      third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp"
"$EMXX" -std=c++17 -O2 -Isrc -Ithird_party/imgui -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS $SRCS "$OUT/xpt_assets.o" "$OUT/xpt_shapes.o" \
  -sUSE_WEBGL2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sSINGLE_FILE=1 -sENVIRONMENT=web \
  -sEXPORTED_RUNTIME_METHODS=stringToNewUTF8,UTF8ToString --embed-file assets/checklists/airbus.txt@/checklists/airbus.txt \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_emu_mouse,_emu_wheel,_emu_typing,_emu_key,_emu_char,_emu_scale,_emu_width,_emu_shape,_emu_tile,_emu_text,_emu_aircraft,_emu_place,_emu_set,_emu_intruder,_emu_new_traffic,_emu_battery,_emu_reset_settings,_emu_alt,_emu_gs,_emu_on_ground \
  -o "$OUT/emu.js"
python3 - "$OUT" <<'PY'
import sys
out = sys.argv[1]
shell = open('emulator/web/shell.html').read()
js = open(out + '/emu.js').read()
open(out + '/index.html', 'w').write(shell.replace('{{{ SCRIPT }}}', '<script>\n' + js.replace('</script', '<\\/script') + '\n</script>'))
print(out + '/index.html')
PY
