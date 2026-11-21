#!/bin/bash
# Builds Local Connect's page: the tablet's own screens compiled with Emscripten into one self-contained HTML page, gzip-compressed
# as the plugin serves it (plugins/XPTablet/localconnect/index.html.gz).
#   EMCC=/path/to/emcc tools/build_localclient.sh <out-dir>
set -e
cd "$(dirname "$0")/.."
OUT=${1:-build-localclient}
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
SRCS="src/localclient/client.cpp src/localwire.cpp src/tabletui.cpp src/arcade.cpp src/acars.cpp src/tabrender.cpp src/metar.cpp src/aptgeo.cpp src/airlines.cpp src/tz.cpp src/airspace_parse.cpp
      third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp"
"$EMXX" -std=c++17 -O2 -Isrc -Ithird_party/imgui -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS $SRCS "$OUT/xpt_assets.o" "$OUT/xpt_shapes.o" \
  -sUSE_WEBGL2=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864 -sSINGLE_FILE=1 -sENVIRONMENT=web \
  -sEXPORTED_RUNTIME_METHODS=stringToNewUTF8,UTF8ToString \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_lc_open,_lc_closed,_lc_message,_lc_battery,_lc_mouse,_lc_wheel,_lc_typing,_lc_touch,_lc_physical_keys,_lc_key,_lc_char,_lc_scale,_lc_shape,_lc_tile,_lc_phase,_lc_connect \
  -o "$OUT/client.js"
python3 - "$OUT" <<'PY'
import gzip, sys
out = sys.argv[1]
shell = open('src/localclient/shell.html').read()
js = open(out + '/client.js').read()
page = shell.replace('{{{ SCRIPT }}}', '<script>\n' + js.replace('</script', '<\\/script') + '\n</script>').encode('utf-8')
open(out + '/index.html', 'wb').write(page)
with open(out + '/index.html.gz', 'wb') as f:
    with gzip.GzipFile(fileobj=f, mode='wb', compresslevel=9, mtime=0, filename='') as g:
        g.write(page)
print(out + '/index.html.gz', len(page), 'bytes as a page')
PY
