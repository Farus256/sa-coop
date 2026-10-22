#include "common/api/nat-traversal/NatTraversal.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <random>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop::nat {

// ============================================================================ helpers
std::string IpToString(uint32_t ipNet) {
    const uint8_t* b = (const uint8_t*)&ipNet;
    return Format("%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

std::string Endpoint::ToString() const { return IpToString(ip) + ":" + std::to_string(port); }

bool Endpoint::Parse(const std::string& s, Endpoint& out) {
    size_t colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    in_addr a{};
    if (inet_pton(AF_INET, s.substr(0, colon).c_str(), &a) != 1) return false;
    int port = atoi(s.c_str() + colon + 1);
    if (port <= 0 || port > 65535) return false;
    out.ip = a.s_addr;
    out.port = (uint16_t)port;
    return true;
}

static void EnsureWinsock() {
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
}

uint32_t LocalIp() {
    EnsureWinsock();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    uint32_t ip = 0;
    // connect() on UDP sends nothing, it only selects the outgoing interface
    if (connect(s, (sockaddr*)&to, sizeof(to)) == 0) {
        sockaddr_in me{};
        int len = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &len) == 0) ip = me.sin_addr.s_addr;
    }
    closesocket(s);
    return ip;
}

bool IsPrivateIp(uint32_t ipNet) {
    uint32_t ip = ntohl(ipNet);
    return (ip >> 24) == 10 || (ip >> 20) == (172 << 4 | 1) || (ip >> 16) == (192 << 8 | 168) ||
           (ip >> 22) == ((100 << 2) | 1) /* 100.64/10 */ || (ip >> 24) == 127 || ip == 0;
}

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Extracts the text of the first <tag>...</tag> after `from`.
static std::string XmlTag(const std::string& xml, const std::string& tag, size_t from = 0, size_t* endPos = nullptr) {
    std::string open = "<" + tag + ">", close = "</" + tag + ">";
    // tags may carry namespace prefixes, so search case-insensitively for the local name
    std::string lx = Lower(xml), lo = Lower(open), lc = Lower(close);
    size_t a = lx.find(lo, from);
    if (a == std::string::npos) {
        // try any prefix: ":tag>"
        size_t p = lx.find(":" + Lower(tag) + ">", from);
        if (p == std::string::npos) return "";
        a = lx.rfind('<', p);
        lo = lx.substr(a, p + tag.size() + 2 - a);
        lc = "</" + lo.substr(1);
    }
    size_t b = lx.find(lc, a + lo.size());
    if (b == std::string::npos) return "";
    if (endPos) *endPos = b + lc.size();
    return Trim(xml.substr(a + lo.size(), b - a - lo.size()));
}

// ============================================================================ minimal HTTP over TCP (for the router)
struct Url {
    std::string host, path = "/";
    uint16_t port = 80;
};

static bool ParseUrl(const std::string& url, Url& u) {
    std::string s = url;
    size_t p = s.find("://");
    if (p != std::string::npos) s = s.substr(p + 3);
    size_t slash = s.find('/');
    std::string hostport = slash == std::string::npos ? s : s.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : s.substr(slash);
    size_t colon = hostport.find(':');
    u.host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    u.port = colon == std::string::npos ? 80 : (uint16_t)atoi(hostport.c_str() + colon + 1);
    return !u.host.empty();
}

static std::string RawHttp(const Url& u, const std::string& request, int timeoutMs) {
    EnsureWinsock();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) return "";
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD to = (DWORD)timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to));
    std::string out;
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0) {
        send(s, request.data(), (int)request.size(), 0);
        char buf[4096];
        int n;
        while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
            out.append(buf, n);
            if (out.size() > 512 * 1024) break;
        }
    }
    closesocket(s);
    freeaddrinfo(res);
    return out;
}

