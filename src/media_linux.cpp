// Linux: MPRIS media players (Spotify, browsers, VLC, Rhythmbox...) on the session D-Bus. libdbus-1 is loaded when the plugin runs
// (every desktop has it), so the plugin itself needs nothing extra; without it, there are no media controls.
#include "media_platform.h"

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <vector>

namespace xpt {
namespace media {
namespace platform {
namespace {

// libdbus's types, as far as they are used (the iterator is an opaque block, the error a small struct)
struct DBusError { const char* name; const char* message; unsigned int dummy; void* padding1; };
struct DBusMessageIter { void* d1; void* d2; uint32_t d3; int d4, d5, d6, d7, d8, d9, d10, d11, pad1; void* pad2; void* pad3; };
typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
enum { BUS_SESSION = 0, T_INVALID = 0, T_STRING = 's', T_ARRAY = 'a', T_VARIANT = 'v', T_DICT = 'e', T_DOUBLE = 'd', T_BOOLEAN = 'b',
       T_OBJECT_PATH = 'o', T_INT64 = 'x', T_UINT64 = 't', T_INT32 = 'i' };

struct Api {
    void (*error_init)(DBusError*);
    void (*error_free)(DBusError*);
    unsigned (*error_is_set)(const DBusError*);
    DBusConnection* (*bus_get)(int, DBusError*);
    DBusMessage* (*new_method_call)(const char*, const char*, const char*, const char*);
    unsigned (*append_args)(DBusMessage*, int, ...);
    DBusMessage* (*send_block)(DBusConnection*, DBusMessage*, int, DBusError*);
    void (*unref)(DBusMessage*);
    unsigned (*iter_init)(DBusMessage*, DBusMessageIter*);
    int (*iter_type)(DBusMessageIter*);
    void (*iter_recurse)(DBusMessageIter*, DBusMessageIter*);
    unsigned (*iter_next)(DBusMessageIter*);
    void (*iter_basic)(DBusMessageIter*, void*);
    void (*iter_init_append)(DBusMessage*, DBusMessageIter*);
    unsigned (*iter_open)(DBusMessageIter*, int, const char*, DBusMessageIter*);
    unsigned (*iter_append_basic)(DBusMessageIter*, int, const void*);
    unsigned (*iter_close)(DBusMessageIter*, DBusMessageIter*);
    void (*conn_set_exit)(DBusConnection*, unsigned);
};
Api d;
void* g_lib = nullptr;
DBusConnection* g_bus = nullptr;
std::string g_player;                 // the bus name of the player shown ("org.mpris.MediaPlayer2.spotify")
std::map<std::string, double> g_baseVolume;   // a player's own volume before it was lowered

template <class F> bool sym(F& f, const char* n) { f = (F)dlsym(g_lib, n); return f != nullptr; }

DBusMessage* call(const char* dest, const char* path, const char* iface, const char* method, const char* a1 = nullptr, const char* a2 = nullptr) {
    DBusMessage* m = d.new_method_call(dest, path, iface, method);
    if (!m) return nullptr;
    if (a1 && a2) d.append_args(m, T_STRING, &a1, T_STRING, &a2, T_INVALID);
    else if (a1) d.append_args(m, T_STRING, &a1, T_INVALID);
    DBusError e;
    d.error_init(&e);
    DBusMessage* r = d.send_block(g_bus, m, 400, &e);
    d.unref(m);
    if (d.error_is_set(&e)) d.error_free(&e);
    return r;
}
std::string iterString(DBusMessageIter* it) {
    const int t = d.iter_type(it);
    if (t != T_STRING && t != T_OBJECT_PATH) return std::string();
    const char* s = nullptr;
    d.iter_basic(it, &s);
    return s ? s : "";
}
// a variant's value: a string, a list of strings joined, a number, a truth
struct Val { std::string s; double num = 0; bool b = false; bool isNum = false; };
Val variant(DBusMessageIter* v) {
    Val out;
    DBusMessageIter in;
    d.iter_recurse(v, &in);
    const int t = d.iter_type(&in);
    if (t == T_STRING || t == T_OBJECT_PATH) out.s = iterString(&in);
    else if (t == T_DOUBLE) { double x = 0; d.iter_basic(&in, &x); out.num = x; out.isNum = true; }
    else if (t == T_BOOLEAN) { uint32_t x = 0; d.iter_basic(&in, &x); out.b = x != 0; }
    else if (t == T_INT64 || t == T_UINT64) { int64_t x = 0; d.iter_basic(&in, &x); out.num = (double)x; out.isNum = true; }
    else if (t == T_INT32) { int32_t x = 0; d.iter_basic(&in, &x); out.num = x; out.isNum = true; }
    else if (t == T_ARRAY) {
        DBusMessageIter a;
        d.iter_recurse(&in, &a);
        while (d.iter_type(&a) == T_STRING) { if (!out.s.empty()) out.s += ", "; out.s += iterString(&a); if (!d.iter_next(&a)) break; }
    }
    return out;
}
// a{sv} into a map; Metadata (itself a{sv}) is flattened with its own keys
void readDict(DBusMessageIter* arr, std::map<std::string, Val>& out) {
    DBusMessageIter e;
    d.iter_recurse(arr, &e);
    while (d.iter_type(&e) == T_DICT) {
        DBusMessageIter kv;
        d.iter_recurse(&e, &kv);
        const std::string key = iterString(&kv);
        d.iter_next(&kv);
        if (d.iter_type(&kv) == T_VARIANT) {
            if (key == "Metadata") {
                DBusMessageIter in;
                d.iter_recurse(&kv, &in);
                if (d.iter_type(&in) == T_ARRAY) readDict(&in, out);
            } else {
                out[key] = variant(&kv);
            }
        }
        if (!d.iter_next(&e)) break;
    }
}
bool getAll(const std::string& name, const char* iface, std::map<std::string, Val>& out) {
    DBusMessage* r = call(name.c_str(), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "GetAll", iface);
    if (!r) return false;
    DBusMessageIter it;
    if (d.iter_init(r, &it) && d.iter_type(&it) == T_ARRAY) readDict(&it, out);
    d.unref(r);
    return true;
}
std::vector<std::string> players() {
    std::vector<std::string> v;
    DBusMessage* r = call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames");
    if (!r) return v;
    DBusMessageIter it, a;
    if (d.iter_init(r, &it) && d.iter_type(&it) == T_ARRAY) {
        d.iter_recurse(&it, &a);
        while (d.iter_type(&a) == T_STRING) {
            const std::string n = iterString(&a);
            if (n.compare(0, 23, "org.mpris.MediaPlayer2.") == 0) v.push_back(n);
            if (!d.iter_next(&a)) break;
        }
    }
    d.unref(r);
    return v;
}
void playerCall(const char* method) {
    if (g_player.empty()) return;
    if (DBusMessage* r = call(g_player.c_str(), "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", method)) d.unref(r);
}
bool setPlayerVolume(const std::string& name, double v) {
    DBusMessage* m = d.new_method_call(name.c_str(), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Set");
    if (!m) return false;
    DBusMessageIter it, var;
    const char* iface = "org.mpris.MediaPlayer2.Player";
    const char* prop = "Volume";
    d.iter_init_append(m, &it);
    d.iter_append_basic(&it, T_STRING, &iface);
    d.iter_append_basic(&it, T_STRING, &prop);
    d.iter_open(&it, T_VARIANT, "d", &var);
    d.iter_append_basic(&var, T_DOUBLE, &v);
    d.iter_close(&it, &var);
    DBusError e;
    d.error_init(&e);
    DBusMessage* r = d.send_block(g_bus, m, 400, &e);
    d.unref(m);
    const bool ok = r != nullptr && !d.error_is_set(&e);
    if (d.error_is_set(&e)) d.error_free(&e);
    if (r) d.unref(r);
    return ok;
}

} // namespace

bool init() {
    g_lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) return false;
    bool ok = sym(d.error_init, "dbus_error_init") && sym(d.error_free, "dbus_error_free") && sym(d.error_is_set, "dbus_error_is_set") &&
              sym(d.bus_get, "dbus_bus_get_private") && sym(d.new_method_call, "dbus_message_new_method_call") &&
              sym(d.append_args, "dbus_message_append_args") && sym(d.send_block, "dbus_connection_send_with_reply_and_block") &&
              sym(d.unref, "dbus_message_unref") && sym(d.iter_init, "dbus_message_iter_init") && sym(d.iter_type, "dbus_message_iter_get_arg_type") &&
              sym(d.iter_recurse, "dbus_message_iter_recurse") && sym(d.iter_next, "dbus_message_iter_next") && sym(d.iter_basic, "dbus_message_iter_get_basic") &&
              sym(d.iter_init_append, "dbus_message_iter_init_append") && sym(d.iter_open, "dbus_message_iter_open_container") &&
              sym(d.iter_append_basic, "dbus_message_iter_append_basic") && sym(d.iter_close, "dbus_message_iter_close_container") &&
              sym(d.conn_set_exit, "dbus_connection_set_exit_on_disconnect");
    if (!ok) { dlclose(g_lib); g_lib = nullptr; return false; }
    DBusError e;
    d.error_init(&e);
    g_bus = d.bus_get(BUS_SESSION, &e);   // a private connection of our own (X-Plane's other plugins keep theirs)
    if (d.error_is_set(&e)) d.error_free(&e);
    if (!g_bus) return false;
    d.conn_set_exit(g_bus, 0);           // a lost bus must not end X-Plane
    return true;
}
void shutdown() {
    g_bus = nullptr;   // (a private connection would be closed and unreffed; libdbus stays loaded for the process)
}

bool poll(Info& info, std::string& artBytes) {
    artBytes.clear();
    if (!g_bus) return false;
    // the player to show: one that is playing, else the one shown before, else the first
    const std::vector<std::string> list = players();
    if (list.empty()) { g_player.clear(); return false; }
    std::string pick;
    std::map<std::string, Val> props;
    for (const std::string& n : list) {
        std::map<std::string, Val> p;
        if (!getAll(n, "org.mpris.MediaPlayer2.Player", p)) continue;
        if (p["PlaybackStatus"].s == "Playing") { pick = n; props = p; break; }
        if (n == g_player || pick.empty()) { pick = n; props = p; }
    }
    if (pick.empty()) return false;
    g_player = pick;
    std::map<std::string, Val> idp;
    getAll(pick, "org.mpris.MediaPlayer2", idp);
    info.app = idp["Identity"].s.empty() ? pick.substr(23) : idp["Identity"].s;
    info.title = props["xesam:title"].s;
    info.artist = props["xesam:artist"].s;
    info.album = props["xesam:album"].s;
    info.artUrl = props["mpris:artUrl"].s;
    info.playing = props["PlaybackStatus"].s == "Playing";
    info.canPrev = props.count("CanGoPrevious") ? props["CanGoPrevious"].b : true;
    info.canNext = props.count("CanGoNext") ? props["CanGoNext"].b : true;
    if (props.count("Volume") && props["Volume"].isNum && !g_baseVolume.count(pick)) {}   // (taken when lowering)
    return true;
}
void send(int what) {
    playerCall(what == 1 ? "PlayPause" : what == 2 ? "Previous" : what == 3 ? "Next" : what == 4 ? "Play" : "Pause");
}
bool setVolume(float factor) {
    if (g_player.empty()) return false;
    if (factor >= 0.999f) {   // back to what it was
        auto it = g_baseVolume.find(g_player);
        if (it == g_baseVolume.end()) return true;
        const bool ok = setPlayerVolume(g_player, it->second);
        g_baseVolume.erase(it);
        return ok;
    }
    std::map<std::string, Val> p;
    if (!getAll(g_player, "org.mpris.MediaPlayer2.Player", p) || !p["Volume"].isNum) return false;   // this player keeps its volume to itself
    const double base = g_baseVolume.count(g_player) ? g_baseVolume[g_player] : p["Volume"].num;
    if (!setPlayerVolume(g_player, base * factor)) return false;
    g_baseVolume[g_player] = base;
    return true;
}
bool radioApps(std::string&) { return false; }

} // namespace platform
} // namespace media
} // namespace xpt
