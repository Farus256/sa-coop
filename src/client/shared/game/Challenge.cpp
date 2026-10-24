#include "shared/game/Challenge.h"

#include <algorithm>

#include "common/lib/log/Log.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"

namespace game::challenge {

namespace {
using script::Cmd;

constexpr int kSlots = 13;
int g_depth = 0;
struct Slot {
    int type = 0, ammo = 0;
};
Slot g_saved[kSlots];
int g_savedActive = 0;
bool g_died = false;

void ClearStars() {
    void* w = Call<0x56E230, void*>(-1);  // FindPlayerWanted
    if (w && Field<int32_t>(w, 0x2C) > 0) Cmd(0x0110).I(0).Run();  // CLEAR_WANTED_LEVEL
}
}  // namespace

bool Active() { return g_depth > 0; }

namespace {
bool g_makingPickups = false;
}
void SetMakingPickups(bool on) { g_makingPickups = on; }

namespace {
bool g_throwSet = false;
CVector g_throwFrom, g_throwSpeed;
}
void SetNextThrow(bool on, const CVector& from, const CVector& speed) {
    g_throwSet = on;
    g_throwFrom = from;
    g_throwSpeed = speed;
}
bool NextThrow(CVector& from, CVector& speed) {
    if (!g_throwSet) return false;
    g_throwSet = false;
    from = g_throwFrom;
    speed = g_throwSpeed;
    return true;
}
bool MakingPickups() { return g_makingPickups; }

void Begin() {
    if (g_depth++ > 0) return;
    CPlayerPed* me = FindPlayerPed(0);
    g_died = false;
    for (int i = 0; i < kSlots; ++i) g_saved[i] = me ? Slot{me->Weapons()[i].type, me->Weapons()[i].ammoTotal} : Slot{};
    g_savedActive = me ? me->ActiveWeapon().type : 0;
    Cmd(0x01F7).I(0).I(1).Run();  // SET_POLICE_IGNORE_PLAYER
    ClearStars();
    LOGI("Challenge: weapons noted, no stars");
}

void Tick() {
    if (!g_depth) return;
    ClearStars();
    CPlayerPed* me = FindPlayerPed(0);
    if (me && me->Health() <= 0.0f) g_died = true;
}

void End() {
    if (g_depth <= 0 || --g_depth > 0) return;
    Cmd(0x01F7).I(0).I(0).Run();  // SET_POLICE_IGNORE_PLAYER: off
    ClearStars();
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    Slot now[kSlots];
    for (int i = 0; i < kSlots; ++i) now[i] = {me->Weapons()[i].type, me->Weapons()[i].ammoTotal};
    int h = GetPedRef(me);
    Cmd(0x048F).I(h).Run();  // REMOVE_ALL_CHAR_WEAPONS
    for (int i = 0; i < kSlots; ++i) {
        const Slot& was = g_saved[i];
        if (was.type <= 0) continue;
        int ammo = was.ammo;
        if (now[i].type == was.type) ammo = std::min(now[i].ammo, was.ammo);  // what was spent stays spent
        else if (g_died) continue;                                            // lost in a death there
        if (ammo <= 0 && was.type >= 16) continue;
        int model = -1;
        Cmd(0x0781).I(was.type).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
        if (model > 0 && !IsModelLoaded(model) && !LoadModelNow(model)) continue;
        Cmd(0x01B2).I(h).I(was.type).I(std::max(ammo, 1)).Run();  // GIVE_WEAPON_TO_CHAR
    }
    Cmd(0x01B9).I(h).I(g_savedActive).Run();  // SET_CURRENT_CHAR_WEAPON
    LOGI("Challenge over: weapons as before it (died in it: %d)", g_died ? 1 : 0);
}

}  // namespace game::challenge
