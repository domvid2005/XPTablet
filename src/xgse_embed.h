// XGSE on another plugin's screen. A plugin that wants XGSE's Ground page on a surface of its own (XPTablet shows it as the
// tablet's Ground page) sends XGSE this message from inside its own drawing callback, once per surface per frame:
//     XPLMSendMessageToPlugin(XPLMFindPluginBySignature(XGSE_SIGNATURE), XGSE_MSG_EMBED, &request);
// Before sending, the caller sets OpenGL's modelview matrix so that the origin is the top-left corner of the area, x runs right,
// y runs DOWN, and one unit is one pixel of the surface being drawn. XGSE draws its page into widthPx x heightPx there (no scissor
// test is used: everything is clipped to the area before it reaches OpenGL) and sets `handled`. An XGSE too old to know the
// message leaves `handled` at 0. The same file is in XPTablet's source (src/xgse_embed.h): change both together and raise `version`.
//
// While a host shows XGSE's page, XGSE does the ground work (jetways, doors, ground power, pushback): the host leaves its own
// ground automation alone. A host can tell XGSE is running by the plugin signature, or by the dataref xgse/running (int, 1).
#pragma once

#define XGSE_SIGNATURE "com.alpineaviation.xgse"
#define XGSE_MSG_EMBED 0x01475345   /* "GSE": plugin-defined messages must be above 0x00FFFFFF */

struct XgseEmbedRequest {
    int structSize;            // sizeof(XgseEmbedRequest)
    int version;               // 1
    float widthPx, heightPx;   // the area, in the units of the caller's matrix
    float pixelsPerUnit;       // real screen pixels per unit (1 on a panel texture, the UI scale in a window): for crisp text
    float mouseX, mouseY;      // the pointer in the area (same units, from its top-left corner, y down); far outside when not over it
    int mouseDown;             // the pointer is pressed
    float wheel;               // mouse wheel clicks since the last frame (up positive)
    float brightness;          // 0..1: the screen's brightness (colours are scaled by it)
    int frame;                 // X-Plane's cycle number: a second request in the same frame repeats the picture on another surface
    int handled;               // out: XGSE sets 1
    // the host's look, so the page matches the screen around it (0xRRGGBB): its accent (what is chosen) and the bottom of its
    // background (XGSE's dark tones are turned towards it). 0 and 0: XGSE's own navy.
    unsigned int accentRgb;
    unsigned int skyRgb;
    // what the host knows about the flight, for the ground equipment's livery (empty when it knows nothing): the airline's ICAO
    // designator from a flight plan (SimBrief's OFP), and the flight's callsign ("DAL1234")
    char planAirline[8];
    char planCallsign[16];
};
