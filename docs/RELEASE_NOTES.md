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
- **The floating window remembers its place and size** after the sim or the plugins reload.

## Faster, lighter

- Web pages are paused while they are not on screen: no more drawing in the background (music keeps playing).
- The map's navaids, fixes and airways load when the Map is first opened, not as soon as you are on the ground.
- Less work each frame when the screen is dimmed.

## Ready for XGSE

- Alpine Aviation's upcoming XGSE ground services plugin will take over the GSE page with its own screen when it is installed, the
  way WXPlane takes over Weather. Until then, nothing changes.
