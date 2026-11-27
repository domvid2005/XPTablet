# XPTablet

A cockpit tablet for X-Plane 12 by Alpine Aviation. Freeware.

XPTablet appears on the tablet of any aircraft that was built for AviTab, with no change to the aircraft: it reads the same
`AviTab.json` file in the aircraft folder, and provides the same `avitab/panel_*` datarefs and `AviTab/*` commands. Aircraft without
a tablet get a floating window (Plugins > XPTablet, or the command `XPTablet/toggle_tablet`), which is also a VR window.

XPTablet and AviTab cannot both own the cockpit tablet. If AviTab is installed, XPTablet leaves the cockpit tablet to it and runs
in its window only; remove AviTab from `Resources/plugins` to let XPTablet take its place.

## Status

Version 1.0.2. In it: the tablet in AviTab's place on aircraft built for AviTab (and in a floating window
on the others), the home screen and App Store, the moving map with VATSIM traffic and airspaces, airports, charts and documents,
SimBrief flight plans and the Dispatch briefing, weather (WXPlane's own page when it is installed), checklists, notes, a logbook,
ground services and pushback, the browser and web apps, media controls, and Local Connect.

With AviTab not installed, XPTablet answers to AviTab's plugin signature as well, so aircraft that look for AviTab by it (the
Aerobask DA42) switch between their own tablet pages and this one.

## Install

Download `XPTablet` from the [releases page](https://github.com/domvid2005/XPTablet/releases), or install and update it with the
SkunkCrafts Updater. Copy the `XPTablet` folder into `X-Plane 12/Resources/plugins/`.

The user manual (how to use each app, with pictures) is `assets/docs/XPTablet_User_Manual.pdf`; it ships in the plugin's `docs`
folder and opens in the tablet's Documents app. The forum description is `docs/forum-description.md`.

## Local Connect

The tablet on a phone or iPad on the same Wi-Fi, as a mirror of the one in the sim: Settings, Local Connect, switch it on, and
open the address it shows in the device's browser. The browser warns once that the connection is not private, because the
certificate is one your own computer made (choose Advanced or Show Details, then continue). The device then asks for the 6-digit
code XPTablet shows in the sim; after that it comes straight in each time.

The device shows the picture of the tablet in the sim and works it by touch: the same page, everything on it (web apps,
documents, WXPlane's page included), whether or not the tablet is on screen in the sim. What is opened on the device is opened
on the cockpit's tablet, and the other way round: it is one tablet.

How it is kept safe: off until switched on; it listens only on the computer's home-network address and answers only private
addresses; everything is encrypted (TLS 1.2, Mbed TLS); a device must be paired with the code shown in the sim (five wrong tries
and that address waits a minute); the computer keeps only the hash of each device's token, and Forget removes a device. A paired
device can do whatever the tablet can, so pair only devices you would hand the tablet to.

What it costs: while a device is connected, the tablet is drawn once more into a texture and read back when its picture changes
(15 times a second at most, about 1200 x 720); compressing it is done on another thread.

Source: `src/localserver.cpp` (the server and pairing), `src/localmirror.cpp` (the picture and the touches), `src/localpage.h`
(the page the device opens), and in `src/plugin.cpp` the picture being taken (OpenGL) and the touches fed to the tablet. Tests:
`ctest` (the server over real TLS) and `tests/e2e_localconnect.mjs` (a real browser pairing, shown the picture, working the
tablet by touch, against `tests/local_host.cpp`, a stand-in for the plugin that draws the picture without a graphics card).

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
