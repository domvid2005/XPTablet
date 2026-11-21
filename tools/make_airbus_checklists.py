#!/usr/bin/env python3
"""Build assets/checklists/airbus.txt from the FlightGear A320-family checklists (A32X-Checklists.xml, the "Standard" group).

Source: legoboyvdlp/A320-family (https://github.com/legoboyvdlp/A320-family), GPL-2.0. The checklist text is theirs; this adds
lines for the type and the engine. A line or section starting "[TAGS] " shows only for those aircraft (ICAO) or engines.
Usage: make_airbus_checklists.py path/to/A32X-Checklists.xml assets/checklists/airbus.txt
"""
import sys
import xml.etree.ElementTree as ET

NARROW = "A319,A320,A20N,A321,A21N"
TWIN = "A319,A320,A20N,A321,A21N,A339,A333"
# what to add, after an item (section title, item name) or at a section's end (name None): (tags, item, value)
EXTRA = {
    ("BEFORE START", "BEACON"): [("LEAP,PW1100G", "ENG START (BOWED ROTOR MOTORING)", "ALLOW TIME")],
    ("BEFORE START", None): [("A339,A333,A346", "CTR / TRIM TANK FUEL", "CHECKED")],
    ("TAXI/BEFORE TAKEOFF", "ECAM MEMO"): [("V2500", "TO THRUST", "EPR CHECKED"), ("CFM56,LEAP,PW1100G", "TO THRUST", "N1 CHECKED")],
}
REPLACE = {   # an item swapped per type: (section, item) -> [(tags, item, value)]
    ("PARKING", "ENGINES"): [(TWIN, "ENGINES", "OFF"), ("A346", "ENGINES 1-4", "OFF")],
}

def main(src, out):
    root = ET.parse(src).getroot()
    std = [g for g in root.findall("group") if g.findtext("name") == "Standard"][0]
    lines = [
        "// Airbus normal checklists (A319, A320, A320neo, A321, A321neo; adapted for the A330-900, A330-300 and A340-600).",
        "// From the FlightGear A320-family checklists by Josh Davidson (Octal450), Sven Seipp, Jarno de Wit and the A320-family",
        "// contributors: https://github.com/legoboyvdlp/A320-family - licensed under the GNU GPL version 2 (see LICENSE-GPL-2.0.txt).",
        "// Changes (XPTablet): converted to XPTablet's checklist format; lines for the type and the engine (in [brackets]) added.",
        "// Not for real-world flight.",
    ]
    for c in std.findall("checklist"):
        title = c.findtext("title").strip()
        lines.append("# " + title.title())
        for it in c.iter("item"):
            name = (it.findtext("name") or "").strip()
            vals = [v.text.strip() for v in it.findall("value") if v.text]
            if not name:
                continue
            if (title, name) in REPLACE:
                for tags, n, v in REPLACE[(title, name)]:
                    lines.append("[%s] %s | %s" % (tags, n, v))
            else:
                lines.append("%s | %s" % (name, " / ".join(vals)))
            for tags, n, v in EXTRA.get((title, name), []):
                lines.append("[%s] %s | %s" % (tags, n, v))
        for tags, n, v in EXTRA.get((title, None), []):
            lines.append("[%s] %s | %s" % (tags, n, v))
    open(out, "w").write("\n".join(lines) + "\n")
    print("wrote", out, len(lines), "lines")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
