// Local Connect's server (see localserver.h). One thread: it accepts, does the TLS handshakes, answers the page and runs every
// device's connection, never waiting on any one of them (the sockets do not block; select() says which are ready).
#include "guard.h"
#include "localserver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/select.h>
#include <sys/stat.h>
#endif

#include "localnet.h"

namespace xpt {
namespace localserver {
namespace {

using Clock = std::chrono::steady_clock;
#if defined(_WIN32)
using SockT = SOCKET;
#else
using SockT = int;
#endif
namespace fs = std::filesystem;

constexpr std::size_t kMaxRequest = 8192;            // an HTTP request's headers
constexpr std::size_t kMaxMessage = 4096;            // one message from a device (a touch, a key: all small)
constexpr std::size_t kHoldBack = 768 * 1024;        // a device this far behind is sent nothing more until the next whole picture
constexpr int kMaxConnections = 16, kMaxDevices = 4, kMaxIncoming = 512;
constexpr int kPairTries = 5;
const std::chrono::seconds kHandshakeTime(10), kRequestTime(10), kPairTime(120), kLockout(60), kPingEvery(15), kSilence(45);

std::uint64_t fnv(const std::string& b) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : b) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// ---- what the plugin's thread and the server's share ----
struct Shared {
    std::mutex mx;
    Status status;
    std::deque<std::pair<std::string, bool>> frames;   // pieces of the picture to send (whole ones marked)
    bool wantKey = true;
    std::deque<Incoming> incoming;
    std::vector<std::string> forget;
    bool deny = false;
    int clients = 0;
    std::string theme = "{}";
};
Shared g;
std::thread g_thread;
std::atomic<bool> g_run{false};

std::string hex(const unsigned char* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}
std::string sha256Hex(const std::string& s) {
    unsigned char out[32];
    mbedtls_sha256((const unsigned char*)s.data(), s.size(), out, 0);
    return hex(out, 32);
}
bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}
bool writeFile(const fs::path& p, const std::string& data, bool secret) {
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) return false;
    }
#if !defined(_WIN32)
    if (secret) chmod(p.c_str(), 0600);   // the key: this user only
#else
    (void)secret;
#endif
    return true;
}
std::string lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
// a device's name as it may be shown and stored: printable, one line, not long
std::string cleanName(const std::string& s, std::size_t max) {
    std::string o;
    for (unsigned char c : s) { if (c == '\t' || c == '\n' || c == '\r') c = ' '; if (c >= 32 && c != 127) o.push_back((char)c); if (o.size() >= max) break; }
    while (!o.empty() && (unsigned char)o.back() >= 0x80 && (o.back() & 0xC0) != 0x80) o.pop_back();   // (no UTF-8 character cut in half at the end)
    o = trim(o);
    return o;
}

// a little reader for the messages' own fields (lengths checked against what is there)
struct In {
    const unsigned char* p;
    const unsigned char* e;
    bool ok = true;
    explicit In(const std::string& s, std::size_t from) : p((const unsigned char*)s.data() + std::min(from, s.size())), e((const unsigned char*)s.data() + s.size()) {}
    std::uint64_t var() {
        std::uint64_t n = 0;
        for (int sh = 0; sh < 64; sh += 7) {
            if (p >= e) { ok = false; return 0; }
            const unsigned char c = *p++;
            n |= (std::uint64_t)(c & 0x7F) << sh;
            if (!(c & 0x80)) return n;
        }
        ok = false;
        return 0;
    }
    std::string str(std::size_t max) {
        const std::uint64_t n = var();
        if (!ok || n > (std::uint64_t)(e - p) || n > max) { ok = false; return std::string(); }
        std::string s((const char*)p, (std::size_t)n);
        p += n;
        return s;
    }
};
void putVar(std::string& o, std::uint64_t n) { while (n >= 0x80) { o.push_back((char)(n | 0x80)); n >>= 7; } o.push_back((char)n); }
void putStr(std::string& o, const std::string& s) { putVar(o, s.size()); o += s; }

struct Paired { std::string hash, name, kind; long long lastSeen = 0; };

