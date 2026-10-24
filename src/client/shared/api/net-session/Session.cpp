#include "shared/api/net-session/Session.h"

#include <algorithm>
#include <cstdlib>

#include "common/config/version/Version.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

using namespace proto;

Session& Net() {
    static Session s;
    return s;
}

uint32_t PlayerColor(uint8_t id) {
    // light enough for the dark chat background and the radar, apart from one another; the first two are the ones the
    // host and its partner always had
    static const uint32_t kColors[kMaxPlayers] = {
        0xFF64B4FF, 0xFF78DC78, 0xFFFFB450, 0xFFE678E6, 0xFFFFE65A, 0xFF5AE6E6, 0xFFFF7878, 0xFFB48CFF,
        0xFFA0E650, 0xFFFF8CC8, 0xFF50C8A0, 0xFFE6A078, 0xFF8CA0FF, 0xFFDCDC96, 0xFFC878FF, 0xFFB4DCFF,
    };
    return kColors[id % kMaxPlayers];
}

void Session::Connect(const std::string& host, uint16_t port, const std::string& nick, const std::string& password,
                      bool wantsHost) {
    Disconnect();
    m_nick = nick;
    m_password = password;
    m_wantsHost = wantsHost;
    m_failReason.clear();
    m_peers.clear();
    m_address = {};
    m_worldRetries = 0;
    m_myId = m_hostId = 255;
    m_state = State::Connecting;
    if (!m_net.Start(host, port)) {
        m_state = State::Failed;
        m_failReason = TR("Не удалось запустить сеть: не инициализировался Winsock.", "Could not start the network: Winsock did not initialise.");
    }
}

void Session::Disconnect(const std::string& reason) {
    bool wasOnline = m_state == State::Online;
    m_net.Stop();
    m_net.TakeConnectEvent();
    m_net.TakeDisconnectEvent();
    if (m_state == State::Online || m_state == State::Connecting) m_state = State::Offline;
    m_peers.clear();
    m_world.clear();
    m_worldExpected = m_worldRecv = 0;
    if (wasOnline && onOffline) onOffline(reason);
}

const PeerInfo* Session::Peer(uint8_t id) const {
    auto it = m_peers.find(id);
    return it == m_peers.end() ? nullptr : &it->second;
}

std::string Session::PeerName(uint8_t id) const {
    if (id == m_myId) return m_nick;
    auto* p = Peer(id);
    return p ? p->name : Format(TR("игрок %d", "player %d"), id);
}

uint8_t Session::FindPeer(const std::string& nameOrId) const {
    std::string want = ToLower(Trim(nameOrId));
    if (want.empty()) return 255;
    for (auto& [id, p] : m_peers)  // the whole nickname first
        if (ToLower(p.name) == want) return id;
    uint8_t found = 255;
    int matches = 0;
    for (auto& [id, p] : m_peers)
        if (ToLower(p.name).rfind(want, 0) == 0) found = id, ++matches;
    if (matches == 1) return found;
    if (matches == 0 && want.find_first_not_of("0123456789") == std::string::npos && want.size() <= 2) {
        int id = atoi(want.c_str());
        if (m_peers.count((uint8_t)id)) return (uint8_t)id;
    }
    return 255;
}

proto::PlayerMask Session::PeersMask() const {
    proto::PlayerMask m = 0;
    for (auto& [id, p] : m_peers) m |= proto::Bit(id);
    return m;
}

void Session::SendTo(proto::PlayerMask mask, uint8_t channel, const Writer& w) {
    mask &= PeersMask();
    if (!mask || w.Size() == 0) return;
    if (mask == PeersMask()) return Send(channel, w);  // all of them: no mask to carry
    Writer t(C_RELAY_TO);
    t.U16(mask);
    t.Raw(w.Data(), w.Size());
    m_net.Send(channel, t);
}

void Session::SendChat(const std::string& text) {
    Writer w(C_CHAT);
    w.Str(text, kMaxChatLen * 4);
    Send(CH_RELIABLE, w);
}

