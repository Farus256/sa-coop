#include "features/progress/SkinChoice.h"

#include <windows.h>

#include <cstring>
#include <iterator>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/profile/Profile.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop::skin {

namespace {
using namespace game;
using game::script::Cmd;

struct Text {
    const char* ru;
    const char* en;
};
struct Choice {
    uint16_t model;
    Text name;
};
const Choice kChoices[] = {{0, TRP("Карл Джонсон (CJ)", "Carl Johnson (CJ)")},
                           {105, TRP("Grove Street Families, боец 1", "Grove Street Families, homie 1")},
                           {106, TRP("Grove Street Families, боец 2", "Grove Street Families, homie 2")},
                           {107, TRP("Grove Street Families, боец 3", "Grove Street Families, homie 3")}};
constexpr uint16_t kOwnControls = 0x1000;  // our bit of CPad::DisablePlayerControls
constexpr uint32_t kProofBits = (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 23);

bool g_want = false, g_active = false;
size_t g_index = 0;
uint32_t g_savedProofs = 0;
uint64_t g_shopNoteMs = 0;
SHORT g_keys[5] = {};  // left, right, enter, A, D: last state

bool KeyPressed(int i, int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool was = g_keys[i] != 0;
    g_keys[i] = down;
    return down && !was;
}

void Camera(CPlayerPed* me) {
    CMatrix* m = me->Matrix();
    CVector fwd = m ? m->forward : CVector{0, 1, 0};
    CVector p = me->GetPosition();
    CVector from = p + fwd * 2.6f + CVector{0, 0, 0.35f}, at = p + CVector{0, 0, 0.15f}, up{};
    void* cam = (void*)addr::TheCamera;
    CallMethod<0x50BEC0>(cam, &from, &up);    // CCamera::SetCamPositionForFixedMode
    CallMethod<0x50C8B0>(cam, &at, 2, 2);     // CCamera::TakeControlNoEntity: a jump cut, by a script
}

void Show(size_t i) {
    g_index = i;
    Wear(kChoices[i].model);
}

void Start(CPlayerPed* me) {
    g_want = false;
    g_active = true;
    g_savedProofs = me->PhysicalFlags() & kProofBits;
    me->PhysicalFlags() |= kProofBits;
    GetPad(0)->DisablePlayerControls |= kOwnControls;
    Cmd(0x0792).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
    for (SHORT& k : g_keys) k = 1;       // a key held from before is no press
    Show(0);
    Camera(me);
    LOGI("Character choice: open");
}

void Finish(CPlayerPed* me) {
    g_active = false;
    me->PhysicalFlags() = (me->PhysicalFlags() & ~kProofBits) | g_savedProofs;
    GetPad(0)->DisablePlayerControls &= (uint16_t)~kOwnControls;
    CallMethod<0x50BAB0>((void*)addr::TheCamera);  // CCamera::RestoreWithJumpCut
    const Choice& c = kChoices[g_index];
    ui::TheChat().AddSystem(Format(TR("Вы играете за: %s.", "You play as: %s."), TR(c.name.ru, c.name.en)));
    if (c.model)
        ui::TheChat().AddSystem(TR("Магазины одежды, парикмахерские и тату-салоны открыты только для CJ.",
                                   "Clothes shops, barbers and tattoo parlours are for CJ only."));
    SaveProfileNow();
    LOGI("Character choice: model %d", c.model);
}

// ---- no clothes on another body
using RebuildFn = void(__cdecl*)(CPlayerPed*, bool);
RebuildFn g_origRebuild = nullptr;
void __cdecl HookRebuildPlayer(CPlayerPed* ped, bool keep) {
    if (ped && ped == FindPlayerPed(0) && ped->ModelIndex() != 0) return;  // a Families member wears what he wears
    g_origRebuild(ped, keep);
}

bool ClothesPlace(const char* name) {
    static const char* kPrefixes[] = {"CS", "LACS", "CLOTH", "BARB", "TATT"};  // the shops' interiors (CSCHP, LACS1...)
    for (const char* p : kPrefixes)
        if (!_strnicmp(name, p, strlen(p))) return true;
    return false;
}

using TransitionFn = bool(__fastcall*)(void*, void*, CPed*);
TransitionFn g_origTransition = nullptr;
bool __fastcall HookTransitionStarted(void* enex, void* edx, CPed* ped) {
    if (ped && ped == FindPlayerPed(0) && ped->ModelIndex() != 0 && ped->AreaCode() == 0) {
        char name[9] = {};
        memcpy(name, enex, 8);  // CEntryExit::m_szName
        if (ClothesPlace(name)) {
            if (NowMs() - g_shopNoteMs > 5000) {
                g_shopNoteMs = NowMs();
                ui::TheChat().AddSystem(TR("Сюда может зайти только CJ: другой персонаж не переодевается.",
                                           "Only CJ can go in here: another character does not change clothes."));
            }
            return false;
        }
    }
    return g_origTransition(enex, edx, ped);
}

class SkinChoice : public Module {
public:
    const char* Name() const override { return "SkinChoice"; }
    void OnInstallHooks() override {
        mem::HookFn(0x5A82C0, &HookRebuildPlayer, &g_origRebuild, "CClothes::RebuildPlayer");
        mem::HookFn(0x43FFD0, &HookTransitionStarted, &g_origTransition, "CEntryExit::TransitionStarted");
    }
    void OnGameShutdown() override {
        g_active = false;
        GetPad(0)->DisablePlayerControls &= (uint16_t)~kOwnControls;
    }
    void OnSessionEnd() override { g_want = false; }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        if (!me) return;
        if (g_want && !g_active) {
            bool free = !me->InVehicle() && me->Health() > 0.0f && !Ref<uint8_t>(0xB5F851) && !Players::Instance().LocalDowned() &&
                        !(GetPad(0)->DisablePlayerControls & ~kOwnControls);
            if (free) Start(me);
            return;
        }
        if (!g_active) return;
        GetPad(0)->DisablePlayerControls |= kOwnControls;
        if (Ref<uint8_t>(0xB5F851) || me->InVehicle()) return;  // a cutscene came: the choice waits under it
        bool keys = IsForeground() && !ui::TheChat().IsOpen() && !MenuActive();
        size_t n = std::size(kChoices);
        if (keys && (int(KeyPressed(0, VK_LEFT)) | int(KeyPressed(3, 'A')))) Show((g_index + n - 1) % n);
        if (keys && (int(KeyPressed(1, VK_RIGHT)) | int(KeyPressed(4, 'D')))) Show((g_index + 1) % n);
        if (keys && KeyPressed(2, VK_RETURN)) {
            Finish(me);
            return;
        }
        Camera(me);  // the player stands still: the camera stays before him
    }
};

SkinChoice g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

void Begin() { g_want = true; }
bool Active() { return g_active; }

std::string Text(std::string& hint) {
    if (!g_active) return {};
    hint = TR("← → выбрать · Enter — играть за него", "← → choose · Enter — play as him");
    const Choice& c = kChoices[g_index];
    return Format(TR("Выберите персонажа: %s", "Choose your character: %s"), TR(c.name.ru, c.name.en));
}

void Wear(uint16_t model) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->InVehicle() || me->ModelIndex() == model) return;
    if (model && !LoadModelNow(model)) return;
    Cmd(0x09C7).I(0).I(model).Run();  // SET_PLAYER_MODEL
    if (!model) Call<0x5A82C0>(me, false);  // CClothes::RebuildPlayer: CJ in his own clothes again
    if (model) MarkModelNotNeeded(model);  // the player holds it now
    LOGI("Player model %d", model);
}

uint16_t Worn() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return 0;
    uint16_t m = me->ModelIndex();
    for (const Choice& c : kChoices)
        if (c.model == m) return m;
    return 0;
}

}  // namespace coop::skin
