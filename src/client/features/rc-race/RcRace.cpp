#include "features/rc-race/RcRace.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/radar-icons/RadarIcons.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::rc {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int MODEL_RCBANDIT = 441;
const CVector kStart{-2240.0f, 140.0f, 35.3f};  // by Zero's RC shop, Garcia
// the course round the blocks of Garcia (each put on the nearest road), back to the start
const CVector kCourse[] = {{-2240.0f, 240.0f, 35.0f}, {-2140.0f, 240.0f, 35.0f}, {-2040.0f, 240.0f, 35.0f},
                           {-2040.0f, 140.0f, 35.0f}, {-2040.0f, 40.0f, 35.0f},  {-2140.0f, 40.0f, 35.0f},
                           {-2240.0f, 40.0f, 35.0f},  {-2240.0f, 140.0f, 35.0f}};
constexpr uint64_t kJoinMs = 8000, kCountMs = 3000, kLimitMs = 4 * 60 * 1000;
constexpr float kReach = 7.0f;
enum Op : uint8_t { OP_CALL = 1, OP_JOIN, OP_TIME, OP_OUT };
enum Phase { OFF, JOINING, COUNTDOWN, RACING, DONE };

Phase g_phase = OFF;
uint8_t g_caller = 255;  // whose race (it decides when it starts)
uint64_t g_callMs = 0;     // when it called it
uint64_t g_phaseMs = 0, g_startMs = 0;
std::vector<CVector> g_points;
int g_next = 0, g_checkpoint = -1;
std::map<uint8_t, uint32_t> g_times;  // the finishers' times (ms)
std::vector<uint8_t> g_racers;

CVector Snap(const CVector& p) {
    float x = 0, y = 0, z = 0;
    Cmd(0x02C1).F(p.x).F(p.y).F(p.z).Out(&x).Out(&y).Out(&z).Run();  // GET_CLOSEST_CAR_NODE
    return (x || y) ? CVector{x, y, z} : p;
}

bool AtStart(CPlayerPed* me) {
    return me && me->AreaCode() == 0 && !me->InVehicle() && (me->GetPosition() - kStart).Length() < 6.0f;
}

void ClearCheckpoint() {
    if (g_checkpoint >= 0) Cmd(0x06D6).I(g_checkpoint).Run();  // DELETE_CHECKPOINT
    g_checkpoint = -1;
}
void ShowCheckpoint() {
    ClearCheckpoint();
    if (g_next >= (int)g_points.size()) return;
    const CVector& p = g_points[g_next];
    const CVector& q = g_next + 1 < (int)g_points.size() ? g_points[g_next + 1] : p;
    bool last = g_next + 1 == (int)g_points.size();
    Cmd(0x06D5).I(last ? 1 : 0).F(p.x).F(p.y).F(p.z).F(q.x).F(q.y).F(q.z).F(4.0f).Out(&g_checkpoint).Run();  // CREATE_CHECKPOINT
}

CVehicle* MyRc() {
    int h = -1;
    Cmd(0x0484).I(0).Out(&h).Run();  // GET_REMOTE_CONTROLLED_CAR
    return h > 0 ? GetVehicle(h) : nullptr;
}

void Send(Op op, uint32_t value = 0) {
    Writer w(proto::G_RC_RACE);
    w.U8(op);
    w.U32(value);
    Net().Send(proto::CH_RELIABLE, w);
}

void Stop(bool keepResults) {
    ClearCheckpoint();
    if (g_phase == RACING || g_phase == COUNTDOWN) {
        if (MyRc()) Cmd(0x04DB).Run();  // REMOVE_RC_BUGGY
    }
    GetPad(0)->DisablePlayerControls &= (uint16_t)~0x1000;
    g_phase = keepResults ? DONE : OFF;
    g_phaseMs = NowMs();
}