void Session::Update() {
    if (m_net.TakeConnectEvent()) {
        Hello h;
        h.protocol = kProtocolVersion;
        h.version = COOP_VERSION_STR;
        h.name = m_nick;
        h.password = m_password;
        h.wantsHost = m_wantsHost;
        h.exeSize = kSupportedExeSize;
        Writer w(C_HELLO);
        h.Write(w);
        m_net.Send(CH_RELIABLE, w);
    }

    m_inbox.clear();
    m_net.Poll(m_inbox);
    for (auto& pkt : m_inbox) {
        if (pkt.data.empty()) continue;
        uint8_t id = pkt.data[0];
        if (IsRelayed(id)) {
            if (pkt.data.size() < 2 || m_state != State::Online) continue;
            uint8_t from = pkt.data[1];
            // a player who left: its late state made its character again (with its old time, so the next player under
            // that number had its states dropped as old ones)
            if (!m_peers.count(from)) continue;
            Reader r(pkt.data.data() + 2, pkt.data.size() - 2);
            if (m_handlers[id]) m_handlers[id](from, r);
        } else {
            Reader r(pkt.data.data() + 1, pkt.data.size() - 1);
            HandleService(id, r);
        }
    }

    if (m_net.TakeDisconnectEvent()) {
        bool wasOnline = m_state == State::Online;
        if (m_state == State::Connecting) {
            m_state = State::Failed;
            if (m_failReason.empty()) m_failReason = m_net.FailReason();
            if (m_failReason.empty())
                // the host's own game is in through 127.0.0.1: what keeps a friend out is mostly its firewall or antivirus
                m_failReason = TR("Сервер не отвечает. Чаще всего к нему не пускает брандмауэр или антивирус хоста: пусть хост "
                                  "создаст игру заново и подтвердит запрос Windows. Проверьте также адрес и порт.",
                                  "The server does not answer. Most often the host's firewall or antivirus keeps you out: let the "
                                  "host create the game again and confirm the Windows prompt. Also check the address and the port.");
        } else if (m_state == State::Online) {
            m_state = State::Failed;
            if (m_failReason.empty()) m_failReason = m_net.FailReason();
            if (m_failReason.empty()) m_failReason = TR("Соединение с сервером потеряно.", "Lost connection to the server.");
        }
        m_net.Stop();
        m_peers.clear();
        if (wasOnline && onOffline) onOffline(m_failReason);
    }
}

