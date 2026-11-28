// XGSE on XPTablet's GSE page. XGSE (Alpine Aviation's ground services plugin: its vehicles, jetways and stairs) can draw its own
// screen where XPTablet's GSE page is, the way WXPlane does on the Weather page (wxplane_embed.h). XPTablet sends XGSE this message
// from inside its own drawing callback, once per surface per frame, while the GSE page is shown:
//     XPLMSendMessageToPlugin(XPLMFindPluginBySignature(XGSE_SIGNATURE), XGSE_MSG_EMBED, &request);
// Before sending, XPTablet sets OpenGL's modelview matrix so that the origin is the top-left corner of the area, x runs right, y runs
// DOWN, and one unit is one pixel of the surface being drawn (the cockpit tablet's panel texture, or the floating window). XGSE draws
// its screen into widthPx x heightPx there, without a scissor test (everything clipped to the area before it reaches OpenGL), and
// sets `handled`. While no XGSE answers (not installed, too old, or it leaves `handled` at 0), XPTablet shows its own GSE page:
// jetways through openSAM or X-Plane, and pushback. With XGSE drawing the page, jetways and services are XGSE's to work.
// The same file is in XGSE's source: change both together, and raise `version` (fields are only ever added at the end).
#pragma once

#define XGSE_SIGNATURE "com.alpineaviation.xgse"
#define XGSE_MSG_EMBED 0x01475345   /* "GSE"; plugin-defined messages must be above 0x00FFFFFF */

struct XgseEmbedRequest {
    int structSize;            // sizeof(XgseEmbedRequest): XGSE reads only the fields that fit in it
    int version;               // 1
    float widthPx, heightPx;   // the area, in the units of the caller's matrix
    float pixelsPerUnit;       // real screen pixels per unit (1 on a panel texture, the UI scale in a window): for crisp text
    float mouseX, mouseY;      // the pointer in the area (same units, from its top-left corner, y down); far outside when not over it
    int mouseDown;             // the pointer is pressed
    float wheel;               // mouse wheel clicks since the last frame (up positive)
    float brightness;          // 0..1: the screen's brightness (colours are scaled by it)
    int frame;                 // X-Plane's cycle number: a second request in the same frame repeats the picture on another surface
    unsigned accentRGBA;       // the tablet's theme: its accent colour, 0xRRGGBBAA (for XGSE's buttons to match)
    unsigned backgroundRGBA;   // and its page background, 0xRRGGBBAA
    int handled;               // out: XGSE sets 1
};
