// Local Connect's server, end to end over real TLS on this machine: the page, the refusals, pairing, the token, what is sent and
// what comes back, forgetting a device, the lockout after wrong codes.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "localserver.h"

using namespace xpt;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL: %s\n", what); ++failures; } } while (0)

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_x509_crt g_ca;
static std::string g_port;

// a browser's side of it, as small as it can be
struct Client {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    bool up = false;
    std::string buf;
    Client() { mbedtls_net_init(&net); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf); }
    ~Client() { mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf); mbedtls_net_free(&net); }
    bool connect() {
        if (mbedtls_net_connect(&net, "127.0.0.1", g_port.c_str(), MBEDTLS_NET_PROTO_TCP) != 0) return false;
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_drbg);
        mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);   // the server's own certificate, and it must be for this address
        mbedtls_ssl_conf_ca_chain(&conf, &g_ca, nullptr);
        mbedtls_ssl_conf_read_timeout(&conf, 3000);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0 || mbedtls_ssl_set_hostname(&ssl, "127.0.0.1") != 0) return false;
        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
        int r;
        while ((r = mbedtls_ssl_handshake(&ssl)) != 0) if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) { std::printf("  (handshake -0x%04x)\n", -r); return false; }
        up = true;
        return true;
    }
    bool send(const std::string& s) { std::size_t at = 0; while (at < s.size()) { const int n = mbedtls_ssl_write(&ssl, (const unsigned char*)s.data() + at, s.size() - at); if (n <= 0) return false; at += (std::size_t)n; } return true; }
    bool more() { unsigned char b[65536]; const int n = mbedtls_ssl_read(&ssl, b, sizeof b); if (n <= 0) return false; buf.append((const char*)b, (std::size_t)n); return true; }
    // an HTTP exchange: the whole answer (the server closes after it)
    std::string http(const std::string& request) { if (!send(request)) return ""; while (more()) {} return buf; }
    bool upgrade(const std::string& host, const std::string& origin) {
        send("GET /ws HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
             (origin.empty() ? std::string() : "Origin: " + origin + "\r\n") + "\r\n");
        while (buf.find("\r\n\r\n") == std::string::npos) if (!more()) return false;
        const bool ok = buf.compare(0, 12, "HTTP/1.1 101") == 0 && buf.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos;   // (RFC 6455's own example key and answer)
        buf.erase(0, buf.find("\r\n\r\n") + 4);
        return ok;
    }
    void wsSend(const std::string& m, bool masked = true) {
        std::string f;
        f.push_back((char)0x82);
        const int mk = masked ? 0x80 : 0;
        if (m.size() < 126) f.push_back((char)(mk | m.size()));
        else if (m.size() < 65536) { f.push_back((char)(mk | 126)); f.push_back((char)(m.size() >> 8)); f.push_back((char)m.size()); }
        else { f.push_back((char)(mk | 127)); for (int i = 7; i >= 0; --i) f.push_back((char)((unsigned long long)m.size() >> (8 * i))); }
        if (masked) { const char key[4] = {0x11, 0x22, 0x33, 0x44}; f.append(key, 4); for (std::size_t i = 0; i < m.size(); ++i) f.push_back((char)(m[i] ^ key[i & 3])); }
        else f += m;
        send(f);
    }
    // the next data message ("" when the connection ends or nothing comes in time); pings are answered by nobody here
    std::string wsRecv() {
        for (;;) {
            if (buf.size() >= 2) {
                const int op = buf[0] & 0x0F;
                unsigned long long len = (unsigned char)buf[1] & 0x7F;
                std::size_t pos = 2;
                if (len == 126 && buf.size() >= 4) { len = ((unsigned char)buf[2] << 8) | (unsigned char)buf[3]; pos = 4; }
                else if (len == 127 && buf.size() >= 10) { len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | (unsigned char)buf[2 + i]; pos = 10; }
                else if (len >= 126) { if (!more()) return ""; continue; }
                if (buf.size() >= pos + len) {
                    std::string m = buf.substr(pos, (std::size_t)len);
                    buf.erase(0, pos + (std::size_t)len);
                    if (op == 8) return "";
                    if (op == 2) return m;
                    continue;
                }
            }
            if (!more()) return "";
        }
    }
};