static std::string HttpBody(const std::string& resp, int* status = nullptr) {
    if (status) *status = resp.size() > 12 ? atoi(resp.c_str() + 9) : 0;
    size_t p = resp.find("\r\n\r\n");
    if (p == std::string::npos) return "";
    std::string headers = Lower(resp.substr(0, p));
    std::string body = resp.substr(p + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string out;
        size_t i = 0;
        while (i < body.size()) {
            size_t e = body.find("\r\n", i);
            if (e == std::string::npos) break;
            size_t len = strtoul(body.substr(i, e - i).c_str(), nullptr, 16);
            if (len == 0 || len > body.size() - (e + 2)) break;  // a length past the end wrapped i in 32 bits
            out += body.substr(e + 2, len);
            i = e + 2 + len + 2;
        }
        return out;
    }
    return body;
}

// ============================================================================ UPnP IGD
static std::vector<std::string> SsdpDiscover(int timeoutMs) {
    EnsureWinsock();
    std::vector<std::string> locations;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return locations;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = LocalIp();  // send from the interface that has the default route
    bind(s, (sockaddr*)&local, sizeof(local));
    int ttl = 2;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    sockaddr_in mc{};
    mc.sin_family = AF_INET;
    mc.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &mc.sin_addr);
    const char* targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:service:WANIPConnection:1",
                             "urn:schemas-upnp-org:service:WANPPPConnection:1",
                             "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
    for (const char* st : targets) {
        std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nST: ") + st +
                          "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n\r\n";
        sendto(s, msg.data(), (int)msg.size(), 0, (sockaddr*)&mc, sizeof(mc));
    }
    uint64_t end = NowMs() + (uint64_t)timeoutMs;
    char buf[2048];
    while (NowMs() < end) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{0, 100 * 1000};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string resp(buf), low = Lower(resp);
        size_t p = low.find("\r\nlocation:");
        if (p == std::string::npos) continue;
        size_t e = resp.find("\r\n", p + 2);
        std::string loc = Trim(resp.substr(p + 11, e - p - 11));
        if (std::find(locations.begin(), locations.end(), loc) == locations.end()) locations.push_back(loc);
    }
    closesocket(s);
    return locations;
}

static std::string Soap(const UpnpResult& r, const std::string& action, const std::string& args, int* status) {
    Url u;
    ParseUrl(r.controlUrl, u);
    std::string body = "<?xml version=\"1.0\"?>\r\n"
                       "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                       "<u:" + action + " xmlns:u=\"" + r.serviceType + "\">" + args + "</u:" + action +
                       "></s:Body></s:Envelope>\r\n";
    std::string req = "POST " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                      "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + r.serviceType + "#" + action +
                      "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    return HttpBody(RawHttp(u, req, 4000), status);
}

UpnpResult UpnpMapPort(uint16_t port, const std::string& description) {
    UpnpResult r;
    uint32_t local = LocalIp();
    if (!local) {
        r.error = "нет сетевого подключения";
        return r;
    }
    auto locations = SsdpDiscover(2500);
    if (locations.empty()) {
        r.error = "роутер с UPnP не найден (UPnP выключен в настройках роутера?)";
        return r;
    }
    for (auto& loc : locations) {
        Url u;
        if (!ParseUrl(loc, u)) continue;
        std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                          "\r\nConnection: close\r\n\r\n";
        std::string xml = HttpBody(RawHttp(u, req, 3000));
        if (xml.empty()) continue;
        std::string base = XmlTag(xml, "URLBase");
        if (base.empty()) base = "http://" + u.host + ":" + std::to_string(u.port);
        while (!base.empty() && base.back() == '/') base.pop_back();
        // find WANIPConnection / WANPPPConnection service blocks
        size_t pos = 0;
        while (true) {
            size_t end = 0;
            std::string svc = XmlTag(xml, "service", pos, &end);
            if (svc.empty()) break;
            pos = end;
            std::string type = XmlTag(svc, "serviceType");
            if (type.find("WANIPConnection") == std::string::npos && type.find("WANPPPConnection") == std::string::npos)
                continue;
            std::string ctl = XmlTag(svc, "controlURL");
            if (ctl.empty()) continue;
            r.serviceType = type;
            r.controlUrl = ctl.rfind("http", 0) == 0 ? ctl : base + (ctl[0] == '/' ? "" : "/") + ctl;
            int st = 0;
            std::string ipResp = Soap(r, "GetExternalIPAddress", "", &st);
            r.externalIp = XmlTag(ipResp, "NewExternalIPAddress");
            std::string localStr = IpToString(local);
            // a finite lease first: a crashed server must not leave the port open for good
            for (int lease : {3600, 0}) {
                std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                                   "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" +
                                   std::to_string(port) + "</NewInternalPort><NewInternalClient>" + localStr +
                                   "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" +
                                   description + "</NewPortMappingDescription><NewLeaseDuration>" +
                                   std::to_string(lease) + "</NewLeaseDuration>";
                Soap(r, "AddPortMapping", args, &st);
                if (st == 200) {
                    r.ok = true;
                    r.leaseSeconds = lease;
                    LOGI("UPnP: UDP %u -> %s:%u на роутере (%s), внешний IP %s", port, localStr.c_str(), port,
                         type.c_str(), r.externalIp.c_str());
                    return r;
                }
            }
            r.error = Format("роутер отказал в пробросе порта (код %d)", st);
        }
    }
    if (r.error.empty()) r.error = "роутер не поддерживает WANIPConnection";
    return r;
}

