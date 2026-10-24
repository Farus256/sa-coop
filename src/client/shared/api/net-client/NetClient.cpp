#include "shared/api/net-client/NetClient.h"

#include <enet/enet.h>

#include "common/api/nat-traversal/NatTraversal.h"
#include "common/api/net/Net.h"
#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"

namespace coop {

namespace {
// What answers on the server's port, asked with a packet of our own after ENet gave up connecting. A closed UDP port
// answers with ICMP "port unreachable", which Windows reports to a connected socket as WSAECONNRESET (ENet swallows it).
enum class Probe { Answered, Refused, Silent, NoRoute };

Probe ProbeServer(const ENetAddress& to) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return Probe::Silent;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = to.host;
    sa.sin_port = htons(to.port);
    Probe result = Probe::Silent;
    if (connect(s, (sockaddr*)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        return Probe::NoRoute;
    }
    for (int i = 0; i < 3 && result == Probe::Silent; ++i) {
        if (send(s, nat::kPingMagic, sizeof(nat::kPingMagic) - 1, 0) < 0) {
            int e = WSAGetLastError();
            if (e == WSAENETUNREACH || e == WSAEHOSTUNREACH || e == WSAENETDOWN) result = Probe::NoRoute;
            else if (e == WSAECONNRESET) result = Probe::Refused;
            break;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{0, 500000};
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char buf[64];
        int n = recv(s, buf, sizeof(buf), 0);
        if (n == (int)sizeof(nat::kPongMagic) - 1 && !memcmp(buf, nat::kPongMagic, n)) result = Probe::Answered;
        else if (n < 0 && WSAGetLastError() == WSAECONNRESET) result = Probe::Refused;
        else if (n < 0 && (WSAGetLastError() == WSAENETUNREACH || WSAGetLastError() == WSAEHOSTUNREACH)) result = Probe::NoRoute;
    }
    closesocket(s);
    return result;
}

bool SameNetwork(uint32_t a, uint32_t b) { return (ntohl(a) >> 8) == (ntohl(b) >> 8); }  // a /24, as home networks are

// why the connection to `shown` (as the player typed it) at `to` could not be made
std::string ConnectFailure(const std::string& shown, const ENetAddress& to) {
    std::string ip = nat::IpToString(to.host);
    std::string where = ip + ":" + std::to_string(to.port);
    std::string named = shown == ip ? where : shown + " (" + where + ")";
    Probe probe = ProbeServer(to);
    uint32_t ipHost = ntohl(to.host);
    bool loopback = (ipHost >> 24) == 127;
    LOGI("Проверка порта %s: %s", where.c_str(),
         probe == Probe::Answered ? "сервер отвечает" : probe == Probe::Refused ? "порт закрыт" : probe == Probe::NoRoute ? "нет маршрута" : "тишина");
    switch (probe) {
        case Probe::Answered:
            return Format(TR("Сервер %s отвечает, но соединение не устанавливается. Попробуйте ещё раз; если повторяется, "
                             "разрешите gta_sa.exe в брандмауэре или антивирусе.",
                             "The server %s answers, but the connection does not open. Try again; if it keeps happening, allow "
                             "gta_sa.exe in your firewall or antivirus."),
                          named.c_str());
        case Probe::Refused:
            if (loopback)
                return Format(TR("На этом компьютере сервер на порту %u не запущен.", "No server is running on port %u on this computer."), to.port);
            return Format(TR("На %s порт %u закрыт: сервер там не запущен. Проверьте, что хост создал сервер и что порт "
                             "совпадает с портом хоста.",
                             "On %s port %u is closed: no server runs there. Check that the host has started the server and "
                             "that the port matches the host's."),
                          ip.c_str(), to.port);
        case Probe::NoRoute:
            return Format(TR("До адреса %s нет маршрута: проверьте подключение к интернету или к сети на этом компьютере.",
                             "There is no route to %s: check this computer's internet or network connection."),
                          ip.c_str());
        case Probe::Silent:
            break;
    }
    if ((ipHost >> 22) == ((100 << 2) | 1))  // 100.64/10
        return Format(TR("%s - адрес из сети провайдера (CGNAT), из интернета к нему подключиться нельзя. Хосту нужен белый IP "
                         "у провайдера или VPN (Radmin VPN, ZeroTier).",
                         "%s is an address inside the provider's network (CGNAT), it cannot be reached from the internet. The "
                         "host needs a public IP from the provider or a VPN (Radmin VPN, ZeroTier)."),
                      ip.c_str());
    if (nat::IsPrivateIp(to.host) && !loopback) {
        uint32_t mine = nat::LocalIp();
        if (mine && !SameNetwork(mine, to.host))
            return Format(TR("%s - локальный адрес чужой сети, через интернет он недоступен. Нужен внешний IP хоста: "
                             "он показан у хоста в окне F9.",
                             "%s is a local address of another network, it cannot be reached over the internet. You need the "
                             "host's external IP: the host sees it in the F9 window."),
                          ip.c_str());
        return Format(TR("Компьютер %s в вашей сети не отвечает на порту %u. Проверьте, что на нём запущен сервер и что "
                         "брандмауэр Windows на нём пропускает coop-server.exe.",
                         "The computer %s in your network does not answer on port %u. Check that the server runs on it and "
                         "that its Windows firewall lets coop-server.exe through."),
                      ip.c_str(), to.port);
    }
    return Format(TR("Сервер %s не отвечает. Скорее всего, у хоста закрыт UDP-порт %u на роутере (окно F9 у хоста "
                     "показывает, сработал ли UPnP). Ещё проверьте адрес и что сервер запущен.",
                     "The server %s does not answer. Most likely UDP port %u is closed on the host's router (the host's F9 "
                     "window shows whether UPnP worked). Also check the address and that the server is running."),
                  named.c_str(), to.port);
}

// why a working connection ended
std::string LostReason(uint32_t code, uint32_t silentMs) {
    switch (code) {
        case proto::DISCONNECT_SERVER_STOP:
            return TR("Хост закрыл сервер.", "The host closed the server.");
        case proto::DISCONNECT_KICKED:
            return TR("Вас выгнали с сервера.", "You were kicked from the server.");
        case proto::DISCONNECT_REJECTED:
            return TR("Сервер отказал во входе.", "The server refused to let you in.");
        default:
            break;
    }
    return Format(TR("Связь с сервером пропала: он не отвечал %u с. У хоста пропал интернет, его игра закрылась или "
                     "зависла, либо пропал интернет у вас.",
                     "Lost the connection: the server did not answer for %u s. The host lost the internet, the host's game "
                     "closed or hung, or your internet dropped."),
                  (silentMs + 500) / 1000);
}
}  // namespace

bool NetClient::Start(const std::string& host, uint16_t port) {
    Stop();
    if (!net::Init()) {
        LOGE("enet init failed");
        return false;
    }
    m_quit = false;
    m_state = State::Connecting;
    m_thread = std::thread(&NetClient::ThreadMain, this, host, port);
    return true;
}

void NetClient::Stop() {
    if (m_thread.joinable()) {
        m_quit = true;
        m_thread.join();
        net::Shutdown();
    }
    m_state = State::Idle;
    std::lock_guard<std::mutex> a(m_outMutex);
    m_out.clear();
    // what came after the last poll (a refusal's reason, a world chunk) belonged to that connection: it was handled
    // in the next one ("the world arrived damaged")
    std::lock_guard<std::mutex> b(m_inMutex);
    m_in.clear();
}

void NetClient::Send(uint8_t channel, const Writer& w) { Send(channel, w.Data(), w.Size()); }

void NetClient::Send(uint8_t channel, const uint8_t* data, size_t size) {
    if (m_state != State::Connected || size == 0) return;
    std::lock_guard<std::mutex> lock(m_outMutex);
    m_out.push_back(Packet{channel, std::vector<uint8_t>(data, data + size)});
}

void NetClient::Poll(std::vector<Packet>& out) {
    std::lock_guard<std::mutex> lock(m_inMutex);
    if (m_in.empty()) return;
    if (out.empty()) {
        out.swap(m_in);
    } else {
        for (auto& p : m_in) out.push_back(std::move(p));
        m_in.clear();
    }
}

std::string NetClient::FailReason() {
    std::lock_guard<std::mutex> lock(m_reasonMutex);
    return m_failReason;
}

void NetClient::Fail(ENetHost* client, const std::string& why) {
    LOGW("%s", why.c_str());
    {
        std::lock_guard<std::mutex> lock(m_reasonMutex);
        m_failReason = why;
    }
    if (client) enet_host_destroy(client);
    m_state = State::Disconnected;
    m_disconnectEvent = true;
}

void NetClient::ThreadMain(std::string host, uint16_t port) {
    {
        std::lock_guard<std::mutex> lock(m_reasonMutex);
        m_failReason.clear();
    }
    ENetHost* client = enet_host_create(nullptr, 1, proto::CH_COUNT, 0, 0);
    if (!client) {
        Fail(nullptr, TR("Не удалось открыть сетевой сокет: Windows не дала создать UDP-сокет (антивирус или нехватка "
                         "ресурсов).",
                         "Could not open a network socket: Windows refused a UDP socket (an antivirus, or out of resources)."));
        return;
    }
    enet_host_compress_with_range_coder(client);
    client->mtu = net::kMtu;
    ENetAddress addr{};
    if (host.empty()) {
        Fail(client, TR("Не указан адрес сервера.", "No server address given."));
        return;
    }
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        Fail(client, Format(TR("Адрес «%s» не найден. Проверьте, что он написан правильно, например 85.12.34.56 или "
                               "85.12.34.56:7788.",
                               "The address \"%s\" was not found. Check that it is spelled right, for example 85.12.34.56 "
                               "or 85.12.34.56:7788."),
                            host.c_str()));
        return;
    }
    addr.port = port;
    ENetPeer* peer = enet_host_connect(client, &addr, proto::CH_COUNT, 0);
    if (!peer) {
        Fail(client, TR("Не удалось начать подключение (ENet).", "Could not start connecting (ENet)."));
        return;
    }
    enet_peer_timeout(peer, 0, 10000, 20000);
    LOGI("Подключение к %s:%u...", host.c_str(), port);

