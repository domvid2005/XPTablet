// The plugin's side of the web browser: starts xptbrowser (src/browser_helper.cpp) when a web page is first wanted, shares a
// block of memory with it (src/browser_ipc.h), sends it commands and takes its pictures. No X-Plane or OpenGL calls here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "browser_ipc.h"

namespace xpt {
namespace browser {

// helper: the xptbrowser program; cefDir: X-Plane's CEF folder; profile: where cookies and logins are kept
bool start(const std::string& helper, const std::string& cefDir, const std::string& profile, std::string& err);
void stop();                    // asks the helper to quit, and makes sure it has
bool started();
int state();                    // 0 starting, 1 running, -1 failed (error()), -2 not started
std::string error();
void push(int type, int a = 0, int b = 0, int c = 0, int d = 0, float f = 0, const std::string& text = std::string());
void heartbeat();
// The newest picture, if there is one the plugin has not taken: BGRA, w x h. Call done() after uploading it.
bool frame(const uint8_t*& px, int& w, int& h);
void done();
// The shown page's address, title and state, and the tabs (the shown one's id in activeTab), when they changed since the last call.
struct TabInfo { int id = 0; std::string title, url; bool loading = false; int group = 0; };
bool info(std::string& url, std::string& title, bool& loading, bool& canBack, bool& canForward, bool& wantsKeyboard, std::vector<TabInfo>& tabs, int& activeTab, int& zoomPct, bool& fullscreen);

} // namespace browser
} // namespace xpt