void UpnpUnmapPort(const UpnpResult& r, uint16_t port) {
    if (!r.ok) return;
    int st = 0;
    Soap(r, "DeletePortMapping",
         "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
             "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
         &st);
}

// ============================================================================ STUN
const std::vector<std::string>& StunServers() {
    static const std::vector<std::string> servers = {
        "stun.l.google.com:19302", "stun1.l.google.com:19302", "stun.cloudflare.com:3478",
        "stun.sipnet.ru:3478",     "stun.voip.blackberry.com:3478"};
    return servers;
}

void BuildStunRequest(uint8_t out[20], uint8_t txid[12]) {
    static std::mt19937 rng((unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId());
    out[0] = 0x00;
    out[1] = 0x01;  // Binding Request
    out[2] = 0x00;
    out[3] = 0x00;  // length
    out[4] = 0x21;
    out[5] = 0x12;
    out[6] = 0xA4;
    out[7] = 0x42;  // magic cookie
    for (int i = 0; i < 12; ++i) txid[i] = (uint8_t)rng();
    memcpy(out + 8, txid, 12);
}

bool IsStunPacket(const uint8_t* d, size_t len) {
    return len >= 20 && (d[0] & 0xC0) == 0 && d[4] == 0x21 && d[5] == 0x12 && d[6] == 0xA4 && d[7] == 0x42;
}

bool ParseStunResponse(const uint8_t* d, size_t len, const uint8_t txid[12], Endpoint& mapped) {
    if (!IsStunPacket(d, len) || d[0] != 0x01 || d[1] != 0x01) return false;
    if (txid && memcmp(d + 8, txid, 12) != 0) return false;
    size_t msgLen = (size_t)(d[2] << 8 | d[3]);
    size_t end = std::min(len, 20 + msgLen);
    size_t p = 20;
    bool got = false;
    while (p + 4 <= end) {
        uint16_t type = (uint16_t)(d[p] << 8 | d[p + 1]);
        uint16_t alen = (uint16_t)(d[p + 2] << 8 | d[p + 3]);
        const uint8_t* v = d + p + 4;
        if (p + 4 + alen > end) break;
        if ((type == 0x0020 || type == 0x8020) && alen >= 8 && v[1] == 0x01) {  // XOR-MAPPED-ADDRESS, IPv4
            uint16_t port = (uint16_t)((v[2] << 8 | v[3]) ^ 0x2112);
            uint8_t ip[4] = {(uint8_t)(v[4] ^ 0x21), (uint8_t)(v[5] ^ 0x12), (uint8_t)(v[6] ^ 0xA4), (uint8_t)(v[7] ^ 0x42)};
            memcpy(&mapped.ip, ip, 4);
            mapped.port = port;
            return true;
        }
        if (type == 0x0001 && alen >= 8 && v[1] == 0x01 && !got) {  // MAPPED-ADDRESS
            mapped.port = (uint16_t)(v[2] << 8 | v[3]);
            memcpy(&mapped.ip, v + 4, 4);
            got = true;
        }
        p += 4 + ((alen + 3) & ~3u);
    }
    return got;
}

}  // namespace coop::nat
