#include "localnet.h"
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace xpt {

std::string localIPv4() {
#if defined(_WIN32)
    static bool started = false;
    if (!started) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return std::string(); started = true; }
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) return std::string();
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return std::string();
#endif
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);   // (a documentation address: never reached, a UDP "connect" sends nothing)
    std::string out;
    if (connect(fd, (const sockaddr*)&to, sizeof to) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (getsockname(fd, (sockaddr*)&me, &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf)) out = buf;
        }
    }
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
    if (out == "0.0.0.0" || out.compare(0, 4, "127.") == 0) out.clear();
    return out;
}

bool isPrivateIPv4(const std::string& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return false;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

}  // namespace xpt
