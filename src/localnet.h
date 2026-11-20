#pragma once
#include <string>
namespace xpt {
// This computer's address on the home network (the IPv4 address its traffic leaves from, e.g. "192.168.1.20"), empty if none.
// Nothing is sent: a UDP socket is pointed at an outside address only to see which local address the system would use.
std::string localIPv4();
// A private address (10.x, 172.16-31.x, 192.168.x, 169.254.x): one only the home network reaches
bool isPrivateIPv4(const std::string& ip);
}