void Session::HandleService(uint8_t id, Reader& r) {
    auto& chat = ui::TheChat();
    switch (id) {
        case S_WELCOME: {
            Welcome w;
            if (!w.Read(r)) break;
            m_myId = w.yourId;
            m_hostId = w.hostId;
            m_serverName = w.serverName;
            m_friendlyFire = w.friendlyFire;
            m_maxPlayers = std::clamp<int>(w.maxPlayers, 1, kMaxPlayers);
            m_peers.clear();
            for (auto& p : w.players) m_peers[p.id] = PeerInfo{p.id, p.name, p.isHost, 0};
            m_state = State::Online;
            LOGI("Вошли на сервер \"%s\" как игрок %d, хост: %d", m_serverName.c_str(), m_myId, m_hostId);
            chat.AddSystem(IsHost() ? TR("Сервер запущен. Вы хост.", "The server is up. You are the host.")
                                    : Format(TR("Вы подключились к серверу «%s».", "You joined the server \"%s\"."),
                                             m_serverName.c_str()));
            if (!w.motd.empty()) chat.Add(Cfg().english && !w.motdEn.empty() ? w.motdEn : w.motd, 0xFFA0C8FF);
            if (m_friendlyFire)
                chat.AddSystem(TR("Урон по напарнику включён: вы можете ранить и убить друг друга.",
                                  "Friendly fire is on: you can hurt and kill each other."));
            if (onOnline) onOnline();
            break;
        }
        case S_REJECT:
        case S_KICKED: {
            std::string reason = r.Str(), en = r.Str();
            if (Cfg().english && !en.empty()) reason = en;
            if (id == S_REJECT) m_failReason = Format(TR("Не удалось войти: %s.", "Could not join: %s."), reason.c_str());
            else if (reason.empty()) m_failReason = TR("Вас выгнали с сервера.", "You were kicked from the server.");
            else m_failReason = Format(TR("Вас выгнали с сервера: %s.", "You were kicked from the server: %s."), reason.c_str());
            LOGW("%s", m_failReason.c_str());
            // shown once: a failed connect by the flow, a kick by onOffline when the connection closes
            break;
        }
        case S_PLAYER_JOIN: {
            PeerInfo p;
            p.id = r.U8();
            p.name = r.Str(64);
            p.isHost = r.Bool();
            if (!r.Ok()) break;
            m_peers[p.id] = p;
            if (p.isHost) m_hostId = p.id;
            if (onPeerJoin) onPeerJoin(p.id);
            break;
        }
        case S_PLAYER_LEAVE: {
            uint8_t pid = r.U8();
            if (onPeerLeave) onPeerLeave(pid);
            m_peers.erase(pid);
            break;
        }
        case S_HOST_CHANGED: {
            m_hostId = r.U8();
            for (auto& [pid, p] : m_peers) p.isHost = pid == m_hostId;
            if (onHostChanged) onHostChanged(m_hostId);
            break;
        }
        case S_CHAT: {
            uint8_t from = r.U8();
            uint32_t rgba = r.U32();
            std::string text = r.Str(), en = r.Str();
            if (!r.Ok()) break;
            if (from == kServerId) {
                chat.Add(Cfg().english && !en.empty() ? en : text, ui::RgbaToArgb(rgba));
            } else {
                char hex[16];
                snprintf(hex, sizeof(hex), "{%06X}", PlayerColor(from) & 0xFFFFFF);
                chat.Add(std::string(hex) + PeerName(from) + "{FFFFFF}: " + text, 0xFFFFFFFF);
            }
            break;
        }
        case S_PINGS: {
            uint8_t n = r.U8();
            for (uint8_t i = 0; i < n && r.Ok(); ++i) {
                uint8_t pid = r.U8();
                uint16_t ping = r.U16();
                auto it = m_peers.find(pid);
                if (it != m_peers.end()) it->second.ping = ping;
            }
            break;
        }
        case S_PROFILE: {
            auto blob = r.Bytes(64 * 1024);
            if (r.Ok() && onProfile) onProfile(blob);
            break;
        }
        case S_WORLD_BEGIN: {
            m_worldExpected = r.U32();
            m_worldCrc = r.U32();
            m_worldLabel = r.Str(128);
            m_worldRecv = 0;
            if (m_worldExpected > 8 * 1024 * 1024) m_worldExpected = 0;
            m_world.assign(m_worldExpected, 0);
            break;
        }
        case S_WORLD_CHUNK: {
            uint32_t off = r.U32();
            auto bytes = r.Bytes(kWorldChunk);
            // off + size wraps in 32 bits: the bound is checked without the sum
            if (!r.Ok() || off > m_world.size() || bytes.size() > m_world.size() - off) break;
            memcpy(m_world.data() + off, bytes.data(), bytes.size());
            m_worldRecv += (uint32_t)bytes.size();
            break;
        }
        case S_WORLD_END: {
            uint32_t crc = Crc32(m_world.data(), m_world.size());
            if (crc != m_worldCrc) {
                LOGE("Сохранение мира повреждено при передаче (crc %08X != %08X)", crc, m_worldCrc);
                // asked again three times at most: a world too big to take (over 8 MB) came "damaged" for ever
                if (++m_worldRetries <= 3) {
                    chat.AddError(TR("Мир пришёл с ошибкой. Запрашиваем ещё раз...", "The world arrived damaged. Asking for it again..."));
                    Writer w(C_WORLD_REQUEST);
                    Send(CH_BULK, w);
                } else {
                    chat.AddError(TR("Мир снова пришёл с ошибкой. Попробуйте подключиться заново.",
                                     "The world arrived damaged again. Try to connect again."));
                }
            } else {
                m_worldRetries = 0;
                if (onWorld) onWorld(m_world, m_worldLabel);
            }
            m_world.clear();
            m_worldExpected = m_worldRecv = 0;
            break;
        }
        case S_WORLD_NONE:
            if (onWorldNone) onWorldNone();
            break;
        case S_WORLD_NEWGAME:
            if (onWorldNewGame) onWorldNewGame();
            break;
        case S_WORLD_WAIT:
            if (onWorldWait) onWorldWait();
            break;
        case S_ADDRESS: {
            Address a;
            a.external = r.Str(32);
            a.lan = r.Str(32);
            uint8_t flags = r.U8();
            a.upnpError = r.Str(128);
            if (!r.Ok()) break;
            a.done = flags & ADDRESS_DONE;
            a.upnp = flags & ADDRESS_UPNP;
            a.cgnat = flags & ADDRESS_CGNAT;
            m_address = a;
            if (onAddress) onAddress(a);
            break;
        }
        case S_WORLD_WANTED:
            if (onWorldWanted) onWorldWanted();
            break;
        default:
            LOGW("Unknown service message %u", id);
            break;
    }
}

}  // namespace coop
