#include "common/api/nat-traversal/NatTraversal.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

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

// ============================================================================ invite code
static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static int DecodeChar(char c) {
    c = (char)toupper((unsigned char)c);
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    const char* p = strchr(kAlphabet, c);
    return p && c ? (int)(p - kAlphabet) : -1;
}

// The 80 bits of a version 2 code as two words: hi holds the top 16 bits, lo the low 64.
static uint32_t Get5(uint64_t hi, uint64_t lo, int pos) {  // the 5 bits from bit `pos` up
    uint64_t v = pos >= 64 ? hi >> (pos - 64) : pos == 0 ? lo : (lo >> pos) | (hi << (64 - pos));
    return (uint32_t)(v & 31);
}

std::string MakeInviteCode(const Endpoint& ep, uint32_t key) {
    // 80 bits: [version 2:4][ip:32][port:16][key:28]
    const uint8_t* b = (const uint8_t*)&ep.ip;
    uint32_t ip = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    uint64_t lo = (uint64_t)(ip & 0xFFFFF) << 44 | (uint64_t)ep.port << 28 | (key & kInviteKeyMask);
    uint64_t hi = (uint64_t)2 << 12 | ip >> 20;
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i && i % 4 == 0) out += '-';
        out += kAlphabet[Get5(hi, lo, 75 - 5 * i)];
    }
    return out;
}

bool ParseInviteCode(const std::string& code, Endpoint& ep, uint32_t& key) {
    uint64_t hi = 0, lo = 0;
    int n = 0;
    std::string c = code;
    if (c.size() > 3 && (c.rfind("SA-", 0) == 0 || c.rfind("sa-", 0) == 0)) c = c.substr(3);
    for (char ch : c) {
        if (ch == '-' || ch == ' ') continue;
        int d = DecodeChar(ch);
        if (d < 0 || ++n > 16) return false;
        hi = (hi << 5) | (lo >> 59);
        lo = (lo << 5) | (uint64_t)d;
    }
    uint32_t ipHost = 0;
    if (n == 12) {  // version 1: [version:4][ip:32][port:16][key:8], the key meant nothing
        if ((lo >> 56) != 1) return false;
        key = 0;
        ep.port = (uint16_t)((lo >> 8) & 0xFFFF);
        ipHost = (uint32_t)((lo >> 24) & 0xFFFFFFFF);
    } else if (n == 16) {
        if ((hi >> 12) != 2) return false;
        key = (uint32_t)(lo & kInviteKeyMask);
        ep.port = (uint16_t)((lo >> 28) & 0xFFFF);
        ipHost = (uint32_t)((hi & 0xFFF) << 20 | lo >> 44);
    } else {
        return false;
    }
    uint8_t b[4] = {(uint8_t)(ipHost >> 24), (uint8_t)(ipHost >> 16), (uint8_t)(ipHost >> 8), (uint8_t)ipHost};
    memcpy(&ep.ip, b, 4);
    return ep.Valid();
}

uint32_t NewInviteKey() {
    std::random_device rd;
    uint32_t k = 0;
    while (!k) k = ((uint32_t)rd() ^ ((uint32_t)rd() << 7) ^ (uint32_t)GetTickCount()) & kInviteKeyMask;
    return k;
}

std::string SignalTopic(const std::string& code) {
    std::string norm;
    for (char ch : code)
        if (ch != '-' && ch != ' ') norm += (char)toupper((unsigned char)ch);
    uint32_t a = Crc32(norm.data(), norm.size()), b = Crc32(norm.data(), norm.size(), 0x5A5A5A5A);
    return Format("sacoop-%08x%08x", a, b);
}

// ============================================================================ WinHTTP
struct CrackedUrl {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = false;
};

