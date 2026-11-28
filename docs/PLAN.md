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

### GSE, Cabin and XGSE (1.1.0)

- The Aircraft app is now **GSE** (its settings id stays "ground"): the aircraft with its door jetway bubbles takes two thirds of the
  screen, the jetway status sits in that card's top-right corner, and a Pushback card always fills the last third (BetterPushback:
  Plan Route, the tug, Start and Stop; X-Plane's tug: Left, Straight, Right, Stop).
- **Cabin** (an App Store app, category "Aircraft Management", a lightbulb icon): the ToLiss cabin lights (Auto, On, Off, the 10 %
  slider), the slides (Arm, Disarm, Auto) and Cabin Ready, moved out of the Aircraft page. Its icon and page are greyed outside a
  ToLiss. Installed once for everyone (a settings file without `cabin_offered` gets it), as its controls were on the Aircraft page.
- **XGSE** takes the GSE page over the way WXPlane takes Weather: src/xgse_embed.h, XGSE's own file copied as is (signature
  com.alpineaviation.xgse, message 0x01475345; accentRgb, skyRgb, the SimBrief airline and callsign; layout checked at compile time).
  While XGSE runs (installed and enabled) the tablet's jetway automation stands down; slides and cabin lights stay the Cabin app's. Detected at start and every 256 frames; the icon and title
  become XGSE; while XGSE does not answer (`handled` 0), XPTablet's own GSE page stays.
- The floating window keeps its place and size between sessions (settings key `window=x,y,w,h`, from the screen's bottom-left; kept
  on screen and shrunk to fit a smaller one).

### Performance (Settings, Performance; 1.1.0)

- **Tablet Frame Rate** 15 / 20 / 30 / 60 / Max (default 30; key `tablet_fps`, 0 Max): the tablet builds its picture at most that
  often, kept to the rate on average whatever X-Plane's own frame rate; between, the last picture is drawn again. Local Connect's
  devices get the same rate (its own Refresh Rate slider is gone; Max sends 60 a second).
- Web pages not on screen are paused: the helper is told (CMD_VISIBLE), Chromium stops drawing the shown tab (its sound plays on),
  no pictures are taken or copied to the graphics card, and hidden tabs give back their last picture's memory.
- **Close Unused Web Pages** Never / 5 / 15 / 30 Min (key `web_close_min`): Chromium quits once no page has been on screen that long
  and nothing plays; the Browser's own tabs open again when it next starts.
- **Reduced Memory Use** (key `low_memory`): 96 map tiles kept on the graphics card instead of 320; the map's navaids, fixes and
  airways (and its tile textures) put away after 5 minutes without the Map.
- Always: the navaids, fixes and airways are read when the Map is first opened (the runway alert needs only the airports index);
  navdata's stop() frees its memory; the dimmed picture is made once per picture, not every frame.

## Browser engine

X-Plane 12 ships Chromium Embedded Framework itself (Resources/dlls/64/cef; 12.4.4 has CEF 117.1.4+ga26f38b, Chromium 117.0.5938.92).
XPTablet uses X-Plane's copy, outside X-Plane's own process (0.5.0): the plugin starts xptbrowser (src/browser_helper.cpp), a small
program beside it that loads libcef from X-Plane's folder through the CEF C API (headers in third_party/cef, matching 117.1.4), and
renders windowless. They share one block of memory (src/browser_ipc.h): commands from the plugin in a ring (navigate, back, reload,
resize, pointer, wheel, keys), the page's pictures back in two BGRA buffers, and the address, title and loading state. The plugin
uploads the newest picture to a texture the tablet draws. Cookies, logins and cache live in Output/XPTablet/browser. The helper
quits by itself if the plugin stops sending heartbeats or X-Plane exits.

Typing: an on-screen keyboard on the tablet (up by itself when a text field on the page takes the focus, or from the keyboard
button); the floating window also takes the physical keyboard while a web page is on it.

Platforms: Windows (cef/win) is the first target. Linux looks in cef/lin, cef/linux, cef and dlls/64. macOS: X-Plane's CEF is a
framework inside its app bundle and is not used yet; the web pages say they are unavailable there.

Still to confirm in X-Plane: that X-Plane's libcef runs from a second program (sandbox off, its resources and locales folder),
and GPU-free rendering speed on big pages.

## Known gaps in 0.1.0

- Touch on the cockpit tablet in VR (controller trigger) is not handled yet; the VR window works.
- No keyboard input yet.
- Aircraft that look for AviTab's plugin signature rather than its datarefs may not show the tablet; to be found by testing.

## Map (0.6.0)

OpenStreetMap or Esri satellite tiles (src/tiles.cpp: two download threads, kept a month in Output/XPTablet/tiles, decoded with
stb_image; the plugin keeps up to 320 as textures). WXPlane's bubbles: TIME / LOCAL with the zone name (X-Plane's own clock near
the aircraft, else Natural Earth's zones plus daylight time; at the + when the map is moved), Center On Aircraft, North / Heading
Lock; zoom - / range / +, Map / Satellite, Airports, Time Zones. No true / magnetic switch.

