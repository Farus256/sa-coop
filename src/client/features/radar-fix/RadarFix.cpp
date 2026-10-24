// The radar's map is drawn only for a player in the open world: CGame::currArea 0 and the player's own area code 0
// (CRadar::DrawMap). The two always go together in the game (an entrance, SET_AREA_VISIBLE with
// SET_CHAR_AREA_VISIBLE). A partner's player could keep another area code out in the world (a cutscene's, the one it
// was brought into beside the host, 13 "seen everywhere"), and its radar showed the blips on nothing. Out in the world
// a while with such a code, the player takes the world's one again.
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kCurrArea = 0xB72914;          // CGame::currArea
constexpr uintptr_t kCutsceneRunning = 0xB5F851;   // CCutsceneMgr::ms_running
uint64_t g_wrongSince = 0, g_checkMs = 0;

class RadarFix : public Module {
public:
    const char* Name() const override { return "RadarFix"; }
    void OnPostProcess() override {
        uint64_t now = NowMs();
        if (now - g_checkMs < 250) return;
        g_checkMs = now;
        CPlayerPed* me = Net().Online() && core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
        bool wrong = me && Ref<int32_t>(kCurrArea) == 0 && me->AreaCode() != 0 && !Ref<uint8_t>(kCutsceneRunning) &&
                     !(GetPad(0)->DisablePlayerControls & 0x3FFF);  // nothing scripted going on
        if (!wrong) {
            g_wrongSince = 0;
            return;
        }
        if (!g_wrongSince) g_wrongSince = now;
        if (now - g_wrongSince < 1500) return;
        LOGW("Player's area code %d out in the world: back to 0 (the radar's map)", me->AreaCode());
        Cmd(0x0860).I(GetPedRef(me)).I(0).Run();  // SET_CHAR_AREA_VISIBLE
        me->AreaCode() = 0;
        g_wrongSince = 0;
    }
};

RadarFix g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
