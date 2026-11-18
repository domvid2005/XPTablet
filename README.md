# XPTablet

A cockpit tablet for X-Plane 12 by Alpine Aviation. Freeware.

XPTablet appears on the tablet of any aircraft that was built for AviTab, with no change to the aircraft: it reads the same
`AviTab.json` file in the aircraft folder, and provides the same `avitab/panel_*` datarefs and `AviTab/*` commands. Aircraft without
a tablet get a floating window (Plugins > XPTablet, or the command `XPTablet/toggle_tablet`), which is also a VR window.

XPTablet and AviTab cannot both own the cockpit tablet. If AviTab is installed, XPTablet leaves the cockpit tablet to it and runs
in its window only; remove AviTab from `Resources/plugins` to let XPTablet take its place.

## Status

Version 0.4.0 is a test build: the tablet in the AviTab slot with touch input, the home screen with rearrangeable icons and a
favorites bar, Settings, and the pages web apps will load into. The browser engine and the other apps are not in yet. See
`docs/PLAN.md` for what is planned.

## Install

Copy the `XPTablet` folder into `X-Plane 12/Resources/plugins/`.

## Build

    cmake -S . -B build -DXPLANE_SDK=/path/to/XPSDK
    cmake --build build --config Release
    ctest --test-dir build -C Release

The plugin lands in `build/package/XPTablet/`. `-DXPT_BUILD_PLUGIN=OFF` builds only the tablet's screens and their off-line test
(`ui_shot`, which also writes PNG pictures of the screens into the build folder).

## Layout

- `src/plugin.cpp` - the X-Plane side: cockpit tablet, floating window, datarefs, commands
- `src/tabletui.cpp` - the tablet's screens (Dear ImGui, no X-Plane calls)
- `src/tabrender.cpp` - turns ImGui's output into clipped triangles
- `tests/ui_shot.cpp` - draws every screen off-line
- `tools/make_logo.py` - draws `assets/xptablet_logo.png`

Third-party notices: `LICENSES.txt`.
