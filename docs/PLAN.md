# XPTablet plan

Decisions: freeware, an Alpine Aviation product, public GitHub repo, written from scratch (AviTab's source is AGPL and is not used;
only its public interface - dataref names, command names, the AviTab.json layout - is matched). Every feature below is in the first
public version; test builds are handed over phase by phase.

## Phases

1. Proof of drop-in: the tablet in the AviTab slot, touch working. (0.1.0)
2. Browser prototype (Chromium embedded; X-Plane ships its own copy, so this is the risky part and comes early).
3. Shell: home screen, wallpaper from the user's own image files, themes, settings.
4. Core apps: map, airports, charts, documents, routes.
5. Weather and ground services.
6. SimBrief and the extras, then beta through SkunkCrafts.

## Features

Core: cockpit tablet on the aircraft's panel texture, floating window, VR window; touch, mouse wheel, VR controller; brightness and
power following the aircraft; moving map (online and offline tiles); airport information from X-Plane's nav data; PDF and image
charts with per-aircraft document folders; routes from flight plan files; notes; settings.

Modern UI: home screen with app icons and a status bar; smooth pan and zoom at the panel's own resolution; dark and light themes
following the cockpit lighting; split view; search across airports, charts and documents; on-screen keyboard that works in VR;
wallpaper chosen from the user's files.

Weather: WXPlane's map and data on a tablet page when WXPlane is installed (needs a small hook in WXPlane); a built-in radar and
METAR map when it is not.

Browser: persistent profile so sites stay logged in (cookies and sessions, no passwords stored by XPTablet); bookmarks with a starter
set (ChartFox, Volanta, VATSIM map, SimBrief) that users can add to, edit and reorder.

Ground services: aircraft icon by ICAO type; jetway button (OpenSAM when present, X-Plane's own jetways otherwise); for ToLiss
aircraft a page with every passenger and cargo door, the service vehicles and the jetways; generic controls for other aircraft.

Flight tools: SimBrief import (OFP, route, fuel, weights); geo-referenced charts with the aircraft drawn on them; vertical profile;
interactive checklists; performance (take-off and landing distance, weight and balance, crosswind); timers, landing rate;
live VATSIM/IVAO traffic; scratchpad; quick sim controls.

Later ideas: phone or iPad companion over the local network; an app API for aircraft developers; Navigraph charts (needs their
approval).

## Home screen (decided 2026-11-17)

Apps: Map, Airports, Charts, Documents, Routes, Weather, Ground, Checklists, Performance, Traffic, Notes, SimBrief, Live Map,
ForeFlight, Browser, Settings. No logbook.

- The Charts icon is ChartFox or Navigraph, and the Live Map icon is Volanta or VATSIM Map; each is chosen in Settings and the
  icon carries the chosen name.
- Charts, Live Map, SimBrief and ForeFlight can be switched off in Settings; with Charts or Live Map off, its source choice is
  greyed out.
- Web apps open as pages of their own (title bar only). The Browser icon opens the full browser with address bar and bookmarks.
- Icons are rearranged by pressing and moving them. A favorites bar along the bottom takes icons dragged into it; its size is a
  slider in Settings under the brightness, 0 to 5 places (4 by default; 0 removes the bar).
- In ToLiss aircraft the Performance icon is left out: their own tablet does the performance calculations.
- The status bar shows the tablet's battery: it charges while an aircraft bus is live and runs down otherwise.
- The arrangement and the choices are kept in Output/preferences/XPTablet.prf.

## Weather page and WXPlane

With WXPlane installed, the Weather page is WXPlane's own screen: XPTablet draws the title bar and hands the rest of the page to
WXPlane, which draws itself there (no Pop out tab on the tablet). The two plugins talk through one message, described in
`src/wxplane_embed.h` (the same file is `src/embed.h` in WXPlane): XPTablet sends it from inside its drawing with the page's area
and the pointer, WXPlane draws and answers. A WXPlane too old to know the message leaves it unanswered and the page says so.
Without WXPlane the page will be a built-in radar and METAR map (not built yet).

## Built-in Weather page (no WXPlane)

"At the aircraft": X-Plane's own weather where the aircraft is (wind, visibility, temperature, QNH, rain, cloud layers, winds aloft),
no internet needed. "METAR search": an ICAO code typed on the page's own keys, the latest report from NOAA's Aviation Weather Center
(aviationweather.gov) and what it says. A radar map is still to come.

## Ground page

The aircraft is drawn from AircraftShapesSVG's top views (RexKramer1 and amnesica, GPL-3.0; source in third_party/, converted by
tools/make_shapes.py into assets/shapes/aircraft_shapes.txt, shipped as XPTablet/shapes/ and read at run time, credited in
Settings > Credits). Each is resized to its type's published length and span; types without a drawing use their family's,
resized (ALIASES in make_shapes.py: A319, B736, MD8x, E175/E190, Challengers, Globals, Learjets, Falcons, ...). Types with
neither are drawn from their measurements (a table in tabletui.cpp). Jetway: openSAM when installed (openSAM/toggle_jwy, opensam/jetway/status), else X-Plane's
(sim/ground_ops/jetway). Ground power, service vehicles and pushback: X-Plane's commands.

ToLiss (names read from the A320's plugin): cabin doors AirbusFBW/PaxDoorModeArray and cargo doors AirbusFBW/CargoDoorModeArray
(0 closed, 1 automatic, 2 open: to confirm in the sim, with the door order), all doors toliss_airbus/door_commands/all_to_mode_*,
chocks AirbusFBW/Chocks, ground power AirbusFBW/EnableExternalPower, ground air AirbusFBW/GroundHPAir and GroundLPAir, de-icing
toliss_airbus/iscsinterface/animatedDeice and quickDeice. The stairs, catering trucks and baggage loaders have no dataref or command:
the ToLiss shows them itself with the doors, as chosen on its own EFB (only their animation values, toliss/anim/..., are published).
XPTablet writes what it finds to Log.txt once per ToLiss aircraft ("[XPTablet] ToLiss: ..."). Confirmed in the A320 (2026-11-17):
PaxDoorModeArray has 4 values, CargoDoorModeArray 3 (forward, aft, bulk), both writable; the door and de-ice names are commands.

## Browser engine

X-Plane 12 ships Chromium Embedded Framework itself (Resources/dlls/64/cef; 12.4.4 has CEF 117.1.4+ga26f38b, Chromium 117.0.5938.92).
A process can hold only one copy of it, so XPTablet uses X-Plane's copy rather than bringing its own: build against the matching CEF
headers, render off-screen into the tablet's texture, keep cookies and sessions in a profile folder under Output so sites stay
logged in. To check when wiring it up: whether X-Plane has already started CEF in its own process, and the helper process each
platform needs.

## Known gaps in 0.1.0

- Touch on the cockpit tablet in VR (controller trigger) is not handled yet; the VR window works.
- No keyboard input yet.
- Aircraft that look for AviTab's plugin signature rather than its datarefs may not show the tablet; to be found by testing.
