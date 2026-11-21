// Local Connect's server: the tablet's page and its live connection, for phones and tablets on the home network.
//
//   https://<this computer>:8443/     the page (the tablet's own screens, compiled for a browser)
//   wss://<this computer>:8443/ws     the connection: the sim's state out, what the user asked for back (src/localwire.h)
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
    std::string pagePath;          // the tablet's page, gzip-compressed
    int wireVersion = 1;           // a page of another version is told to reload
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

// What a paired device sent: 'A' what the user asked for (localwire's Actions), 'T' the settings as text, 'N' the notes as text
struct Incoming { int client = 0; char kind = 0; std::string bytes; };

void start(const Config& c);       // returns at once; status() tells how it went
void stop();                       // closes every connection and waits for the thread
Status status();
int clients();                     // paired devices connected now

void forget(const std::string& name);   // a paired device removed (and disconnected)
void denyPairing();                     // the device asking to pair is turned away

// The newest of each part of the state; each device is sent the parts that differ from what it has.
void publishSection(int section, const std::string& bytes);
// Texts kept the same on every device: 's' settings, 'n' notes, 'l' logbook. A device is not sent back the text it sent itself.
void publishText(char kind, const std::string& text);

bool poll(Incoming& in);           // the next thing a device sent, if any

}   // namespace localserver
}   // namespace xpt