Airports: src/aptdat.cpp indexes every apt.dat (custom scenery in scenery_packs.ini order, then Global Airports; the first copy of
an airport wins, as in X-Plane) on a thread the first time the map opens; layouts of the airports around the view are read on
demand (src/aptgeo.cpp: pavement with curves and holes, triangulated; runways with numbers; centrelines, hold lines, edges;
stands; taxiway names from the taxi routes). Dots with codes further out.

Browser tabs (0.6.0): up to 8; a page's new windows open as tabs; web apps (Charts, Live Map, SimBrief, planning) each keep a tab
of their own and come back to it as they were left. The planning icon switches between ForeFlight and SkyVector.

## 0.7.0: Airports, Documents, Notes, Checklists; Traffic and Outlines on the map

- Airports: search (code or name) or the nearest; X-Plane's apt.dat (the scenery you fly in: runways, radio, elevation, stands)
  merged with OurAirports (names, places, more frequencies). CTAF is always shown: the published CTAF, else UNICOM, else the
  tower's frequency (the CTAF when the tower is closed), else 122.900 MULTICOM. METAR from NOAA. Show on map.
- Documents: PDFs and PNG/JPG from Output/XPTablet/Documents, the aircraft's folder and AviTab's charts. Pictures in the tablet's
  own viewer (zoom, pan); PDFs in a browser tab (Chromium's PDF viewer, so extensions are no longer switched off).
- Notes: Clearance (CRAFT), ATIS and a scratchpad (pen in three colours, or typed); kept in Output/preferences/XPTablet_notes.txt.
  The on-screen keyboard and the floating window's keyboard type into any field.
- Checklists: the aircraft's own (Output/XPTablet/Checklists/<ICAO>.txt or xptablet_checklist.txt in its folder: "# Section",
  then "Item | VALUE"), else general airliner / light aircraft lists. Hidden on ToLiss aircraft.