static bool Crack(const std::string& url, CrackedUrl& out) {
    std::wstring w = Utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return false;
    out.host.assign(host, uc.dwHostNameLength);
    out.path.assign(path, uc.dwUrlPathLength);
    if (out.path.empty()) out.path = L"/";
    out.port = uc.nPort;
    out.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

HttpResponse HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                         const std::string& headers, int timeoutMs) {
    HttpResponse resp;
    CrackedUrl u;
    if (!Crack(url, u)) return resp;
    HINTERNET ses = WinHttpOpen(L"SA-COOP/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"SA-COOP/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return resp;
    WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET con = WinHttpConnect(ses, u.host.c_str(), u.port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, Utf8ToWide(method).c_str(), u.path.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             u.https ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (req) {
        std::wstring h = Utf8ToWide(headers);
        BOOL ok = WinHttpSendRequest(req, h.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : h.c_str(), (DWORD)-1L,
                                     (void*)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
                  WinHttpReceiveResponse(req, nullptr);
        if (ok) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz,
                                nullptr);
            resp.status = (int)status;
            char buf[4096];
            DWORD n = 0;
            while (WinHttpReadData(req, buf, sizeof(buf), &n) && n > 0) {
                resp.body.append(buf, n);
                if (resp.body.size() > 256 * 1024) break;
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return resp;
}

bool SignalPublish(const std::string& baseUrl, const std::string& topic, const std::string& message) {
    auto r = HttpRequest("POST", baseUrl + "/" + topic, message, "Content-Type: text/plain\r\n", 6000);
    if (r.status != 200) LOGW("Сигнальный канал: публикация не удалась (HTTP %d)", r.status);
    return r.status == 200;
}

void SignalSubscriber::Start(const std::string& baseUrl, const std::string& topic,
                             std::function<void(const std::string&)> onMessage) {
    Stop();
    m_url = baseUrl + "/" + topic + "/json";
    m_cb = std::move(onMessage);
    m_quit = false;
    m_thread = CreateThread(nullptr, 0, &SignalSubscriber::ThreadProc, this, 0, nullptr);
}

void SignalSubscriber::Stop() {
    if (!m_thread) return;
    m_quit = true;
    if (void* req = m_request.exchange(nullptr)) WinHttpCloseHandle((HINTERNET)req);  // aborts the read
    WaitForSingleObject((HANDLE)m_thread, 15000);
    CloseHandle((HANDLE)m_thread);
    m_thread = nullptr;
}

unsigned long __stdcall SignalSubscriber::ThreadProc(void* param) {
    auto* self = (SignalSubscriber*)param;
    CrackedUrl base;
    if (!Crack(self->m_url, base)) return 0;
    std::string lastTime;  // newest event seen: a reconnect asks for what it missed (?since=)
    while (!self->m_quit) {
        CrackedUrl u = base;
        if (!lastTime.empty()) u.path += L"?since=" + std::wstring(lastTime.begin(), lastTime.end());
        HINTERNET ses = WinHttpOpen(L"SA-COOP/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) ses = WinHttpOpen(L"SA-COOP/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) {
            Sleep(3000);
            continue;
        }
        // the stream sends keepalive events every ~45 s; Stop() closes the request to end a read early
        WinHttpSetTimeouts(ses, 8000, 8000, 8000, 60000);
        HINTERNET con = WinHttpConnect(ses, u.host.c_str(), u.port, 0);
        HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)
                            : nullptr;
        self->m_request = req;
        bool ok = req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
                  WinHttpReceiveResponse(req, nullptr);
        std::string pending;
        while (ok && !self->m_quit) {
            // ask what arrived first: a plain read would wait for a full buffer of this slow stream
            DWORD avail = 0, n = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, 0);
            if (!WinHttpReadData(req, chunk.data(), avail, &n) || n == 0) break;
            pending.append(chunk.data(), n);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                size_t t = line.find("\"time\":");
                if (t != std::string::npos) lastTime = line.substr(t + 7, line.find_first_not_of("0123456789", t + 7) - t - 7);
                // {"id":"..","time":..,"event":"message","topic":"..","message":"punch 1.2.3.4:5"}
                if (line.find("\"event\":\"message\"") == std::string::npos) continue;
                size_t m = line.find("\"message\":\"");
                if (m == std::string::npos) continue;
                size_t e = line.find('"', m + 11);
                if (e == std::string::npos) continue;
                if (self->m_cb) self->m_cb(line.substr(m + 11, e - m - 11));
            }
        }
        if (void* open = self->m_request.exchange(nullptr)) WinHttpCloseHandle((HINTERNET)open);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        for (int i = 0; i < 30 && !self->m_quit; ++i) Sleep(100);
    }
    return 0;
}

}  // namespace coop::nat
