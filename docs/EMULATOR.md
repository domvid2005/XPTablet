# XPTablet Emulator

Try XPTablet without X-Plane. The emulator runs the real plugin code against a stand-in for X-Plane: its datarefs, commands, windows,
menus and textures, plus a simulated aircraft you control from a panel. It draws exactly as the plugin does in the sim.

## Running it

Start **XPTablet Emulator** (Windows: `XPTablet Emulator.exe`). Keep the `XPTablet` folder next to it: it holds the aircraft shapes.

- The left side is the X-Plane screen with the tablet's window. Drag its title bar to move it and its bottom-right corner to resize it.
- The right side is the control panel:
  - **Aircraft:** pick the aircraft (Cessna, the default A330 and 737, ToLiss A320/A321/A346, CL650), its position (airport buttons),
    heading, height, speed, battery, beacon, parking brake and chocks. You can also turn on the cockpit tablet (writes an AviTab.json)
    and show it on the cockpit panel, and set which other plugins are installed (WXPlane, OpenSAM, AviTab).
  - **Traffic:** moving TCAS traffic around you, and a button for a TCAS RA intruder.
  - **World:** the time, the sun and the weather at the aircraft.
  - **Datarefs:** every dataref the plugin uses, editable. The ones it looked for that this X-Plane or aircraft doesn't have are listed underneath.
  - **Commands:** the plugin's menu and commands, and the commands it sent (jetway, GPU, pushback).
  - **Log:** what would be in X-Plane's Log.txt.
- **Reload plugin** stops and starts it, as X-Plane's plugin reload does. **Screenshot** saves a picture.

### Your X-Plane's airports and navaids

Point the emulator at your X-Plane 12 folder once and it remembers:

    "XPTablet Emulator" --xplane "C:\X-Plane 12"

The plugin then reads your scenery's apt.dat and your navdata, as it does in the sim. Like the plugin, it keeps its map-tile cache in
`Output/XPTablet` in that folder. Without `--xplane`, it uses an empty folder of its own, so the map has no airports or navaids.

### macOS

The emulator isn't signed. The first time, right-click it and choose Open.

## Scripts (automatic testing)

    "XPTablet Emulator" --script steps.txt --out pictures
    xptablet_emu_headless --script steps.txt --out pictures      (Linux, no screen needed; add --panel to include the control panel)

A script is a list of steps: `tap 400 300` (tablet layout units, 800 x 480), `wait 1.5`, `type KORD`, `key return`,
`command XPTablet/app_maps`, `sim gs 250`, `intruder`, `view cockpit`, `reload` and `shot name`. The full list is in
`emulator/emu_script.cpp`, and `emulator/tests/smoke.txt` is an example.

## What it doesn't do

- **Scenery:** there is no 3D world, just a backdrop.
- **WXPlane, AviTab and OpenSAM:** these are stand-ins. The plugin finds them, but they don't draw or answer.
- **Web pages:** the built-in browser runs only when the emulator points at a real X-Plane folder and the XPTablet folder holds the
  browser helper. The browser helper is built for Windows and Linux.
- **Your X-Plane:** the emulator never changes anything in your X-Plane except the tile cache.

## Building

    cmake -S . -B build -DXPLANE_SDK=<SDK folder> -DXPT_BUILD_EMULATOR=ON [-DXPT_BUILD_EMULATOR_HEADLESS=ON]
    cmake --build build --target xptablet_emulator

GLFW is downloaded during the build. On Linux you also need the X11 development packages (libx11-dev, libxrandr-dev,
libxinerama-dev, libxcursor-dev, libxi-dev) and libgl-dev.
