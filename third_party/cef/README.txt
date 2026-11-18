Chromium Embedded Framework (CEF) C API headers, from https://github.com/chromiumembedded/cef at commit a26f38b
("Update to Chromium version 117.0.5938.92"): the exact build X-Plane 12 ships in Resources/dlls/64/cef
(CEF 117.1.4+ga26f38b+chromium-117.0.5938.92). BSD licence, LICENSE.txt.

XPTablet's browser helper uses only these headers. It brings no CEF binaries of its own: it loads X-Plane's libcef.
Two files are XPTablet's own stand-ins for files CEF's build generates: include/cef_config.h and
include/base/internal/cef_net_error_list.h.