struct Conn {
    int id = 0;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    std::string ip;
    enum State { HANDSHAKE, HTTP, SENDING, WS, DEAD } st = HANDSHAKE;
    Clock::time_point deadline;
    std::string in;
    std::deque<std::string> out;   // (chunks: the one being written stays where it is while more are added)
    std::size_t outOff = 0, outBytes = 0;
    bool closeAfterSend = false, wantWrite = false;
    // the WebSocket
    std::string msg;
    bool msgOpen = false;
    bool authed = false;
    std::string hash, name, kind;
    int battery = -1;
    bool charging = false;
    bool needKey = true;           // nothing is sent to it until the next whole picture
    Clock::time_point lastPing, lastHeard;
    Conn() { mbedtls_net_init(&net); mbedtls_ssl_init(&ssl); }
    ~Conn() { mbedtls_ssl_free(&ssl); mbedtls_net_free(&net); }
};

struct Server {
    Config cfg;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_net_context listener;
    int port = 0;
    std::string page, pageTag;
    std::vector<std::string> hosts;   // the names this server answers to (its own address and port)
    std::list<std::unique_ptr<Conn>> conns;
    int nextId = 1;
    std::vector<Paired> paired;
    struct Pending { int conn = 0; std::string code, name, kind; Clock::time_point expires; int tries = 0; } pending;
    std::map<std::string, Clock::time_point> lockout;   // by address
    bool dirty = true;

    Server() {
        mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&drbg); mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&cert); mbedtls_pk_init(&key); mbedtls_net_init(&listener);
    }
    ~Server() {
        conns.clear();
        mbedtls_net_free(&listener); mbedtls_pk_free(&key); mbedtls_x509_crt_free(&cert);
        mbedtls_ssl_config_free(&conf); mbedtls_ctr_drbg_free(&drbg); mbedtls_entropy_free(&entropy);
    }

    void random(unsigned char* out, std::size_t n) { if (mbedtls_ctr_drbg_random(&drbg, out, n) != 0) std::abort(); }   // (no randomness: nothing safe can be done)

    // ---- the key and certificate: made here once, kept, made again when the address changes or the certificate nears its end ----
    bool makeCertificate(std::string& certPem, std::string& keyPem, long long& until, std::string& err) {
        mbedtls_pk_context k;
        mbedtls_x509write_cert w;
        mbedtls_pk_init(&k); mbedtls_x509write_crt_init(&w);
        bool ok = false;
        do {
            if (mbedtls_pk_setup(&k, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) { err = "could not make a key"; break; }
            if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(k), mbedtls_ctr_drbg_random, &drbg) != 0) { err = "could not make a key"; break; }
            mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
            mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
            mbedtls_x509write_crt_set_subject_key(&w, &k);
            mbedtls_x509write_crt_set_issuer_key(&w, &k);
            if (mbedtls_x509write_crt_set_subject_name(&w, "CN=XPTablet Local Connect,O=XPTablet") != 0 ||
                mbedtls_x509write_crt_set_issuer_name(&w, "CN=XPTablet Local Connect,O=XPTablet") != 0) { err = "could not make a certificate"; break; }
            unsigned char serial[16];
            random(serial, sizeof serial);
            serial[0] = (unsigned char)((serial[0] & 0x7F) | 0x01);
            if (mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial) != 0) { err = "could not make a certificate"; break; }
            const std::time_t now = std::time(nullptr), from = now - 86400, to = now + 86400LL * 730;
            char nb[20], na[20];
            std::tm tf{}, tt{};
#if defined(_WIN32)
            gmtime_s(&tf, &from); gmtime_s(&tt, &to);
#else
            gmtime_r(&from, &tf); gmtime_r(&to, &tt);
