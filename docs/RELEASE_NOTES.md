# XPTablet 1.1.0

A feature release for XPTablet, the free cockpit tablet for X-Plane 12 by Alpine Aviation.

**Install:** download `XPTablet-1.1.0.zip` below, unzip it, and copy the `XPTablet` folder into `X-Plane 12/Resources/plugins/`,
replacing the old one. SkunkCrafts Updater users get it automatically.

## New

- **GSE (was Aircraft):** a redesigned ground page. The aircraft is drawn larger with its jetway bubbles by the doors and the jetway
  status beside the nose, and pushback (BetterPushback or X-Plane's tug) always sits in the right third, with what is still in the
  way listed above its buttons.
- **Cabin:** a new app in the App Store, under the new **Aircraft Management** category, with a lightbulb icon. It has the ToLiss
  cabin lights (Auto, On, Off and a dimmer), the slides (Arm, Disarm, Auto) and Cabin Ready. Outside a ToLiss its icon is greyed and
  does nothing. Existing users get it installed once, since these controls used to be on the Aircraft page.
- **Performance settings:** a new Settings page.
  - **Tablet Frame Rate:** 15, 20, 30, 60 or Max (default 30). Lower leaves more of the computer to X-Plane. Local Connect devices
    get the same rate (its own Refresh Rate slider is gone).
  - **Close Unused Web Pages:** Never, or after 5, 15 or 30 minutes unseen (not while something plays). The Browser's tabs open
    again, and sites stay signed in.
  - **Reduced Memory Use:** fewer map tiles kept, and the map's navaids, fixes and airways put away after 5 minutes without the Map.
- **Headings in the navlog:** Flight Plan's and Dispatch's navlogs have a **Headings** view beside **Times**: each leg's track, the
  heading to fly in the wind, the wind correction (left or right) and the wind, from your SimBrief plan (Dispatch adds the
  groundspeed).
- **Documents with folders and tabs:** Documents now shows your folders as they are on disk, as AviTab does with its charts: XPTablet
  Docs and AviTab Charts at the top, the folders inside them, then the files. Open several documents at once and switch between them
  as tabs along the top; a PDF stays open where you left it, and a picture shows again at once. A search box finds files by name
  in the folder you have open and every folder inside it.
- **Documents lists every file:** it stopped after 500 files, so a large chart collection (a country's whole AIP) was cut off part
  way. Every file is listed now, and the list scrolls with the wheel anywhere over it.
- **The floating window remembers its place and size** after the sim or the plugins reload.

## Faster, lighter

- Web pages are paused while they are not on screen: no more drawing in the background (music keeps playing).
- The map's navaids, fixes and airways load when the Map is first opened, not as soon as you are on the ground.
- Less work each frame when the screen is dimmed.

## Ready for XGSE

- Alpine Aviation's upcoming XGSE ground services plugin will take over the GSE page with its own screen when it is installed, the
  way WXPlane takes over Weather. Until then, nothing changes.