    std::vector<Packet> outLocal;
    bool connected = false;
    // the connection just before it ended, for the log: ENet resets the peer before it reports the end
    enet_uint32 lastLoss = 0, lastInTransit = 0, lastHeard = 0;
    while (!m_quit) {
        // flush outgoing
        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            outLocal.swap(m_out);
        }
        if (connected) {
            for (auto& p : outLocal) {
                ENetPacket* pkt = net::MakePacket(p.data.data(), p.data.size(), p.channel);
                if (enet_peer_send(peer, p.channel, pkt) != 0) enet_packet_destroy(pkt);
                m_bytesOut += p.data.size();
            }
        }
        outLocal.clear();

        ENetEvent ev;
        int rc = enet_host_service(client, &ev, 2);
        while (rc > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    connected = true;
                    m_state = State::Connected;
                    m_connectEvent = true;
                    LOGI("Соединение с сервером установлено (MTU %u)", peer->mtu);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT: {
                    LOGW("Соединение с сервером %s", connected ? "потеряно" : "не установлено");
                    std::string why;
                    if (connected) {
                        LOGI("Перед разрывом: пинг %u мс, потери %.1f%%, без подтверждения %u байт, сервер молчал %u мс, код %u",
                             m_rtt.load(), lastLoss * 100.0 / (double)ENET_PEER_PACKET_LOSS_SCALE, lastInTransit,
                             enet_time_get() - lastHeard, ev.data);
                        why = LostReason(ev.data, enet_time_get() - lastHeard);
                    } else {
                        why = ev.data ? LostReason(ev.data, 0) : ConnectFailure(host, addr);
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_reasonMutex);
                        m_failReason = why;
                    }
                    connected = false;
                    m_state = State::Disconnected;
                    m_disconnectEvent = true;
                    m_quit = true;
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    m_bytesIn += ev.packet->dataLength;
                    Packet p{ev.channelID, std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength)};
                    enet_packet_destroy(ev.packet);
                    std::lock_guard<std::mutex> lock(m_inMutex);
                    // a game that stands (a long load, a hang, alt-tab without run_in_background) piled up every state
                    // and applied them all at once, stale: past a few thousand only the reliable ones are kept
                    if (m_in.size() > 4000 && (p.channel == proto::CH_STATE || p.channel == proto::CH_VOICE)) break;
                    m_in.push_back(std::move(p));
                    break;
                }
                default:
                    break;
            }
            if (m_quit) break;
            rc = enet_host_check_events(client, &ev);
        }
        if (connected) {
            m_rtt = peer->roundTripTime;
            lastLoss = peer->packetLoss;
            lastInTransit = peer->reliableDataInTransit;
            lastHeard = peer->lastReceiveTime;
        }
    }

    if (connected) {
        // what the game queued last goes first (the partner's profile on leaving): enet_peer_disconnect drops the
        // queues, enet_peer_disconnect_later sends them and then disconnects
        {
            std::lock_guard<std::mutex> lock(m_outMutex);
            outLocal.swap(m_out);
        }
        for (auto& p : outLocal) {
            ENetPacket* pkt = net::MakePacket(p.data.data(), p.data.size(), p.channel);
            if (enet_peer_send(peer, p.channel, pkt) != 0) enet_packet_destroy(pkt);
        }
        outLocal.clear();
        // graceful quit: the code tells the server it was not a timeout
        enet_peer_disconnect_later(peer, proto::DISCONNECT_QUIT);
        ENetEvent ev;
        uint64_t deadline = GetTickCount64() + 1000;
        while (GetTickCount64() < deadline && enet_host_service(client, &ev, 50) >= 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
        enet_peer_reset(peer);
    }
    enet_host_destroy(client);
    if (m_state != State::Disconnected) {
        m_state = State::Disconnected;
    }
}

}  // namespace coop
