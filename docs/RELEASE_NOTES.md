# XPTablet 1.0.1

A fix release for XPTablet, the free cockpit tablet for X-Plane 12 by Alpine Aviation.

**Install:** download `XPTablet-1.0.1.zip` below, unzip it, and copy the `XPTablet` folder into `X-Plane 12/Resources/plugins/`,
replacing the old one. SkunkCrafts Updater users get it automatically.

## Changes

- **SimBrief Pilot ID on a keypad:** Settings > Accounts now takes your SimBrief Pilot ID (the number under Account Settings on
  simbrief.com) on a number keypad. Only digits are kept, whether typed, pasted or entered with the computer's keys. A user name
  saved before keeps working.
- **No freeze on Import:** pressing Import while the 2-minute SimBrief check is still downloading no longer makes X-Plane wait for it.
- **No check while typing:** SimBrief is no longer asked about a Pilot ID that is still being typed (it could ask about pilot "1"
  and offer someone else's flight).
- **The STAR is read again:** with a SID in the plan, the STAR was missing on the map and from the crossing restrictions.
- **Safer:** an error in the tablet's code no longer closes X-Plane. It is written to Log.txt, and the page it happened on is
  closed ("Page Closed").