void BeginCountdown() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return Stop(false);
    g_points.clear();
    for (const CVector& p : kCourse) g_points.push_back(Snap(p));
    g_next = 0;
    LoadModelNow(MODEL_RCBANDIT);
    // side by side on the start's road, each by its number
    CVector at = Snap(kStart), ahead = g_points[0] - at;
    ahead.z = 0;
    float len = std::max(0.1f, ahead.Length2D());
    CVector dir{ahead.x / len, ahead.y / len, 0}, side{-dir.y, dir.x, 0};
    int order = 0;
    for (size_t i = 0; i < g_racers.size(); ++i)
        if (g_racers[i] == Net().MyId()) order = (int)i;
    CVector p = at + side * (-1.5f * (float)(g_racers.size() - 1) + 3.0f * order);
    float heading = std::atan2(-dir.x, dir.y) * 57.29578f;
    Cmd(0x046E).I(0).F(p.x).F(p.y).F(p.z + 0.5f).F(heading).I(MODEL_RCBANDIT).Run();  // GIVE_REMOTE_CONTROLLED_MODEL_TO_PLAYER
    GetPad(0)->DisablePlayerControls |= 0x1000;  // held until the start
    g_phase = COUNTDOWN;
    g_phaseMs = NowMs();
    ShowCheckpoint();
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint32_t v = r.U32();
    if (!r.Ok() || !core::IsPlayerReady()) return;
    if (op == OP_CALL) {
        if (g_phase == OFF || g_phase == DONE) {
            g_caller = from;
            g_callMs = NowMs();
            g_racers.clear();
            g_times.clear();
            if (AtStart(FindPlayerPed(0)))
                ui::TheChat().AddSystem(Format(TR("%s зовёт на гонку радиоуправляемых машинок: %s — участвовать.",
                                                  "%s calls a toy car race: %s to join."),
                                               Net().PeerName(from).c_str(), "E"));
        }
    } else if (op == OP_JOIN) {
        if (std::find(g_racers.begin(), g_racers.end(), from) == g_racers.end()) g_racers.push_back(from);
        std::sort(g_racers.begin(), g_racers.end());
    } else if (op == OP_TIME) {
        g_times[from] = v;
        ui::TheChat().AddSystem(Format(TR("%s финишировал: %d:%05.2f", "%s finished: %d:%05.2f"), Net().PeerName(from).c_str(),
                                       (int)(v / 60000), (v % 60000) / 1000.0f));
    } else if (op == OP_OUT) {
        ui::TheChat().AddSystem(Format(TR("%s сошёл с дистанции.", "%s is out of the race."), Net().PeerName(from).c_str()));
    }
}

void Finish(uint32_t ms) {
    int place = 1;
    for (auto& [id, t] : g_times)
        if (t < ms) ++place;
    g_times[Net().MyId()] = ms;
    int prize = place == 1 ? 1000 : place == 2 ? 600 : 400;
    PlayerInfos()[0].money += prize;
    Send(OP_TIME, ms);
    ui::TheChat().AddSystem(Format(TR("Финиш! %d:%05.2f, место %d. Награда $%d.", "Finish! %d:%05.2f, place %d. Prize $%d."),
                                   (int)(ms / 60000), (ms % 60000) / 1000.0f, place, prize));
    LOGI("RC race finished in %u ms (place %d)", ms, place);
    Stop(true);
}

