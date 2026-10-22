#include "features/internet/Internet.h"

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"

namespace coop::server {

namespace {
Internet* g_self = nullptr;
}

void Internet::Start(ENetHost* host, uint16_t port, bool internet) {
    m_host = host;
    m_port = port;
    g_self = this;
    host->intercept = &Internet::Intercept;
    AddressInfo info;
    if (nat::Endpoint lan{nat::LocalIp(), port}; lan.Valid()) info.lan = lan.ToString();
    info.done = !internet;
    Publish(info);
    if (!internet) return;
    m_quit = false;
    m_thread = std::thread(&Internet::Worker, this);
}

void Internet::Stop() {
    m_quit = true;
    if (m_thread.joinable()) m_thread.join();
    if (m_upnp.ok) {
        nat::UpnpUnmapPort(m_upnp, m_port);
        m_upnp.ok = false;
    }
    if (m_host) m_host->intercept = nullptr;
    g_self = nullptr;
}

void Internet::Publish(const AddressInfo& info) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_info = info;
    m_infoNew = true;
}

bool Internet::TakeNewInfo(AddressInfo& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_infoNew) return false;
    m_infoNew = false;
    out = m_info;
    return true;
}

AddressInfo Internet::Info() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_info;
}

// STUN answers and the clients' pings arrive on the game port: they never reach ENet's protocol code
int ENET_CALLBACK Internet::Intercept(ENetHost* host, ENetEvent*) {
    Internet* self = g_self;
    const uint8_t* data = host->receivedData;
    size_t len = host->receivedDataLength;
    if (!self) return 0;
    if (len == sizeof(nat::kPingMagic) - 1 && !memcmp(data, nat::kPingMagic, len)) {
        ENetBuffer buf{sizeof(nat::kPongMagic) - 1, (void*)nat::kPongMagic};
        enet_socket_send(host->socket, &host->receivedAddress, &buf, 1);
        return 1;
    }
    if (!nat::IsStunPacket(data, len)) return 0;
    std::lock_guard<std::mutex> lock(self->m_mutex);
    nat::Endpoint mapped;
    if (self->m_stunWaiting && nat::ParseStunResponse(data, len, self->m_txid, mapped)) {
        self->m_mapped = mapped;
        self->m_stunWaiting = false;
    }
    return 1;
}

void Internet::Worker() {
    AddressInfo info = Info();

    // 1. the router forwards the port (most home routers can; it does not help behind the provider's NAT)
    m_upnp = nat::UpnpMapPort(m_port, "SA:COOP server");
    info.upnp = m_upnp.ok;
    info.upnpError = m_upnp.error;
    if (m_upnp.ok) LOGI("UPnP: порт %u проброшен. Внешний адрес роутера: %s", m_port, m_upnp.externalIp.c_str());
    else LOGW("UPnP недоступен (%s). Порт %u нужно открыть на роутере вручную", m_upnp.error.c_str(), m_port);

    // 2. the address the internet sees for our socket
    for (const std::string& server : nat::StunServers()) {
        if (m_quit) return;
        size_t colon = server.rfind(':');
        ENetAddress addr{};
        if (enet_address_set_host(&addr, server.substr(0, colon).c_str()) != 0) continue;
        addr.port = (enet_uint16)atoi(server.c_str() + colon + 1);
        uint8_t req[20];
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            nat::BuildStunRequest(req, m_txid);
            m_stunWaiting = true;
        }
        ENetBuffer buf{sizeof(req), req};
        enet_socket_send(m_host->socket, &addr, &buf, 1);
        for (int i = 0; i < 20 && !m_quit; ++i) {
            Sleep(100);
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_stunWaiting) break;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_mapped.Valid()) break;
        m_stunWaiting = false;
    }
    nat::Endpoint stun;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        stun = m_mapped;
    }

    // the router's WAN address is private (or another than the internet sees): the provider's NAT is in front of it
    nat::Endpoint wan;
    bool wanKnown = m_upnp.ok && nat::Endpoint::Parse(m_upnp.externalIp + ":" + std::to_string(m_port), wan);
    if (wanKnown && (nat::IsPrivateIp(wan.ip) || (stun.Valid() && stun.ip != wan.ip))) info.cgnat = true;
    if (stun.Valid()) info.external = nat::Endpoint{stun.ip, m_port}.ToString();  // the port the router forwards
    else if (wanKnown && !nat::IsPrivateIp(wan.ip)) info.external = wan.ToString();
    info.done = true;
    Publish(info);
    if (info.external.empty()) LOGW("Внешний адрес не определён (нет интернета?). Подключиться можно только по локальной сети");
    else LOGI("Адрес для друзей: %s%s", info.external.c_str(), info.cgnat ? " (за NAT провайдера, из интернета недоступен)" : "");

    // 3. renew a finite UPnP lease while the server runs
    for (uint64_t renewAt = NowMs() + (uint64_t)m_upnp.leaseSeconds * 500; m_upnp.ok && m_upnp.leaseSeconds > 0 && !m_quit;) {
        Sleep(500);
        if (NowMs() < renewAt) continue;
        nat::UpnpResult again = nat::UpnpMapPort(m_port, "SA:COOP server");
        if (again.ok) m_upnp = again;
        renewAt = NowMs() + (uint64_t)m_upnp.leaseSeconds * 500;
    }
}

}  // namespace coop::server
