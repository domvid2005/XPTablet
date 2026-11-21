// Local Connect's server: the tablet's page and its live connection, for phones and tablets on the home network.
//
//   https://<this computer>:8443/     the page (a small one: it shows the tablet's picture and sends touches)
//   wss://<this computer>:8443/ws     the connection: the tablet's picture out as it changes, touches and keys back (src/localmirror.h)
//
// How it is kept safe:
//   * off until switched on in Settings, and it listens only on the computer's home-network address;
//   * only private addresses are answered (192.168.x, 10.x, 172.16-31.x, 169.254.x): nothing from the internet;
//   * everything is encrypted (TLS 1.2, a key and certificate made on this computer and kept in its data folder);
//   * a device must be paired: the server shows a 6-digit code in the sim only, the device's user types it in; five wrong tries
//     and that address waits a minute; a code lasts two minutes;
//   * a paired device holds a random 256-bit token; the computer keeps only its SHA-256; Forget removes it;
//   * the connection is refused unless it comes from the page itself (the Host and Origin headers must be this server's own);
//   * message sizes, connection counts and time limits are all bounded.
//
// One thread does all the network work; the plugin's side of these functions never waits on the network.
#pragma once
#include <string>
#include <vector>

namespace xpt {
namespace localserver {

struct Config {
    std::string bindIp;            // the address to listen on (the computer's on the home network)
    int port = 8443;               // tried first, then the next few if it is taken
    std::string dataDir;           // where the key, the certificate and the paired devices are kept (made if missing)
    std::string page;              // the page itself (HTML)
    int wireVersion = 1;           // the page's version: a page of another one (kept open across an update) is told to reload
    bool allowLoopback = false;    // the tests: 127.0.0.1 too
};

struct Device {
    std::string name, kind;        // "Dom's iPhone", "iPhone"
    bool connected = false;
    int batteryPct = -1;           // -1 not told
    bool charging = false;
    long long lastSeen = 0;        // seconds since 1970 (the computer's clock), 0 never
};

struct Status {
    int state = 0;                 // 0 off, 1 starting, 2 on, -1 could not start (error says why)
    std::string address, error;    // https://192.168.1.20:8443
    std::string pairCode, pairName;   // a device asking to pair: the code to show in the sim, and its name
    std::vector<Device> devices;
};

// What a paired device sent: 'I' a touch, a scroll or a key (src/localmirror.h reads them)
struct Incoming { int client = 0; char kind = 0; std::string bytes; };

void start(const Config& c);       // returns at once; status() tells how it went
void stop();                       // closes every connection and waits for the thread
Status status();
int clients();                     // paired devices connected now

void forget(const std::string& name);   // a paired device removed (and disconnected)
void denyPairing();                     // the device asking to pair is turned away

// A piece of the tablet's picture, to every paired device. key: it is a whole picture (a device that fell behind, or has just
// connected, is sent nothing until the next whole one). wantKey(): some device is waiting for a whole picture.
void publishFrame(const std::string& message, bool key);
bool wantKey();

bool poll(Incoming& in);           // the next thing a device sent, if any

}   // namespace localserver
}   // namespace xpt
