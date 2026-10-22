#pragma once
// Playing over the internet by IP address:
//  * UPnP IGD port mapping (SSDP discovery + SOAP), no external libraries;
//  * STUN (RFC 5389) helpers to learn the public address of an ENet socket (shown to the host to give to friends).
#include <cstdint>
#include <string>
#include <vector>

namespace coop::nat {

struct Endpoint {
    uint32_t ip = 0;  // network byte order (as in ENetAddress.host)
    uint16_t port = 0;
    bool Valid() const { return ip != 0 && port != 0; }
    std::string ToString() const;
    static bool Parse(const std::string& s, Endpoint& out);  // "1.2.3.4:5678"
};

// ---------------------------------------------------------------- local network
uint32_t LocalIp();                     // IPv4 of the default-route interface (network order)
bool IsPrivateIp(uint32_t ipNet);       // 10/8, 172.16/12, 192.168/16, 100.64/10 (CGNAT), 127/8
std::string IpToString(uint32_t ipNet);

// ---------------------------------------------------------------- UPnP
struct UpnpResult {
    bool ok = false;
    std::string error;
    std::string externalIp;  // router WAN address, may be private when the ISP uses CGNAT
    std::string controlUrl, serviceType;
    int leaseSeconds = 0;    // 0 = permanent (the router supports nothing else)
};
// Blocking (2-6 s). Maps UDP `port` on the router to this machine.
UpnpResult UpnpMapPort(uint16_t port, const std::string& description);
void UpnpUnmapPort(const UpnpResult& r, uint16_t port);

// ---------------------------------------------------------------- STUN
// Default public STUN servers ("host:port").
const std::vector<std::string>& StunServers();
// Fills a 20-byte binding request; txid receives the 12-byte transaction id.
void BuildStunRequest(uint8_t out[20], uint8_t txid[12]);
// Parses a binding success response; returns false if `data` is not a STUN response for txid.
bool ParseStunResponse(const uint8_t* data, size_t len, const uint8_t txid[12], Endpoint& mapped);
bool IsStunPacket(const uint8_t* data, size_t len);

// A packet a client sends to the server port before connecting, and the server's answer: tells "nothing listens there"
// from "the server is there" when the connection fails
constexpr const char kPingMagic[] = "SACOOP-PING";
constexpr const char kPongMagic[] = "SACOOP-PONG";

}  // namespace coop::nat
