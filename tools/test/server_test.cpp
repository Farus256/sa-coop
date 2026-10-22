// coop-server-test.exe — checks coop-server.exe from the outside, as a few game clients would see it: places, the
// password, the ping before connecting, relaying to everyone and to some players (C_RELAY_TO), the host's role, profile files.
//
//   python tools/build.py servertest
//   build/release/bin/coop-server-test.exe [path to coop-server.exe]   (runs on Windows, and under Wine)
//
// It starts its own servers on free ports with their data in a temporary folder and stops them at the end.
#include <enet/enet.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/api/nat-traversal/NatTraversal.h"
#include "common/api/net/Net.h"
#include "common/api/protocol/Protocol.h"
#include "common/config/version/Version.h"
#include "common/lib/buffer/Buffer.h"
#include "common/lib/util/Util.h"

using namespace coop;
using namespace coop::proto;

namespace {

int g_failed = 0, g_passed = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (cond) {                                          \
            ++g_passed;                                      \
        } else {                                             \
            ++g_failed;                                      \
            printf("  FAIL %s:%d: %s — ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

struct Received {
    uint8_t channel;
    std::vector<uint8_t> data;
};

// one game client: its own ENet host and peer
struct Bot {
    std::string name;
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false, disconnected = false;
    bool welcomed = false, rejected = false;
    Welcome welcome;
    std::string rejectReason;
    std::vector<Received> inbox;  // everything after the handshake

    ~Bot() {
        if (host) enet_host_destroy(host);
    }
    void Service(int ms = 0) {
        ENetEvent ev;
        while (enet_host_service(host, &ev, (enet_uint32)ms) > 0) {
            ms = 0;
            if (ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) disconnected = true, connected = false;
            if (ev.type != ENET_EVENT_TYPE_RECEIVE) continue;
            std::vector<uint8_t> d(ev.packet->data, ev.packet->data + ev.packet->dataLength);
            uint8_t ch = ev.channelID;
            enet_packet_destroy(ev.packet);
            if (d.empty()) continue;
            if (d[0] == S_WELCOME) {
                Reader r(d.data() + 1, d.size() - 1);
                welcomed = welcome.Read(r);
            } else if (d[0] == S_REJECT) {
                Reader r(d.data() + 1, d.size() - 1);
                rejected = true;
                rejectReason = r.Str();
                std::string en = r.Str();
                if (!en.empty()) rejectReason = en;
            } else {
                inbox.push_back({ch, std::move(d)});
            }
        }
    }
    void Send(uint8_t channel, const Writer& w) {
        enet_peer_send(peer, channel, net::MakePacket(w.Data(), w.Size(), channel));
        enet_host_flush(host);
    }
    bool Has(uint8_t id, uint8_t from = 255) const {
        for (auto& r : inbox)
            if (r.data[0] == id && (from == 255 || (r.data.size() > 1 && r.data[1] == from))) return true;
        return false;
    }
    int Count(uint8_t id) const {
        int n = 0;
        for (auto& r : inbox) n += r.data[0] == id;
        return n;
    }
};

struct World {
    std::vector<std::unique_ptr<Bot>> bots;
    void Pump(int ms) {
        uint64_t end = NowMs() + (uint64_t)ms;
        do {
            for (auto& b : bots) b->Service(0);
            Sleep(5);
        } while (NowMs() < end);
    }
    template <typename F>
    bool Until(F done, int ms) {
        uint64_t end = NowMs() + (uint64_t)ms;
        while (NowMs() < end) {
            for (auto& b : bots) b->Service(0);
            if (done()) return true;
            Sleep(5);
        }
        return done();
    }
};

Bot& Join(World& w, uint16_t port, const std::string& name, const std::string& password = "",
          bool wantsHost = false) {
    auto b = std::make_unique<Bot>();
    b->name = name;
    b->host = enet_host_create(nullptr, 1, CH_COUNT, 0, 0);
    enet_host_compress_with_range_coder(b->host);
    ENetAddress a{};
    enet_address_set_host(&a, "127.0.0.1");
    a.port = port;
    b->peer = enet_host_connect(b->host, &a, CH_COUNT, 0);
    Bot& bot = *b;
    w.bots.push_back(std::move(b));
    w.Until([&] { return bot.connected || bot.disconnected; }, 5000);
    if (!bot.connected) return bot;
    Hello h;
    h.protocol = kProtocolVersion;
    h.version = COOP_VERSION_STR;
    h.name = name;
    h.password = password;
    h.wantsHost = wantsHost;
    h.exeSize = kSupportedExeSize;
    Writer hw(C_HELLO);
    h.Write(hw);
    bot.Send(CH_RELIABLE, hw);
    w.Until([&] { return bot.welcomed || bot.rejected || bot.disconnected; }, 5000);
    return bot;
}

// a server process of its own
struct ServerProcess {
    PROCESS_INFORMATION pi{};
    uint16_t port = 0;
    std::wstring data;
    bool Start(const std::wstring& exe, uint16_t p, const std::wstring& args) {
        port = p;
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        data = std::wstring(tmp) + L"coop-server-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(p) + L"\\";
        EnsureDirectory(data);
        std::wstring ini = data + L"server.ini";  // no internet: no UPnP or STUN while testing
        std::string text = "[server]\r\ninternet = 0\r\n";
        WriteFileBytes(ini, text.data(), text.size());
        // no backslash before a closing quote: it would escape the quote and take the next arguments along
        std::wstring dir = data.substr(0, data.size() - 1);
        std::wstring cmd = L"\"" + exe + L"\" --port " + std::to_wstring(p) + L" --data \"" + dir + L"\" --config \"" + ini +
                           L"\" " + args;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        STARTUPINFOW si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;  // its console output stays out of ours
        si.hStdInput = si.hStdOutput = si.hStdError = INVALID_HANDLE_VALUE;
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            printf("cannot start %ls (error %lu)\n", exe.c_str(), GetLastError());
            return false;
        }
        Sleep(800);  // it opens its port
        return true;
    }
    ~ServerProcess() {
        if (pi.hProcess) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
};

Writer GameMsg(uint8_t id, uint32_t tag) {
    Writer w(id);
    w.U32(tag);
    return w;
}

bool GotTag(const Bot& b, uint8_t id, uint8_t from, uint32_t tag) {
    for (auto& r : b.inbox)
        if (r.data.size() >= 6 && r.data[0] == id && r.data[1] == from && *(const uint32_t*)&r.data[2] == tag) return true;
    return false;
}

// ---------------------------------------------------------------- the checks
// the ids the games give their vehicles and peds: never 0, the owner in the top bits, no two owners alike
void EntityIds() {
    printf("entity ids\n");
    bool zero = false, owners = true;
    for (int owner = 0; owner < kMaxPlayers; ++owner)
        for (uint32_t serial = 0; serial < 9000; ++serial) {
            uint16_t id = EntityId((uint8_t)owner, (uint16_t)serial);
            zero |= id == 0;
            owners &= (id >> 12) == owner;
        }
    CHECK(!zero, "an id was 0");
    CHECK(owners, "an id did not carry its owner");
    CHECK(EntityId(3, 0) != EntityId(3, 1) && EntityId(3, 4094) != EntityId(3, 4095) - 0, "serials repeat too early");
    CHECK(Bit(15) == 0x8000 && Bit(16) == 0 && Bit(255) == 0, "player bits");
}

void Places(const std::wstring& exe) {
    printf("places (--max 4)\n");
    ServerProcess s;
    if (!s.Start(exe, 7811, L"--max 4")) return ++g_failed, void();
    World w;
    for (int i = 0; i < 4; ++i) {
        Bot& b = Join(w, s.port, "bot" + std::to_string(i));
        CHECK(b.welcomed, "bot%d joins (%s)", i, b.rejectReason.c_str());
        if (b.welcomed) CHECK(b.welcome.maxPlayers == 4, "welcome says %d places", b.welcome.maxPlayers);
    }
    Bot& fifth = Join(w, s.port, "bot4");
    CHECK(!fifth.welcomed, "a fifth one is not let in");
    w.Pump(200);
    CHECK(fifth.rejected || fifth.disconnected, "and is told so (%s)", fifth.rejectReason.c_str());
    Bot& again = Join(w, s.port, "BOT1");  // nicknames are case-insensitive
    CHECK(!again.welcomed, "the same nickname twice");
    // the first one is the host, the others know
    CHECK(w.bots[0]->welcome.hostId == w.bots[0]->welcome.yourId, "the first one is the host");
    CHECK(w.bots[3]->welcome.hostId == w.bots[0]->welcome.yourId, "the fourth knows the host");
    CHECK(w.bots[3]->welcome.players.size() == 3, "the fourth sees %zu players", w.bots[3]->welcome.players.size());
}

void Sixteen(const std::wstring& exe) {
    printf("sixteen players, relaying\n");
    ServerProcess s;
    if (!s.Start(exe, 7812, L"--max 16")) return ++g_failed, void();
    World w;
    for (int i = 0; i < 16; ++i) {
        Bot& b = Join(w, s.port, "p" + std::to_string(i));
        CHECK(b.welcomed, "p%d joins (%s)", i, b.rejectReason.c_str());
    }
    if (g_failed) return;
    std::vector<uint8_t> ids;
    for (auto& b : w.bots) ids.push_back(b->welcome.yourId);
    CHECK(ids[15] == 15 || ids[15] < 16, "ids within 0..15 (%d)", ids[15]);
    w.Pump(300);
    for (auto& b : w.bots) b->inbox.clear();

    // to everyone
    w.bots[3]->Send(CH_RELIABLE, GameMsg(G_PLAYER_SPAWN, 0x11111111));
    w.Until([&] {
        for (size_t i = 0; i < w.bots.size(); ++i)
            if (i != 3 && !GotTag(*w.bots[i], G_PLAYER_SPAWN, ids[3], 0x11111111)) return false;
        return true;
    }, 3000);
    int got = 0;
    for (size_t i = 0; i < w.bots.size(); ++i) got += i != 3 && GotTag(*w.bots[i], G_PLAYER_SPAWN, ids[3], 0x11111111);
    CHECK(got == 15, "a message of p3 reached %d of 15", got);
    CHECK(!w.bots[3]->Has(G_PLAYER_SPAWN), "not back to its sender");

    // to some: p5 -> {p0, p9, p15}
    Writer inner = GameMsg(G_VEH_SYNC, 0x22222222);
    Writer to(C_RELAY_TO);
    to.U16((PlayerMask)(Bit(ids[0]) | Bit(ids[9]) | Bit(ids[15]) | Bit(ids[5])));  // the sender's own bit is ignored
    to.Raw(inner.Data(), inner.Size());
    w.bots[5]->Send(CH_RELIABLE, to);
    w.Until([&] { return GotTag(*w.bots[0], G_VEH_SYNC, ids[5], 0x22222222) && GotTag(*w.bots[9], G_VEH_SYNC, ids[5], 0x22222222) &&
                         GotTag(*w.bots[15], G_VEH_SYNC, ids[5], 0x22222222); }, 3000);
    w.Pump(300);
    for (size_t i = 0; i < w.bots.size(); ++i) {
        bool want = i == 0 || i == 9 || i == 15;
        CHECK(GotTag(*w.bots[i], G_VEH_SYNC, ids[5], 0x22222222) == want, "p%zu %s the targeted message", i, want ? "missed" : "got");
    }
    // a service message inside C_RELAY_TO is not relayed
    Writer bad(C_RELAY_TO);
    bad.U16(kAllPlayers);
    Writer chat(C_CHAT);
    chat.Str("/say hacked");
    bad.Raw(chat.Data(), chat.Size());
    w.bots[5]->Send(CH_RELIABLE, bad);
    w.Pump(400);
    int chats = 0;
    for (auto& b : w.bots) chats += b->Has(C_CHAT);
    CHECK(chats == 0, "a service message passed through C_RELAY_TO");

    // unreliable state of everyone at 30 Hz for a second: nobody drops off. States go to players in the game only: the
    // last one stays in the menu and gets none
    for (size_t i = 0; i + 1 < w.bots.size(); ++i) {
        Writer st(C_STATUS);
        st.U8(STATUS_INGAME);
        w.bots[i]->Send(CH_RELIABLE, st);
    }
    w.Pump(300);
    for (int frame = 0; frame < 30; ++frame) {
        for (auto& b : w.bots) {
            Writer st = GameMsg(G_PLAYER_ONFOOT, (uint32_t)frame);
            st.Raw(std::vector<uint8_t>(60, 0xAB).data(), 60);
            b->Send(CH_STATE, st);
        }
        w.Pump(33);
    }
    w.Pump(500);
    int dropped = 0;
    for (auto& b : w.bots) dropped += b->disconnected;
    CHECK(dropped == 0, "%d dropped under load", dropped);
    int states = 0;
    for (size_t i = 0; i + 1 < w.bots.size(); ++i) states += w.bots[i]->Count(G_PLAYER_ONFOOT);
    CHECK(states > 15 * 15 * 30 * 8 / 10, "states relayed: %d of %d", states, 15 * 15 * 30);
    CHECK(w.bots.back()->Count(G_PLAYER_ONFOOT) == 0, "states reached a player in the menu");
}

void Password(const std::wstring& exe) {
    printf("password\n");
    ServerProcess s;
    if (!s.Start(exe, 7813, L"--password secret")) return ++g_failed, void();
    World w;
    Bot& ok = Join(w, s.port, "friend", "secret");
    CHECK(ok.welcomed, "the right password (%s)", ok.rejectReason.c_str());
    Bot& none = Join(w, s.port, "none");
    CHECK(!none.welcomed && none.rejectReason.find("password") != std::string::npos, "no password (%s)", none.rejectReason.c_str());
    Bot& wrong = Join(w, s.port, "wrong", "guess");
    CHECK(!wrong.welcomed && wrong.rejectReason.find("wrong password") != std::string::npos, "a wrong one (%s)", wrong.rejectReason.c_str());
    for (int i = 0; i < 8; ++i) Join(w, s.port, "guess" + std::to_string(i), "guess" + std::to_string(i));
    Bot& late = Join(w, s.port, "late", "secret");
    CHECK(!late.welcomed, "the right password after 8 wrong ones from the address (%s)", late.rejectReason.c_str());

    printf("no password\n");
    ServerProcess open;
    if (!open.Start(exe, 7814, L"")) return ++g_failed, void();
    World w2;
    Bot& any = Join(w2, open.port, "anyone");
    CHECK(any.welcomed, "joins by address (%s)", any.rejectReason.c_str());
}

// what the game sends to the server port when a connection fails: the server answers, a closed port answers with ICMP
void Ping(const std::wstring& exe) {
    printf("ping before connecting\n");
    ServerProcess s;
    if (!s.Start(exe, 7816, L"")) return ++g_failed, void();
    auto ask = [](uint16_t port, std::string& answer) {
        SOCKET so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(so, (sockaddr*)&sa, sizeof sa);
        send(so, nat::kPingMagic, sizeof(nat::kPingMagic) - 1, 0);
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(so, &rd);
        timeval tv{1, 0};
        int err = 0;
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
            char buf[64];
            int n = recv(so, buf, sizeof buf, 0);
            if (n > 0) answer.assign(buf, n);
            else err = WSAGetLastError();
        }
        closesocket(so);
        return err;
    };
    std::string answer;
    ask(s.port, answer);
    CHECK(answer == nat::kPongMagic, "the server answers the ping (%s)", answer.c_str());
    answer.clear();
    int err = ask(7899, answer);  // nothing listens there
    CHECK(answer.empty(), "no answer from a closed port (%s)", answer.c_str());
    if (err != WSAECONNRESET) printf("  note: a closed port gave error %d here, not WSAECONNRESET (Wine?)\n", err);
}

void OldClient(const std::wstring& exe) {
    printf("a client of the original mod\n");
    ServerProcess s;
    if (!s.Start(exe, 7815, L"")) return ++g_failed, void();
    World w;
    auto b = std::make_unique<Bot>();
    b->host = enet_host_create(nullptr, 1, CH_COUNT, 0, 0);
    enet_host_compress_with_range_coder(b->host);
    ENetAddress a{};
    enet_address_set_host(&a, "127.0.0.1");
    a.port = s.port;
    b->peer = enet_host_connect(b->host, &a, CH_COUNT, 0);
    Bot& bot = *b;
    w.bots.push_back(std::move(b));
    w.Until([&] { return bot.connected; }, 5000);
    Writer hw(C_HELLO);  // protocol 17's hello
    hw.U32(17);
    hw.Str("0.2.6", 32);
    hw.Str("oldie", 64);
    hw.Str("", 64);
    hw.Bool(false);
    hw.U32(kSupportedExeSize);
    bot.Send(CH_RELIABLE, hw);
    w.Until([&] { return bot.rejected; }, 5000);
    CHECK(bot.rejected && bot.rejectReason.find("version") != std::string::npos, "told about the versions (%s)",
          bot.rejectReason.c_str());
}

void HostChange(const std::wstring& exe) {
    printf("the host leaves\n");
    ServerProcess s;
    if (!s.Start(exe, 7816, L"--max 4")) return ++g_failed, void();
    World w;
    Join(w, s.port, "first");
    Bot& b = Join(w, s.port, "second");
    Join(w, s.port, "third");
    w.Pump(300);
    uint8_t firstId = w.bots[0]->welcome.yourId;
    enet_peer_disconnect(w.bots[0]->peer, 1);
    w.Until([&] { return b.Has(S_HOST_CHANGED); }, 5000);
    bool left = false, changed = false;
    for (auto& r : b.inbox) {
        if (r.data[0] == S_PLAYER_LEAVE && r.data.size() > 1 && r.data[1] == firstId) left = true;
        if (r.data[0] == S_HOST_CHANGED) changed = true;
    }
    CHECK(left, "the others hear that the host left");
    CHECK(changed, "and who the host is now");
}

void Profiles(const std::wstring& exe) {
    printf("profile files\n");
    ServerProcess s;
    if (!s.Start(exe, 7817, L"--max 4")) return ++g_failed, void();
    World w;
    Join(w, s.port, "host");
    // a device name (profiles\con.bin would be the console) is refused as a nickname
    Bot& con = Join(w, s.port, "con");
    CHECK(!con.welcomed && !con.rejectReason.empty(), "the nickname CON was let in");
    Bot& ok = Join(w, s.port, "Player_2");
    Writer p(C_PROFILE_SAVE);
    std::vector<uint8_t> blob(100, 7);
    p.Bytes(blob.data(), blob.size());
    ok.Send(CH_RELIABLE, p);
    w.Pump(500);
    CHECK(FileExists(s.data + L"profiles\\player_2.bin") || FileExists(s.data + L"profiles\\Player_2.bin"),
          "a profile is saved");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring exe = argc > 1 ? argv[1] : ModuleDir() + L"coop-server.exe";
    if (!FileExists(exe)) {
        printf("no server at %ls\n", exe.c_str());
        return 2;
    }
    net::Init();
    EntityIds();
    Places(exe);
    Sixteen(exe);
    Password(exe);
    Ping(exe);
    OldClient(exe);
    HostChange(exe);
    Profiles(exe);
    net::Shutdown();
    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