static void var(std::string& o, unsigned long long n) { while (n >= 0x80) { o.push_back((char)(n | 0x80)); n >>= 7; } o.push_back((char)n); }
static void str(std::string& o, const std::string& s) { var(o, s.size()); o += s; }
static std::string helloMsg(int version, const std::string& token, const std::string& name, const std::string& kind) { std::string m = "H"; var(m, (unsigned long long)version); str(m, token); str(m, name); str(m, kind); return m; }
static std::string textOf(const std::string& h) { return h.size() > 3 ? h.substr(3) : std::string(); }   // an 'h' message's text (short ones: one length byte)
static localserver::Status waitFor(bool (*ready)(const localserver::Status&)) {
    localserver::Status s;
    for (int i = 0; i < 300; ++i) { s = localserver::status(); if (ready(s)) break; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return s;
}
static bool pollFor(localserver::Incoming& in) { for (int i = 0; i < 200; ++i) { if (localserver::poll(in)) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); } return false; }

int main() {
    mbedtls_entropy_init(&g_entropy); mbedtls_ctr_drbg_init(&g_drbg); mbedtls_x509_crt_init(&g_ca);
    mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy, nullptr, 0);
    const fs::path dir = fs::temp_directory_path() / ("xpt_lc_test_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    std::string page = "<!doctype html><title>XPTablet</title>";
    for (int i = 0; i < 20000; ++i) page += (char)('a' + i % 26);
    localserver::Config cfg;
    cfg.bindIp = "127.0.0.1"; cfg.port = 18443; cfg.dataDir = (dir / "data").string(); cfg.page = page; cfg.wireVersion = 1; cfg.allowLoopback = true;
    localserver::start(cfg);
    const localserver::Status st = waitFor([](const localserver::Status& s) { return s.state == 2 || s.state == -1; });
    if (st.state != 2) { std::printf("FAIL: the server did not start (%s)\n", st.error.c_str()); return 1; }
    g_port = st.address.substr(st.address.rfind(':') + 1);
    const std::string host = "127.0.0.1:" + g_port, origin = "https://" + host;
    CHECK(st.address == origin, "the address to open is given");
    { std::ifstream f(dir / "data" / "cert.pem"); std::stringstream ss; ss << f.rdbuf(); const std::string pem = ss.str();
      CHECK(mbedtls_x509_crt_parse(&g_ca, (const unsigned char*)pem.c_str(), pem.size() + 1) == 0, "a certificate was made and kept"); }
#if !defined(_WIN32)
    CHECK((fs::status(dir / "data" / "key.pem").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none, "the key is this user's only");
#endif

    // the page
    { Client c; CHECK(c.connect(), "TLS: the certificate is the server's own and names its address");
      const std::string r = c.http("GET / HTTP/1.1\r\nHost: " + host + "\r\n\r\n");
      CHECK(r.compare(0, 15, "HTTP/1.1 200 OK") == 0 && r.find("Content-Type: text/html") != std::string::npos && r.find("Content-Security-Policy:") != std::string::npos, "the page is served, with its policy");
      const std::size_t b = r.find("\r\n\r\n");
      CHECK(b != std::string::npos && r.substr(b + 4) == page, "the page arrives whole");
      const std::size_t e = r.find("ETag: ");
      const std::string tag = e == std::string::npos ? "" : r.substr(e + 6, r.find("\r\n", e) - e - 6);
      Client c2; c2.connect();
      CHECK(c2.http("GET / HTTP/1.1\r\nHost: " + host + "\r\nIf-None-Match: " + tag + "\r\n\r\n").compare(0, 12, "HTTP/1.1 304") == 0, "a page already held is not sent again"); }
    { Client c; c.connect(); CHECK(c.http("GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n").compare(0, 12, "HTTP/1.1 421") == 0, "asked for by another name: refused"); }
    { Client c; c.connect(); CHECK(c.http("POST / HTTP/1.1\r\nHost: " + host + "\r\nContent-Length: 0\r\n\r\n").compare(0, 12, "HTTP/1.1 405") == 0, "only GET"); }
    { Client c; c.connect(); CHECK(c.http("GET /../key.pem HTTP/1.1\r\nHost: " + host + "\r\n\r\n").compare(0, 12, "HTTP/1.1 404") == 0, "nothing but the page is served"); }
    { Client c; c.connect(); CHECK(c.http(std::string(9000, 'A')).empty(), "an endless request is dropped"); }
    { Client c; c.connect(); CHECK(!c.upgrade(host, ""), "a connection without an origin is refused"); }
    { Client c; c.connect(); CHECK(!c.upgrade(host, "https://evil.example"), "a connection from another site's page is refused"); }

    // before pairing nothing is listened to
    { Client c; c.connect(); CHECK(c.upgrade(host, origin), "the page's own connection is taken");
      c.wsSend(std::string("I") + (char)1 + "\x10\x20\x30\x40");
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      localserver::Incoming in; CHECK(!localserver::poll(in), "an unpaired device is not listened to"); }
    { Client c; c.connect(); c.upgrade(host, origin); c.wsSend(helloMsg(99, "", "Old Page", "iPhone")); const std::string h = c.wsRecv(); CHECK(h.size() >= 2 && h[0] == 'h' && h[1] == 2, "a page of another version is told to reload"); }
    { Client c; c.connect(); c.upgrade(host, origin); c.wsSend(helloMsg(1, "", "X", "iPhone"), false); CHECK(c.wsRecv().empty(), "an unmasked frame ends the connection"); }

    // pairing
    std::string token;
    {
        Client c; c.connect(); c.upgrade(host, origin);
        c.wsSend(helloMsg(1, "", "Test\tPhone\n", "iPhone"));
        std::string h = c.wsRecv();
        CHECK(h.size() >= 2 && h[0] == 'h' && h[1] == 0, "an unknown device is asked for the code");
        const localserver::Status ps = waitFor([](const localserver::Status& s) { return !s.pairCode.empty(); });
        CHECK(ps.pairCode.size() == 6 && ps.pairName == "Test Phone", "the code and the device's name are shown in the sim");
        CHECK(h.find(ps.pairCode) == std::string::npos, "the code is not sent to the device");
        { Client o; o.connect(); o.upgrade(host, origin); o.wsSend(helloMsg(1, "", "Other", "iPad")); const std::string oh = o.wsRecv(); CHECK(oh.size() >= 2 && oh[1] == 3, "one device pairs at a time"); }
        std::string wrong = ps.pairCode; wrong[0] = wrong[0] == '9' ? '0' : (char)(wrong[0] + 1);
        std::string p = "P"; str(p, wrong); c.wsSend(p);
        h = c.wsRecv();
        CHECK(h.size() >= 2 && h[1] == 4 && textOf(h) == "4", "a wrong code: four tries left");
        CHECK(localserver::clients() == 0, "a wrong code pairs nothing");
        p = "P"; str(p, ps.pairCode); c.wsSend(p);
        h = c.wsRecv();
        CHECK(h.size() >= 2 && h[1] == 1 && textOf(h).size() == 64, "the right code: paired, and given its token");
        token = textOf(h);
        // the picture: nothing until a whole one; then the pieces after it, in order
        waitFor([](const localserver::Status&) { return localserver::clients() == 1 && localserver::wantKey(); });
        CHECK(localserver::wantKey(), "a device just connected waits for a whole picture");
        localserver::publishFrame("piece-0", false);
        localserver::publishFrame("WHOLE-1", true);
        localserver::publishFrame("piece-2", false);
        CHECK(c.wsRecv() == "WHOLE-1", "it is sent nothing before a whole picture");
        CHECK(c.wsRecv() == "piece-2", "and then the pieces that follow");
        CHECK(!waitFor([](const localserver::Status&) { return !localserver::wantKey(); }).address.empty() && !localserver::wantKey(), "after which no whole picture is waited for");
        // what the device does: a touch arrives as it was sent
        c.wsSend(std::string("I") + (char)0x11 + "\x10\x20\x30\x40");
        localserver::Incoming in;
        CHECK(pollFor(in) && in.kind == 'I' && in.bytes == std::string("\x11\x10\x20\x30\x40"), "a touch arrives");
        c.wsSend(std::string("B") + (char)82 + (char)1);
        const localserver::Status ds = waitFor([](const localserver::Status& s) { return s.devices.size() == 1 && s.devices[0].batteryPct == 81; });
        CHECK(ds.devices.size() == 1 && ds.devices[0].connected && ds.devices[0].name == "Test Phone" && ds.devices[0].kind == "iPhone" && ds.devices[0].charging && ds.pairCode.empty(), "the device is listed, with its battery");
        c.wsSend("I" + std::string(200, 'x'));
        CHECK(c.wsRecv().empty(), "an oversized message ends the connection");
    }
    { std::ifstream f(dir / "data" / "devices.txt"); std::stringstream ss; ss << f.rdbuf(); CHECK(!token.empty() && ss.str().find(token) == std::string::npos && ss.str().find("Test Phone") != std::string::npos, "the computer keeps the token's hash, not the token"); }
    {   // back again with the token: no code
        Client c; c.connect(); c.upgrade(host, origin);
        c.wsSend(helloMsg(1, token, "Test Phone", "iPhone"));
        const std::string h = c.wsRecv();
        CHECK(h.size() >= 2 && h[1] == 1 && textOf(h).empty(), "a paired device comes straight back in");
        CHECK(waitFor([](const localserver::Status& s) { return s.devices.size() == 1 && s.devices[0].connected; }).devices[0].lastSeen > 0, "it is seen again");
        localserver::forget("Test Phone");
        std::string m; do { m = c.wsRecv(); } while (!m.empty());
        CHECK(waitFor([](const localserver::Status& s) { return s.devices.empty(); }).devices.empty(), "Forget removes the device and ends its connection");
    }
    {   // its token is worth nothing now; declined in the sim
        Client c; c.connect(); c.upgrade(host, origin);
        c.wsSend(helloMsg(1, token, "Test Phone", "iPhone"));
        const std::string h = c.wsRecv();
        CHECK(h.size() >= 2 && h[1] == 0, "a forgotten device must pair again");
        waitFor([](const localserver::Status& s) { return !s.pairCode.empty(); });
        localserver::denyPairing();
        const std::string d = c.wsRecv();
        CHECK(d.size() >= 2 && d[1] == 3 && waitFor([](const localserver::Status& s) { return s.pairCode.empty(); }).pairCode.empty(), "pairing can be declined in the sim");
    }
    {   // five wrong codes: turned away, and that address waits
        Client c; c.connect(); c.upgrade(host, origin);
        c.wsSend(helloMsg(1, "", "Guesser", "Computer"));
        c.wsRecv();
        const localserver::Status ps = waitFor([](const localserver::Status& s) { return !s.pairCode.empty(); });
        std::string last;
        for (int i = 0; i < 5; ++i) { std::string p = "P"; str(p, ps.pairCode == "000000" ? "111111" : "000000"); c.wsSend(p); last = c.wsRecv(); }
        CHECK(last.size() >= 2 && last[1] == 3, "five wrong codes: turned away");
        Client c2; c2.connect(); c2.upgrade(host, origin);
        c2.wsSend(helloMsg(1, "", "Guesser", "Computer"));
        const std::string h = c2.wsRecv();
        CHECK(h.size() >= 2 && h[1] == 3 && localserver::status().pairCode.empty(), "and that address waits before it may try again");
    }
    localserver::stop();
    CHECK(localserver::status().state == 0, "stopped");
    { Client c; CHECK(!c.connect(), "nothing listens once it is off"); }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) { std::printf("localserver_test: %d failed\n", failures); return 1; }
    std::printf("localserver_test: ok\n");
    return 0;
}
