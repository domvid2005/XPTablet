# XPTablet 1.1.1 – a cockpit tablet for X-Plane 12

**XPTablet is a free tablet for your cockpit, by Alpine Aviation.** It appears on the tablet of any aircraft that was built for AviTab, with no change to the aircraft, and in a floating window (also in VR) on every other aircraft. On it: a moving map with live traffic, airports and frequencies, your SimBrief flight plan and briefing, weather, jetways and pushback, checklists, charts and web apps, your music, and the whole tablet on your phone or iPad.

Free. Windows, macOS and Linux. X-Plane 12.

## What it does

- **Fits the aircraft you already fly:** XPTablet reads the same `AviTab.json` in the aircraft's folder and answers to AviTab's datarefs and commands, so the tablet in the cockpit just works. Aircraft without a tablet get a window: Plugins > XPTablet, or the command `XPTablet/toggle_tablet`.
- **Home screen:** apps, folders, widgets (weather, flight, frame rate) and a favorites bar, arranged the way you like. Pull down from the top for notifications, media, volume and brightness.
- **Moving map:** street map, satellite or no map, north up or heading up. Navaids, fixes, airports, time zones and the day/night line.
- **Live traffic:** X-Plane's and VATSIM's aircraft with trails and routes. Tap one for its altitude, speed, route and pilot. TCAS-style alerts can send a notification.
- **Airports down to the gate:** runways, taxiways and stands from X-Plane's airport data and your own scenery. Tap a stand for its size and airlines, and select your parking. Departure and arrival gates are highlighted.
- **Route and procedures:** your SimBrief route on the map with its SID, STAR and approach.
- **VATSIM:** airspaces and who is online, controller details and ATIS, a notification when ATC comes online at your departure or destination, and buttons for xPilot's windows (messages, nearby ATC, notifications, aircraft labels).
- **Airports:** search any airport by code or name: runways, elevation, local time, radio frequencies, CTAF and live VATSIM ATC. Tap the headset beside a frequency to put it in COM1 standby.
- **Flight Plan:** import your latest SimBrief flight with your SimBrief Pilot ID: route, fuel and weights, navlog (times, or track, heading, wind correction and wind) and progress along the route. A new plan can load by itself.
- **Dispatch:** the SimBrief briefing by section: Briefing, Summary, Navlog, Weather, NOTAMs and ATC Flight Plan.
- **Weather:** X-Plane's own weather at the aircraft (wind, visibility, clouds, winds aloft) and decoded METARs for any airport. With WXPlane installed, WXPlane's page appears here.
- **GSE:** connect openSAM jetways door by door, and call BetterPushback or X-Plane's own tug, with what is still in the way listed. Top views for many aircraft types.
- **Cabin (ToLiss):** the cabin lights with a dimmer, the slides and Cabin Ready, in an app of its own (App Store, Aircraft Management).
- **Checklists:** normal, abnormal and emergency checklists for airliners, turboprops and light aircraft. Add your own as a text file per aircraft type.
- **Documents:** your PDFs and pictures (and AviTab's charts folder), browsed folder by folder with a search, several open at once as tabs, with a list that folds away to read across the page.
- **App Store:** add only what you use. Charts (ChartFox, Navigraph, SkyVector, ForeFlight), SimBrief, D-ATIS, VATSIM Radar, Flightradar24, Discord, Spotify, YouTube and YouTube Music, Apple Music, SoundCloud, plus XPTablet's own Timers, Logbook, ACARS, Games and Cabin.
- **Browser:** tabs, bookmarks, history, zoom and an on-screen keyboard, with touch scrolling.
- **ACARS:** Hoppie's ACARS with CPDLC, pre-departure clearances and telex (a free Hoppie logon code).
- **Logbook, Timers, Notes, Navaids:** every flight logged by itself (times, route, landing rate, fuel); a stopwatch, hold timer, ETA and top of descent; a clearance form, ATIS form and scratchpad; and a search for any VOR, NDB, ILS, DME or fix.
- **Media:** what is playing on your computer, with its cover and controls. It can lower or pause your music while the radio receives or transmits. On Windows, choose which sound output the volume controls (Voicemeeter users: your speakers or headset).
- **Local Connect:** the same tablet on a phone or iPad on your Wi-Fi, worked by touch. Details below.
- **Performance:** choose the tablet's frame rate, close web pages you are not using, and a reduced memory mode. Web pages pause by themselves while not on screen.
- **Themes:** seven color themes, three custom ones (your own colors, picked on a color ring and triangle, or a background picture), seasonal and holiday themes, and an option to color the built-in app icons to match.
- **Games:** 2048, Knots and Crosses, and six aviation games of its own: Contrail, Taxi Rush, Sky Patrol, Hangar Climb, Cockpit 21 and Cargo Load.

## Install

1. Unzip, and copy the `XPTablet` folder into `X-Plane 12/Resources/plugins/`.
2. Start X-Plane. On an aircraft with an AviTab tablet, XPTablet is on it. On any other, open it from Plugins > XPTablet.

**Updates:** XPTablet works with the SkunkCrafts Updater, which installs it and keeps it up to date. The tablet also tells you when a new version is out.

**User manual:** a full manual with pictures comes with the plugin, in `XPTablet/docs`, and opens in the tablet's own Documents app.

**If AviTab is installed:** the two cannot share the cockpit tablet. XPTablet leaves the cockpit tablet to AviTab and runs in its window only. To put XPTablet on the cockpit tablet, move the `AviTab` folder out of `Resources/plugins`.

## Local Connect

Settings > Local Connect, switch it on, and open the address it shows in the browser of a phone or iPad on the same Wi-Fi. The browser warns once that the connection is not private, because the certificate is one your own computer made: choose Advanced or Show Details, then continue. Enter the 6-digit code XPTablet shows in the sim, and after that the device comes straight in each time.

The device shows the tablet in the sim and works it by touch. It is one tablet: what you open on the device is open in the cockpit.

How it is kept safe: it is off until you switch it on, it answers only on your home network, everything is encrypted, each device is paired once with a code shown in the sim, and you can remove a device at any time.

## Good to know

- **macOS:** the web pages (Browser, web apps, PDFs) and the media controls are not available on macOS yet. Everything else is.
- **Optional, and used when installed:** openSAM (jetways), BetterPushback, xPilot, WXPlane, a SimBrief Pilot ID, a Hoppie logon code, and Navigraph's AIRAC in Custom Data.
- **Accounts:** your SimBrief Pilot ID and Hoppie code are kept only in XPTablet's settings on your computer.
- **Your files:** documents go in `Output/XPTablet/Documents`, your own checklists in `Output/XPTablet/Checklists`.
- **Found a problem?** Please report it here with X-Plane's `Log.txt`, and `Output/XPTablet/crash-last.txt` if there is one.
- For flight simulation only. Never for real-world navigation.

## Pictures

1. `01_XPTablet_Intro.png` – XPTablet
2. `02_XPTablet_Home.png` – Home screen and widgets
3. `03_XPTablet_Moving_Map.png` – Moving map with live traffic
4. `04_XPTablet_Airport_Map.png` – Airport layout, stands and Select Parking
5. `05_XPTablet_Route_And_Procedures.png` – Route, procedures and VATSIM airspace
6. `06_XPTablet_Airports.png` – Airports and frequencies
7. `07_XPTablet_Flight_Plan_And_Dispatch.png` – SimBrief flight plan and Dispatch
8. `08_XPTablet_Weather.png` – Weather and METAR search
9. `09_XPTablet_GSE_And_Cabin.png` – GSE (jetways and pushback) and Cabin
10. `10_XPTablet_Checklists.png` – Checklists
11. `11_XPTablet_Pilot_Tools.png` – ACARS, Logbook, Timers, Notes, Navaids, xPilot
12. `12_XPTablet_App_Store.png` – App Store
13. `13_XPTablet_Media.png` – Media and the pull-down
14. `14_XPTablet_Themes.png` – Themes
15. `15_XPTablet_Local_Connect.png` – Local Connect
16. `16_XPTablet_Games.png` – Games

## Credits

XPTablet is made by Alpine Aviation. It stands on the work of others, with thanks:

- Dear ImGui by Omar Cornut and contributors (MIT)
- AircraftShapesSVG by RexKramer1 and amnesica (GPL-3.0): the aircraft top views
- FlightGear A320-family by Josh Davidson, Jonathan Redpath and contributors (GPL-2.0): the Airbus checklists, adapted
- IBM Plex Sans (IBM) and JetBrains Mono (SIL Open Font License)
- libcurl, zlib, Mbed TLS, stb_image and stb_image_write
- Chromium Embedded Framework (X-Plane's own copy) for the web pages
- Map: © OpenStreetMap contributors; satellite imagery: Esri, Maxar, Earthstar Geographics
- Airport layouts: X-Plane's airport data, the Scenery Gateway and its contributors
- Airport facts: OurAirports. Time zones: Natural Earth. METARs: NOAA Aviation Weather Center
- VATSIM's data feed, SimBrief, Hoppie's ACARS
- openSAM by Holger Teutsch (hotbso) and contributors: XPTablet uses its commands and datarefs, and contains none of its code
- Material Design Icons by Pictogrammers and Google's Material Icons (Apache 2.0)

XPTablet is not made by or affiliated with ForeFlight, Navigraph, SkyVector, VATSIM Radar, ChartFox, SimBrief, VATSIM, Flightradar24, Discord, Spotify, Google / YouTube, Apple, SoundCloud or openSAM. Their names and logos belong to them and are shown only to open their own web apps. AviTab is a separate plugin by Folke Will; XPTablet is not affiliated with it.

## License

Freeware, © 2026 Alpine Aviation. Free to download and use. Please do not sell it, re-upload it elsewhere or publish changed versions: link to this page instead. Screenshots, videos and streams are welcome. Full terms are in `LICENSE.txt`, and the third-party licenses in `LICENSES.txt`.

Source code: https://github.com/domvid2005/XPTablet