class RcRace : public Module {
public:
    const char* Name() const override { return "RcRace"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_RC_RACE, &Handle); }
    void OnGameStarted() override { radar::SetIcon(9500, kStart, 33 /* RACE */); }
    void OnGameShutdown() override {
        g_phase = OFF;
        g_checkpoint = -1;
    }
    void OnPostProcess() override {
        if (!core::IsPlayerReady() || g_phase == OFF) return;
        uint64_t now = NowMs();
        CPlayerPed* me = FindPlayerPed(0);
        if (!me || me->Health() <= 0.0f) return Stop(false);
        switch (g_phase) {
            case JOINING:
                if (now - g_phaseMs >= kJoinMs) BeginCountdown();
                break;
            case COUNTDOWN:
                if (now - g_phaseMs >= kCountMs) {
                    GetPad(0)->DisablePlayerControls &= (uint16_t)~0x1000;
                    g_phase = RACING;
                    g_startMs = now;
                }
                break;
            case RACING: {
                CVehicle* car = MyRc();
                if (!car || car->Health() <= 0.0f || now - g_startMs > kLimitMs) {
                    Send(OP_OUT);
                    ui::TheChat().AddSystem(TR("Машинка разбита или время вышло.", "The toy car is wrecked or the time is up."));
                    Stop(true);
                    break;
                }
                if ((car->GetPosition() - g_points[g_next]).Length2D() < kReach) {
                    if (++g_next >= (int)g_points.size()) Finish((uint32_t)(now - g_startMs));
                    else ShowCheckpoint();
                }
                break;
            }
            case DONE:
                if (now - g_phaseMs > 8000) g_phase = OFF;
                break;
            default: break;
        }
    }
    void OnRender() override {
        if (!core::IsPlayerReady() || MenuActive()) return;
        auto& g = ui::Gfx();
        uint64_t now = NowMs();
        std::string big, small;
        if (g_phase == JOINING) {
            big = Format(TR("Гонка через %d с", "The race in %d s"), (int)((kJoinMs - std::min(kJoinMs, now - g_phaseMs)) / 1000 + 1));
            small = Format(TR("Участников: %d", "Racers: %d"), (int)g_racers.size());
        } else if (g_phase == COUNTDOWN) {
            big = Format("%d", 3 - (int)std::min<uint64_t>(2, (now - g_phaseMs) / 1000));
        } else if (g_phase == RACING) {
            uint64_t t = now - g_startMs;
            big = Format("%d:%05.2f", (int)(t / 60000), (t % 60000) / 1000.0f);
            small = Format(TR("Чекпоинт %d из %d", "Checkpoint %d of %d"), g_next + 1, (int)g_points.size());
        } else if (g_phase == OFF && AtStart(FindPlayerPed(0))) {
            small = TR("E — гонка на радиоуправляемых машинках", "E — a toy car race");
        }
        if (!big.empty()) g.Text(ui::FONT_BIG, g.Width() * 0.5f, g.Height() * 0.12f, big, 0xFFFFFFFF, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        if (!small.empty())
            g.Text(ui::FONT_NORMAL, g.Width() * 0.5f, g.Height() * (big.empty() ? 0.82f : 0.12f + 0.06f), small, ui::w::kSaBlue,
                   ui::TEXT_CENTER | ui::TEXT_OUTLINE);
    }
};

RcRace g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

bool KeyDown(int vk) {
    if (vk != Cfg().reviveKey || !core::IsPlayerReady()) return false;
    CPlayerPed* me = FindPlayerPed(0);
    if ((g_phase != OFF && g_phase != DONE) || !AtStart(me) || GetPad(0)->DisablePlayerControls) return false;
    bool joining = g_caller != 255 && g_caller != Net().MyId() && NowMs() - g_callMs < kJoinMs && Net().Peer(g_caller);
    if (!joining) {
        g_caller = Net().MyId();
        g_callMs = NowMs();
        g_racers.clear();
        g_times.clear();
        Send(OP_CALL);
    }
    g_phase = JOINING;
    g_phaseMs = g_callMs;  // the caller's countdown
    if (std::find(g_racers.begin(), g_racers.end(), Net().MyId()) == g_racers.end()) g_racers.push_back(Net().MyId());
    std::sort(g_racers.begin(), g_racers.end());
    Send(OP_JOIN);
    ui::TheChat().AddSystem(TR("Вы в гонке! Старт через несколько секунд.", "You are in the race! The start is in a few seconds."));
    return true;
}

}  // namespace coop::rc