#endif
            std::strftime(nb, sizeof nb, "%Y%m%d%H%M%S", &tf); std::strftime(na, sizeof na, "%Y%m%d%H%M%S", &tt);
            until = (long long)to;
            if (mbedtls_x509write_crt_set_validity(&w, nb, na) != 0) { err = "could not make a certificate"; break; }
            mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1);
            mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
            unsigned b[4] = {0, 0, 0, 0};
            unsigned char ip4[4];
            if (std::sscanf(cfg.bindIp.c_str(), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) == 4) {   // the address, as the name the certificate is for
                for (int i = 0; i < 4; ++i) ip4[i] = (unsigned char)b[i];
                mbedtls_x509_san_list san;
                std::memset(&san, 0, sizeof san);
                san.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
                san.node.san.unstructured_name.p = ip4;
                san.node.san.unstructured_name.len = 4;
                san.next = nullptr;
                mbedtls_x509write_crt_set_subject_alternative_name(&w, &san);
            }
            std::vector<unsigned char> buf(8192);
            if (mbedtls_x509write_crt_pem(&w, buf.data(), buf.size(), mbedtls_ctr_drbg_random, &drbg) != 0) { err = "could not make a certificate"; break; }
            certPem = (const char*)buf.data();
            if (mbedtls_pk_write_key_pem(&k, buf.data(), buf.size()) != 0) { err = "could not write the key"; break; }
            keyPem = (const char*)buf.data();
            ok = true;
        } while (false);
        mbedtls_x509write_crt_free(&w); mbedtls_pk_free(&k);
        return ok;
    }

    bool loadIdentity(std::string& err) {
        const fs::path dir = fs::u8path(cfg.dataDir);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string certPem, keyPem, meta;
        bool have = readFile(dir / "cert.pem", certPem) && readFile(dir / "key.pem", keyPem) && readFile(dir / "cert.txt", meta);
        if (have) {   // cert.txt: the address it was made for, and when it ends
            std::istringstream ms(meta);
            std::string ip;
            long long until = 0;
            ms >> ip >> until;
            if (ip != cfg.bindIp || until < (long long)std::time(nullptr) + 86400LL * 30) have = false;
        }
        if (have) {
            have = mbedtls_x509_crt_parse(&cert, (const unsigned char*)certPem.c_str(), certPem.size() + 1) == 0 &&
                   mbedtls_pk_parse_key(&key, (const unsigned char*)keyPem.c_str(), keyPem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &drbg) == 0;
            if (!have) { mbedtls_x509_crt_free(&cert); mbedtls_x509_crt_init(&cert); mbedtls_pk_free(&key); mbedtls_pk_init(&key); }
        }
        if (!have) {
            long long until = 0;
            if (!makeCertificate(certPem, keyPem, until, err)) return false;
            if (!writeFile(dir / "key.pem", keyPem, true) || !writeFile(dir / "cert.pem", certPem, false) ||
                !writeFile(dir / "cert.txt", cfg.bindIp + " " + std::to_string(until) + "\n", false)) { err = "could not write to " + cfg.dataDir; return false; }
            if (mbedtls_x509_crt_parse(&cert, (const unsigned char*)certPem.c_str(), certPem.size() + 1) != 0 ||
                mbedtls_pk_parse_key(&key, (const unsigned char*)keyPem.c_str(), keyPem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &drbg) != 0) { err = "could not read the certificate back"; return false; }
        }
        return true;
    }

    // ---- the paired devices (devices.txt: the token's SHA-256, when last seen, the kind, the name) ----
    void loadPaired() {
        std::string text;
        if (!readFile(fs::u8path(cfg.dataDir) / "devices.txt", text)) return;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            std::vector<std::string> f;
            std::size_t a = 0;
            for (int i = 0; i < 3; ++i) { const std::size_t t = line.find('\t', a); if (t == std::string::npos) break; f.push_back(line.substr(a, t - a)); a = t + 1; }
            if (f.size() != 3) continue;
            Paired p;
            p.hash = f[0]; p.lastSeen = std::atoll(f[1].c_str()); p.kind = cleanName(f[2], 24); p.name = cleanName(line.substr(a), 40);
            if (p.hash.size() == 64 && !p.name.empty() && paired.size() < 32) paired.push_back(p);
        }
    }
    void savePaired() {
        std::string text;
        for (const Paired& p : paired) text += p.hash + "\t" + std::to_string(p.lastSeen) + "\t" + p.kind + "\t" + p.name + "\n";
        writeFile(fs::u8path(cfg.dataDir) / "devices.txt", text, true);
    }
    Paired* findPaired(const std::string& hash) { for (Paired& p : paired) if (p.hash == hash) return &p; return nullptr; }

    // ---- starting ----
    bool begin(std::string& err) {
        const char* pers = "xptablet-local-connect";
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, std::strlen(pers)) != 0) { err = "the computer gave no randomness"; return false; }
        if (!loadIdentity(err)) return false;
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) { err = "TLS could not be set up"; return false; }
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);   // (devices prove themselves by pairing, not by a certificate)
        if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0) { err = "the certificate could not be used"; return false; }
        loadPaired();
        page = cfg.page;
        pageTag = "\"" + std::to_string(page.size()) + "-" + std::to_string((unsigned long long)fnv(page)) + "\"";
        int rc = -1;
        for (int p = cfg.port; p < cfg.port + 10; ++p) {
            rc = mbedtls_net_bind(&listener, cfg.bindIp.c_str(), std::to_string(p).c_str(), MBEDTLS_NET_PROTO_TCP);
            if (rc == 0) { port = p; break; }
            mbedtls_net_free(&listener); mbedtls_net_init(&listener);
        }
        if (rc != 0) { err = "ports " + std::to_string(cfg.port) + " to " + std::to_string(cfg.port + 9) + " are all taken on " + cfg.bindIp; return false; }
        mbedtls_net_set_nonblock(&listener);
        hosts.push_back(cfg.bindIp + ":" + std::to_string(port));
        if (cfg.allowLoopback) { hosts.push_back("127.0.0.1:" + std::to_string(port)); hosts.push_back("localhost:" + std::to_string(port)); }
        return true;
    }

    // ---- sending ----
    void queue(Conn& c, std::string bytes) { c.outBytes += bytes.size(); c.out.push_back(std::move(bytes)); }
    void frame(Conn& c, int op, const std::string& data) {
        std::string f;
        f.reserve(data.size() + 10);
        f.push_back((char)(0x80 | op));
        if (data.size() < 126) f.push_back((char)data.size());
        else if (data.size() < 65536) { f.push_back(126); f.push_back((char)(data.size() >> 8)); f.push_back((char)data.size()); }
        else { f.push_back(127); for (int i = 7; i >= 0; --i) f.push_back((char)((std::uint64_t)data.size() >> (8 * i))); }
        f += data;
        queue(c, std::move(f));
    }
    void hello(Conn& c, int status, const std::string& text) { std::string m = "h"; m.push_back((char)status); putStr(m, text); frame(c, 2, m); }
    void flush(Conn& c) {
        c.wantWrite = false;
        while (!c.out.empty() && c.st != Conn::DEAD) {
            const std::string& f = c.out.front();
            const int n = mbedtls_ssl_write(&c.ssl, (const unsigned char*)f.data() + c.outOff, f.size() - c.outOff);
            if (n > 0) {
                c.outOff += (std::size_t)n; c.outBytes -= (std::size_t)n;
                if (c.outOff >= f.size()) { c.out.pop_front(); c.outOff = 0; }
            } else if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) { c.wantWrite = true; break; }
            else { c.st = Conn::DEAD; break; }
        }
        if (c.out.empty() && c.closeAfterSend && c.st != Conn::DEAD) { mbedtls_ssl_close_notify(&c.ssl); c.st = Conn::DEAD; }
    }
    void readSome(Conn& c, std::size_t cap) {
        unsigned char buf[16384];
        for (int i = 0; i < 64 && c.st != Conn::DEAD; ++i) {
            const int n = mbedtls_ssl_read(&c.ssl, buf, sizeof buf);
            if (n > 0) { c.in.append((const char*)buf, (std::size_t)n); c.lastHeard = Clock::now(); if (c.in.size() > cap) c.st = Conn::DEAD; }
            else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) break;
            else { c.st = Conn::DEAD; break; }   // (closed, or an error)
        }
    }

    // ---- the page ----
    void respond(Conn& c, const char* status, const std::string& headers, const std::string& body) {
        std::string r = std::string("HTTP/1.1 ") + status + "\r\n" + headers +
                        "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n\r\n";
        queue(c, std::move(r));
        if (!body.empty()) queue(c, body);
        c.closeAfterSend = true;
        c.st = Conn::SENDING;
    }
    int wsCount() const { int n = 0; for (const auto& c : conns) if (c->st == Conn::WS) ++n; return n; }
    void http(Conn& c) {
        const std::size_t end = c.in.find("\r\n\r\n");
        if (end == std::string::npos) return;
        std::istringstream ss(c.in.substr(0, end));
        std::string line, method, path;
        std::getline(ss, line);
        { std::istringstream ls(line); ls >> method >> path; }
        std::map<std::string, std::string> h;
        while (std::getline(ss, line)) { const std::size_t colon = line.find(':'); if (colon != std::string::npos) h[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1)); }
        c.in.erase(0, end + 4);
        // only when asked for by its own address (a page elsewhere made to point here is refused)
        const std::string host = lower(h["host"]);
        if (std::find(hosts.begin(), hosts.end(), host) == hosts.end()) { respond(c, "421 Misdirected Request", "Content-Type: text/plain\r\n", "Not this server.\n"); return; }
        if (method != "GET") { respond(c, "405 Method Not Allowed", "Allow: GET\r\nContent-Type: text/plain\r\n", "GET only.\n"); return; }
        const std::size_t q = path.find('?');
        if (q != std::string::npos) path.resize(q);
        if (path == "/ws") {
            const std::string k = h["sec-websocket-key"];
            if (lower(h["upgrade"]) != "websocket" || k.size() < 16 || k.size() > 64 || h["sec-websocket-version"] != "13" || lower(h["origin"]) != "https://" + host) {
                respond(c, "400 Bad Request", "Content-Type: text/plain\r\n", "Not the tablet's page.\n"); return;
            }
            if (wsCount() >= kMaxDevices + 2) { respond(c, "503 Service Unavailable", "Content-Type: text/plain\r\n", "Too many devices.\n"); return; }
            const std::string cat = k + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            unsigned char sha[20], b64[64];
            std::size_t n = 0;
            mbedtls_sha1((const unsigned char*)cat.data(), cat.size(), sha);
            mbedtls_base64_encode(b64, sizeof b64, &n, sha, sizeof sha);
            queue(c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + std::string((const char*)b64, n) + "\r\n\r\n");
            c.st = Conn::WS;
            c.lastPing = c.lastHeard = Clock::now();
            return;
        }
        if (path == "/theme") {
            std::string t;
            { std::lock_guard<std::mutex> lk(g.mx); t = g.theme; }
            respond(c, "200 OK", "Content-Type: application/json\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n", t);
            return;
        }
        if (path == "/" || path == "/index.html") {
            if (page.empty()) {
                respond(c, "200 OK", "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n",
                        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>XPTablet</title>"
                        "<body style='font:16px sans-serif;background:#0b1220;color:#e8eefc;padding:24px'><h3>XPTablet Local Connect</h3>"
                        "<p>The tablet's page is missing from this build of XPTablet.</p>");
                return;
            }
            const std::string common = "ETag: " + pageTag + "\r\nCache-Control: no-cache\r\n";
            if (h["if-none-match"] == pageTag) { respond(c, "304 Not Modified", common, ""); return; }
            respond(c, "200 OK", common + "Content-Type: text/html; charset=utf-8\r\nX-Frame-Options: DENY\r\n"
                    "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data: blob:; "
                    "connect-src 'self' wss:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n", page);
            return;
        }
        respond(c, "404 Not Found", "Content-Type: text/plain\r\n", "Not found.\n");
    }

    // ---- pairing and messages ----
    std::string uniqueName(const std::string& want) {
        std::string name = want;
        for (int n = 2; n < 100; ++n) {
            bool taken = false;
            for (const Paired& p : paired) taken = taken || p.name == name;
            if (!taken) break;
            name = want + " " + std::to_string(n);
        }
        return name;
    }
    int authedCount() const { int n = 0; for (const auto& c : conns) if (c->st == Conn::WS && c->authed) ++n; return n; }
    void admit(Conn& c, Paired& p) {
        c.authed = true; c.needKey = true; c.hash = p.hash; c.name = p.name; c.kind = p.kind;
        p.lastSeen = (long long)std::time(nullptr);
        savePaired();
        dirty = true;
    }
    void message(Conn& c, const std::string& m) {
        if (m.empty()) { c.st = Conn::DEAD; return; }
        const char type = m[0];
        In in(m, 1);
        const Clock::time_point now = Clock::now();
        if (type == 'H') {
            if (c.authed) return;
            const int version = (int)in.var();
            const std::string token = in.str(128), name = cleanName(in.str(256), 32), kind = cleanName(in.str(64), 24);
            if (!in.ok) { c.st = Conn::DEAD; return; }
            if (version != cfg.wireVersion) { hello(c, 2, "Reload the page: XPTablet was updated."); c.closeAfterSend = true; return; }
            if (!token.empty()) {
                if (Paired* p = findPaired(sha256Hex(token))) {
                    if (authedCount() >= kMaxDevices) { hello(c, 3, "Too many devices are connected."); c.closeAfterSend = true; return; }
                    admit(c, *p);
                    hello(c, 1, "");
                    return;
                }
            }
            // not paired (or forgotten): the code goes on the sim's screen, the device asks its user for it
            const auto lk = lockout.find(c.ip);
            if (lk != lockout.end() && lk->second > now) { hello(c, 3, "Too many wrong codes. Wait a minute, then reload."); c.closeAfterSend = true; return; }
            if (pending.conn != 0 && pending.conn != c.id) { hello(c, 3, "Another device is pairing. Try again in a moment."); c.closeAfterSend = true; return; }
            if ((int)paired.size() >= 32) { hello(c, 3, "Too many paired devices. Forget one in XPTablet's Settings."); c.closeAfterSend = true; return; }
            std::uint32_t v = 0;
            do { random((unsigned char*)&v, sizeof v); } while (v >= 4294000000u);   // (evenly over the million codes)
            char code[8];
            std::snprintf(code, sizeof code, "%06u", (unsigned)(v % 1000000u));
            pending.conn = c.id; pending.code = code; pending.name = name.empty() ? std::string("A device") : name; pending.kind = kind.empty() ? std::string("Device") : kind;
            pending.expires = now + kPairTime; pending.tries = 0;
            dirty = true;
            hello(c, 0, "");
            return;
        }
        if (type == 'P') {
            if (c.authed || pending.conn != c.id) return;
            const std::string code = in.str(16);
            unsigned diff = code.size() == pending.code.size() ? 0u : 1u;   // (compared in full, whatever differs)
            for (std::size_t i = 0; i < pending.code.size(); ++i) diff |= (unsigned)(pending.code[i] ^ (i < code.size() ? code[i] : 0));
            if (!in.ok || diff != 0) {
                if (++pending.tries >= kPairTries) {
                    lockout[c.ip] = now + kLockout;
                    pending = Pending(); dirty = true;
                    hello(c, 3, "Too many wrong codes. Wait a minute, then reload.");
                    c.closeAfterSend = true;
                } else { std::string m2 = "h"; m2.push_back(4); putStr(m2, std::to_string(kPairTries - pending.tries)); frame(c, 2, m2); }
                return;
            }
            if (authedCount() >= kMaxDevices) { pending = Pending(); dirty = true; hello(c, 3, "Too many devices are connected."); c.closeAfterSend = true; return; }
            unsigned char raw[32];
            random(raw, sizeof raw);
            const std::string token = hex(raw, sizeof raw);
            Paired p;
            p.hash = sha256Hex(token); p.name = uniqueName(pending.name); p.kind = pending.kind;
            paired.push_back(p);
            pending = Pending();
            admit(c, paired.back());
            hello(c, 1, token);   // (the one time the token is sent: the device keeps it)
            return;
        }
        if (!c.authed) return;   // (nothing else is listened to before pairing)
        if (type == 'B') {
            if (m.size() >= 3) { const int b = (unsigned char)m[1]; c.battery = b == 0 ? -1 : std::min(100, b - 1); c.charging = m[2] != 0; dirty = true; }
            return;
        }
        if (type == 'I') {   // a touch, a scroll, a key
            if (m.size() > 64) { c.st = Conn::DEAD; return; }
            Incoming inc;
            inc.client = c.id; inc.kind = type; inc.bytes = m.substr(1);
            std::lock_guard<std::mutex> lk(g.mx);
            if ((int)g.incoming.size() < kMaxIncoming) g.incoming.push_back(std::move(inc));
            return;
        }
    }
    void frames(Conn& c) {
        std::size_t at = 0;
        while (c.st == Conn::WS && c.in.size() - at >= 2) {
            const unsigned char b0 = (unsigned char)c.in[at], b1 = (unsigned char)c.in[at + 1];
            const int op = b0 & 0x0F;
            const bool fin = (b0 & 0x80) != 0;
            if ((b0 & 0x70) || !(b1 & 0x80)) { c.st = Conn::DEAD; break; }   // (no extensions; a browser always masks)
            std::uint64_t len = b1 & 0x7F;
            std::size_t pos = at + 2;
            if (len == 126) { if (c.in.size() - at < 4) break; len = ((std::uint64_t)(unsigned char)c.in[at + 2] << 8) | (unsigned char)c.in[at + 3]; pos = at + 4; }
            else if (len == 127) { if (c.in.size() - at < 10) break; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | (unsigned char)c.in[at + 2 + i]; pos = at + 10; }
            if (len > kMaxMessage) { c.st = Conn::DEAD; break; }
            if (c.in.size() - pos < 4 + len) break;
            std::string payload(c.in, pos + 4, (std::size_t)len);
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = (char)(payload[i] ^ c.in[pos + (i & 3)]);
            at = pos + 4 + (std::size_t)len;
            if (op >= 8) {
                if (!fin || len > 125) { c.st = Conn::DEAD; break; }
                if (op == 8) { frame(c, 8, payload.substr(0, std::min<std::size_t>(2, payload.size()))); c.closeAfterSend = true; }
                else if (op == 9) frame(c, 10, payload);
                continue;
            }
            if (op == 0 ? !c.msgOpen : (c.msgOpen || (op != 1 && op != 2))) { c.st = Conn::DEAD; break; }
            if (c.msg.size() + payload.size() > kMaxMessage) { c.st = Conn::DEAD; break; }
            c.msg += payload;
            c.msgOpen = !fin;
            if (fin) { std::string m; m.swap(c.msg); message(c, m); }
        }
        if (at) c.in.erase(0, at);
    }

    // the picture's new pieces, to every paired device that is keeping up; one that is not waits for the next whole picture
    void distribute() {
        std::deque<std::pair<std::string, bool>> frames;
        { std::lock_guard<std::mutex> lk(g.mx); frames.swap(g.frames); }
        bool want = false;
        for (auto& cp : conns) {
            Conn& c = *cp;
            if (!c.authed || c.st != Conn::WS || c.closeAfterSend) continue;
            for (const auto& f : frames) {
                if (c.needKey && !f.second) continue;
                if (c.outBytes > kHoldBack) { c.needKey = true; continue; }
                frame(c, 2, f.first);
                c.needKey = false;
            }
            want = want || c.needKey;
        }
        std::lock_guard<std::mutex> lk(g.mx);
        g.wantKey = want;
    }

    void step(Conn& c, Clock::time_point now) {
        if (c.st == Conn::HANDSHAKE) {
            const int r = mbedtls_ssl_handshake(&c.ssl);
            if (r == 0) { c.st = Conn::HTTP; c.deadline = now + kRequestTime; }
            else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) c.wantWrite = true;
            else if (r != MBEDTLS_ERR_SSL_WANT_READ) c.st = Conn::DEAD;
            if (c.st == Conn::HANDSHAKE && now > c.deadline) c.st = Conn::DEAD;
            if (c.st != Conn::HTTP) return;
        }
        if (c.st == Conn::HTTP) {
            readSome(c, kMaxRequest);
            if (c.st == Conn::HTTP) http(c);
            if (c.st == Conn::HTTP && now > c.deadline) c.st = Conn::DEAD;
        }
        if (c.st == Conn::WS) {
            readSome(c, kMaxMessage + 64);
            if (c.st == Conn::WS) frames(c);
            if (c.st == Conn::WS) {
                if (now - c.lastHeard > kSilence) c.st = Conn::DEAD;
                else if (now - c.lastPing > kPingEvery) { c.lastPing = now; frame(c, 9, ""); }
            }
        }
        if (c.st == Conn::SENDING || c.st == Conn::WS) flush(c);
        if (c.st == Conn::SENDING && now > c.deadline + std::chrono::seconds(60)) c.st = Conn::DEAD;   // (a page that is never taken)
    }

    void acceptAll(Clock::time_point now) {
        for (int i = 0; i < 8; ++i) {
            auto c = std::make_unique<Conn>();
            unsigned char ip[16];
            std::size_t ipLen = 0;
            if (mbedtls_net_accept(&listener, &c->net, ip, sizeof ip, &ipLen) != 0) break;
            if (ipLen != 4) continue;
            char text[32];
            std::snprintf(text, sizeof text, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            c->ip = text;
            const bool loopback = ip[0] == 127;
            if (!(isPrivateIPv4(c->ip) || (loopback && cfg.allowLoopback))) continue;   // (the home network only: the socket is closed unanswered)
            if ((int)conns.size() >= kMaxConnections) continue;
            mbedtls_net_set_nonblock(&c->net);
            if (mbedtls_ssl_setup(&c->ssl, &conf) != 0) continue;
            mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send, mbedtls_net_recv, nullptr);
            c->id = nextId++;
            c->deadline = now + kHandshakeTime;
            c->lastHeard = now;
            conns.push_back(std::move(c));
        }
    }

    void commands(Clock::time_point now) {
        std::vector<std::string> forget;
        bool deny = false;
        { std::lock_guard<std::mutex> lk(g.mx); forget.swap(g.forget); deny = g.deny; g.deny = false; }
        for (const std::string& name : forget) {
            for (auto it = paired.begin(); it != paired.end();) {
                if (it->name != name) { ++it; continue; }
                for (auto& c : conns) if (c->authed && c->hash == it->hash) c->st = Conn::DEAD;
                it = paired.erase(it);
            }
            savePaired();
            dirty = true;
        }
        if (pending.conn != 0 && (deny || now > pending.expires)) {
            for (auto& c : conns) if (c->id == pending.conn && c->st == Conn::WS) { hello(*c, 3, deny ? "Pairing was declined in XPTablet." : "The code ran out. Reload to try again."); c->closeAfterSend = true; }
            pending = Pending();
            dirty = true;
        }
        for (auto it = lockout.begin(); it != lockout.end();) it = it->second <= now ? lockout.erase(it) : std::next(it);
    }

    void publishStatus() {
        Status s;
        s.state = 2;
        s.address = "https://" + cfg.bindIp + ":" + std::to_string(port);
        if (pending.conn != 0) { s.pairCode = pending.code; s.pairName = pending.name; }
        int n = 0;
        for (const Paired& p : paired) {
            Device d;
            d.name = p.name; d.kind = p.kind; d.lastSeen = p.lastSeen;
            for (const auto& c : conns) if (c->st == Conn::WS && c->authed && c->hash == p.hash) { d.connected = true; d.batteryPct = c->battery; d.charging = c->charging; }
            if (d.connected) ++n;
            s.devices.push_back(d);
        }
        std::lock_guard<std::mutex> lk(g.mx);
        g.status = s;
        g.clients = n;
    }

    void run() {
        Clock::time_point lastStatus = Clock::now();
        while (g_run.load()) {
            fd_set rd, wr;
            FD_ZERO(&rd); FD_ZERO(&wr);
            int maxFd = listener.fd;
            FD_SET((SockT)listener.fd, &rd);
            for (const auto& c : conns) {
                FD_SET((SockT)c->net.fd, &rd);
                if (c->wantWrite || !c->out.empty()) FD_SET((SockT)c->net.fd, &wr);
                maxFd = std::max(maxFd, c->net.fd);
            }
            timeval tv;
            tv.tv_sec = 0; tv.tv_usec = 20000;
            select(maxFd + 1, &rd, &wr, nullptr, &tv);
            const Clock::time_point now = Clock::now();
            acceptAll(now);
            distribute();
            for (auto& c : conns) step(*c, now);
            commands(now);
            for (auto it = conns.begin(); it != conns.end();) {
                Conn& c = **it;
                if (c.st != Conn::DEAD) { ++it; continue; }
                if (c.authed) { if (Paired* p = findPaired(c.hash)) { p->lastSeen = (long long)std::time(nullptr); savePaired(); } dirty = true; }
                if (pending.conn == c.id) { pending = Pending(); dirty = true; }   // (it left before pairing: the code goes with it)
                it = conns.erase(it);
            }
            if (dirty || now - lastStatus > std::chrono::milliseconds(500)) { publishStatus(); dirty = false; lastStatus = now; }
        }
        for (auto& c : conns) if (c->st == Conn::WS) { frame(*c, 8, ""); flush(*c); }
        for (auto& c : conns) if (c->authed) if (Paired* p = findPaired(c->hash)) p->lastSeen = (long long)std::time(nullptr);
        savePaired();
    }
};

