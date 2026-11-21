# XPTablet

A cockpit tablet for X-Plane 12 by Alpine Aviation. Freeware.

XPTablet appears on the tablet of any aircraft that was built for AviTab, with no change to the aircraft: it reads the same
`AviTab.json` file in the aircraft folder, and provides the same `avitab/panel_*` datarefs and `AviTab/*` commands. Aircraft without
a tablet get a floating window (Plugins > XPTablet, or the command `XPTablet/toggle_tablet`), which is also a VR window.

XPTablet and AviTab cannot both own the cockpit tablet. If AviTab is installed, XPTablet leaves the cockpit tablet to it and runs
in its window only; remove AviTab from `Resources/plugins` to let XPTablet take its place.

## Status

Version 0.4.1 is a test build: the tablet in the AviTab slot with touch input, the home screen with rearrangeable icons and a
favorites bar, Settings, and the pages web apps will load into. The browser engine and the other apps are not in yet. See
`docs/PLAN.md` for what is planned.

## Install

Copy the `XPTablet` folder into `X-Plane 12/Resources/plugins/`.

## Local Connect

The tablet on a phone or iPad on the same Wi-Fi: Settings, Local Connect, switch it on, and open the address it shows in the
device's browser. The browser warns once that the connection is not private, because the certificate is one your own computer
made (choose Advanced or Show Details, then continue). The device then asks for the 6-digit code XPTablet shows in the sim; after
that it comes straight in each time. What the device shows is the same tablet: the same pages, settings, notes and flight.

How it is kept safe: off until switched on; it listens only on the computer's home-network address and answers only private
addresses; everything is encrypted (TLS 1.2, Mbed TLS); a device must be paired with the code shown in the sim (five wrong tries
and that address waits a minute); the computer keeps only the hash of each device's token, and Forget removes a device; a paired
device can only do what the tablet's own pages do (it cannot manage pairing, drive the computer's browser or delete its data).

A device shows only what works on a device. The pages that are pictures made by the computer are left out there altogether (no
icon, no App Store entry, no button): the web apps and the Browser, Documents, and the briefing PDF; a Custom theme's picture is
chosen on the computer. Each device arranges its own home screen (the first time it takes the computer's, closed up); every other
setting is the same everywhere. Devices fetch their own map tiles, so they need the internet for the map.

Source: `src/localserver.cpp` (the server), `src/localwire.cpp` (the messages), `src/localbridge.cpp` (the plugin's side),
`src/localclient/` (the page, built with Emscripten by `tools/build_localclient.sh` into `localconnect/index.html.gz`). Tests:
`ctest` (the messages; the server over real TLS) and `tests/e2e_localconnect.mjs` (a real browser pairing with and using it).

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

## License

XPTablet is freeware, copyright (c) 2026 Alpine Aviation: free to download and use, with its source visible here. Selling it,
re-uploading it elsewhere or publishing changed versions needs written permission. The full terms are in `LICENSE.txt`.

Third-party notices: `LICENSES.txt` (those parts keep their own licenses).
