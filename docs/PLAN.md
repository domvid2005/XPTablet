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