void threadMain(Config cfg) {
    auto srv = std::make_unique<Server>();
    srv->cfg = cfg;
    std::string err;
    if (!srv->begin(err)) {
        std::lock_guard<std::mutex> lk(g.mx);
        g.status = Status();
        g.status.state = -1;
        g.status.error = err;
        return;
    }
    srv->run();
}

}   // namespace

void start(const Config& c) {
    stop();
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.status = Status();
        g.status.state = 1;
        g.incoming.clear(); g.forget.clear(); g.frames.clear(); g.wantKey = true; g.deny = false; g.clients = 0;
    }
    g_run.store(true);
    g_thread = std::thread(guarded("Local Connect server", [c]() { threadMain(c); }));
}

void stop() {
    g_run.store(false);
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> lk(g.mx);
    g.status = Status();
    g.clients = 0;
    g.incoming.clear();
}

Status status() { std::lock_guard<std::mutex> lk(g.mx); return g.status; }
int clients() { std::lock_guard<std::mutex> lk(g.mx); return g.clients; }
void forget(const std::string& name) { std::lock_guard<std::mutex> lk(g.mx); if (g.forget.size() < 64) g.forget.push_back(name); }
void denyPairing() { std::lock_guard<std::mutex> lk(g.mx); g.deny = true; }

void publishFrame(const std::string& message, bool key) {
    std::lock_guard<std::mutex> lk(g.mx);
    if (g.clients == 0) { g.frames.clear(); return; }
    if (key) g.frames.clear();   // (a whole picture replaces whatever was waiting)
    if (g.frames.size() < 12) g.frames.emplace_back(message, key);
}
bool wantKey() { std::lock_guard<std::mutex> lk(g.mx); return g.wantKey; }
void setTheme(const std::string& json) { std::lock_guard<std::mutex> lk(g.mx); g.theme = json; }

bool poll(Incoming& in) {
    std::lock_guard<std::mutex> lk(g.mx);
    if (g.incoming.empty()) return false;
    in = std::move(g.incoming.front());
    g.incoming.pop_front();
    return true;
}

}   // namespace localserver
}   // namespace xpt
