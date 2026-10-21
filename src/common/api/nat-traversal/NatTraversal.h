#pragma once
// Internet play without VPN:
//  * UPnP IGD port mapping (SSDP discovery + SOAP), no external libraries;
//  * STUN (RFC 5389) helpers to learn the public address of an ENet socket;
//  * invite codes ("XXXX-XXXX-XXXX") carrying the host's public endpoint;
//  * a tiny HTTP pub/sub signalling channel (ntfy-compatible) used for UDP hole punching
//    when the router has no UPnP.
#include <atomic>
#include <cstdint>
#include <functional>
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

// ---------------------------------------------------------------- invite code
// Crockford base32. Version 2 (this fork): 16 characters, 4 bits version, 32 bits IPv4, 16 bits port and a 28-bit key
// the server asks for, so the code is the ticket into the game ("XXXX-XXXX-XXXX-XXXX"). Version 1 (the original
// two-player mod): 12 characters with an 8-bit key nobody checked; it still parses, with key 0.
constexpr uint32_t kInviteKeyMask = 0x0FFFFFFF;
std::string MakeInviteCode(const Endpoint& ep, uint32_t key);
bool ParseInviteCode(const std::string& code, Endpoint& ep, uint32_t& key);
uint32_t NewInviteKey();  // random, never 0
std::string SignalTopic(const std::string& code);  // channel name derived from the code

// ---------------------------------------------------------------- HTTP / signalling
struct HttpResponse {
    int status = 0;
    std::string body;
};
// Plain HTTP(S) request through WinHTTP. `url` = http(s)://host[:port]/path
HttpResponse HttpRequest(const std::string& method, const std::string& url, const std::string& body = "",
                         const std::string& headers = "", int timeoutMs = 5000);

// Subscribes to `baseUrl/topic` (ntfy JSON stream) on a background thread and calls `onMessage`
// for every published message text. Stop() ends the subscription.
class SignalSubscriber {
public:
    ~SignalSubscriber() { Stop(); }
    void Start(const std::string& baseUrl, const std::string& topic, std::function<void(const std::string&)> onMessage);
    void Stop();

private:
    void* m_thread = nullptr;
    std::atomic<bool> m_quit{false};
    std::atomic<void*> m_request{nullptr};  // the open stream, closed by Stop() to end a blocking read
    std::string m_url;
    std::function<void(const std::string&)> m_cb;
    static unsigned long __stdcall ThreadProc(void* self);
};

bool SignalPublish(const std::string& baseUrl, const std::string& topic, const std::string& message);

constexpr const char* kDefaultSignalUrl = "https://ntfy.sh";
constexpr const char kPunchMagic[] = "SACOOP-PUNCH";
// LAN discovery for a joiner behind the same public address as the host (same flat, same carrier NAT):
// broadcast kDiscoverMagic to the server port, the server answers kHereMagic + "<port>"
constexpr const char kDiscoverMagic[] = "SACOOP-DISCOVER";
constexpr const char kHereMagic[] = "SACOOP-HERE ";

}  // namespace coop::nat