- Routes removed (SimBrief); the Traffic app became the Map's Traffic layer (TCAS targets: AI, xPilot, LiveTraffic...).
- Map: Outlines layer (WXPlane's Natural Earth coastlines, borders, states and provinces).

- Performance removed for every aircraft (accurate figures need the manufacturer's certified data; ToLiss has its own).

## 0.8.0

- Status bar: the clock on the left (tap: Zulu or local time where the aircraft is, with its zone); the XPTablet logo in the
  middle (made at every size from the original, tools/make_logo_rgba.py).
- Map: Navaids (VOR with a compass rose to magnetic north, NDB, DME, ILS localizer feathers), Waypoints (fixes; terminal ones
  close in), Route (the user's latest SimBrief plan, by user name or pilot ID in Settings). Traffic coloured above / level /
  below. X-Plane's nav data: Custom Data first, as X-Plane does.
- AviTab compatibility: AviTab/zoom_in, zoom_out, recentre, pan_* drive the Map; avitab/map/latitude, longitude, zoom and
  vertical_range report it; AviTab/app_routes opens SimBrief; AviTab/app_plane_manual opens Documents on the aircraft's folder.
- Sharpness: every circle and arc is tessellated for the screen's pixel density (no fixed segment counts); concave shapes are
  filled whichever way their points run; map tiles, pictures and the logo are mipmapped; fonts sampled finer (3 x 2).
- Fix: Airports' Show on map now opens the Map.

## 0.9.0
- Settings in pages: General (what is on the home screen: chart providers ForeFlight / ChartFox / Navigraph / SkyVector, live maps Volanta /
  VATSIM, SimBrief; status; touch test), Display (brightness with Auto dim like WXPlane, favourites, clock, map), Data (SimBrief ID, import,
  data sources, clear map cache), and Credits at the bottom of the sidebar.
- Flight Plan page back: Import from SimBrief, Clear, Briefing PDF, Show on map; route, fuel and weights, navlog.
- iPad-like: pages open from their icon and close back to it; hold an icon ~0.75 s to arrange, free places shown faintly, icons go anywhere;
  labels under the dock icons; sliders ease; hover tints on everything tappable (no tooltips).
- Map: WXPlane's ranges (0.25 nm to 2500 nm) with smooth zoom; own aircraft and traffic drawn as their types' outlines; tap traffic,
  navaids (frequency, Morse), fixes, route points and airports (elevation, CTAF, frequencies, runways) for one card at a time; traffic
  cards show the VATSIM data (pilot, server, squawk, flight plan, route, remarks) from the public feed, read every 30 s while a card is open.
- AviTab commands: app_routes opens Flight Plan, app_navigraph opens Navigraph Charts.

## 0.9.1
- The VATSIM web map is now VATSIM Radar (vatsim-radar.com), with its own logo.
- New icons: Material Design Icons pictograms (Settings cog, Flight Plan four-point star, Checklists check, Documents file, Ground airplane, Airports airport, Browser globe) drawn sharp from their vector paths; ForeFlight, Navigraph, SkyVector,
  Volanta, VATSIM Radar and SimBrief tiles from their own logos. One mipmapped atlas (assets/icons.rgba, tools/make_icons_rgba.py). Credited in Credits.
- Map cards stay clear of the map's buttons (layers, time, locks, zoom bar); one at a time, with its ×.
- Settings Display no longer repeats the map's options (they are on the map).
- Map labels (traffic, navaids, ILS, waypoints, route, airports) placed so none overlap: each tries 8 places around its item, most
  important first (traffic, route, navaids, airports, waypoints); with no free place it is left off (the item can still be tapped).
- Under 2 nm with Airports on: the whole layout (taxiway edges and names, stands); stand names from 0.5 nm. Taxiway names never overlap.
- TCAS: traffic in resolution-advisory range (TCAS II v7.1 tau / DMOD / ZTHR for our height; none below 1000 ft AGL) or about to
  collide (0.15 nm, 300 ft) pulses red with a spreading ring and a red label.
- Settings Display: the clock zone (Zulu / Local) under the favourites bar.
- Power button left of the clock: fades to black; a tap wakes it (logo, a loading bar, then home fades in as the logo rises).
- ChartFox's fox and the weather-partly-snowy-rainy pictogram (Weather without WXPlane); icon paths now taken from the MDI repository.

## 1.0 (first release)
- Everything above, plus since 0.9.1: Dispatch tabs (Briefing, Summary, Navlog, Weather, NOTAMs, ATC Flight Plan) with tap-to-find in
  the briefing PDF; Discord web app; Select Parking on the map's stands, departure and arrival gates highlighted; one tap opens a map
  card; traffic is coloured only within 6 nm (an RA only within 3 nm), not from TCAS's time test alone; xPilot's Messages, Nearby ATC, Notifications and Aircraft Labels; Theme-Colored Apps; custom theme Reset; Local Connect
  refresh rate, full-screen button and the tablet's theme on the device's page; the media volume put back when X-Plane closes.
- Fixes: a crash when a Local Connect device connected before the tablet was first drawn; the Settings sidebar no longer flickers;
  aircraft that look for AviTab by its plugin signature (Aerobask DA42) switch tablet pages; WXPlane 5.1.9's own icon kept off the
  tablet; the ToLiss cockpit screen filled to its edges; openSAM jetways docked again after the beacon goes off.

## 1.0.1
- SimBrief Pilot ID typed on a number keypad (digits only, 10 at most); the 2-minute check never runs while it is typed (5 s after
  the last change); a fetch asked for while one is on its way waits its turn (the main thread never joins a downloading thread);
  an answer for an ID since changed is set aside; the STAR read when the plan has a SID.
- C++ exceptions in X-Plane's callbacks caught (Safe<callback, fallback>::call) and logged; one while the page is built: ImGui's
  stacks put back (ErrorRecoveryStoreState), the page closed, a "Page Closed" note.
- Tests: SimBrief answers in SimBrief's own format and odd ones (error, cut short, wrong types, 3000 fixes, 300 deep).

## 1.0.2
- Windows media: the computer volume's output chosen in Settings, Media (any active render endpoint, by its ID; unplugged: the
  default until it is back; switching gives the one before its volume back); players' and radio apps' sessions found on every
  output, not only the default (Voicemeeter, headsets).
- Documents: the files' folder labels and the page's filter spelled the same (kDocsFolderXPTablet; "XPTablet docs" vs "XPTablet
  Docs" showed nothing); the AviTab button removed (its charts under All); the list folds away (docs_list_hidden).

## 1.1.2
- Runways And Procedures (an IVAO pilot's ask, 2026-11-28): the Flight Plan's Route card opens it. The departure's and arrival's
  runways, SIDs, STARs, transitions and approaches read from the CIFP (readAirportProcs); a change (Actions::procEdit) goes into the
  plan (applyProcEdit; SimBrief's kept in ofp*), the map's lines are built again and the route spliced: departure, new SID, SimBrief's
  legs between (from where the SID ends to where the STAR starts; legs' proc flags from is_sid_star), new STAR, approach, destination
  (routeWithProcedures). The navlog stays SimBrief's.
- Diversions: to the alternate, direct, with no new SimBrief plan (divertToAlternate; its runway and STAR chosen in the window, which
  opens); SimBrief's diversion in flight goes direct to its STAR's first leg, or the airport (divertFrom), no SID. Undo as before.

## Emulator
- emulator/: the real plugin built against a stand-in X-Plane (every XPLM call it makes), a simulated aircraft, traffic, time and
  weather, and a control panel; a desktop app (GLFW) for Windows/macOS/Linux and a screen-less Linux one for scripted tests and CI.
  (Kept off GitHub, on the developer's machine.) It already found one thing: after XPluginStop the plugin kept its font scale, so a second start in the same
  process (without unloading) crashed; it now resets it.
