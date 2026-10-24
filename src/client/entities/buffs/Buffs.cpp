#include "entities/buffs/Buffs.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop::buffs {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kStatsBlock = 0xB79000, kFloatStats = 0xB79380;  // CStats::StatTypesInt, StatTypesFloat
constexpr int kSkillFirst = 69, kSkillLast = 79;  // pistol .. sniper rifle skill
constexpr int STAT_STAMINA = 22;
constexpr uint64_t kMin = 60 * 1000;

std::mt19937 g_rng{(uint32_t)NowMs()};
float& Stat(int id) { return Ref<float>(kFloatStats + (uintptr_t)id * 4); }

// what is on, until when (0: off)
uint64_t g_aimUntil[2] = {0, 0};  // the drink's, the cigarettes'
uint64_t g_speedUntil[2] = {0, 0};  // the drink's (+10%), the soda's (+5%)
uint64_t g_healthUntil = 0, g_staminaUntil = 0, g_drinkUntil = 0, g_nextDrunkRoll = 0, g_soberAt = 0;
// what was added to the stats (taken off again at the end, and for a save)
float g_skillAdded[kSkillLast - kSkillFirst + 1] = {};
float g_staminaAdded = 0;
int g_healthAdded = 0;
uint8_t g_healthRaised = 0;  // the most health as raised (the game may set it anew meanwhile)

void SetSkills(bool on) {
    for (int id = kSkillFirst; id <= kSkillLast; ++id) {
        float& added = g_skillAdded[id - kSkillFirst];
        if (on && added == 0) {
            added = std::max(0.0f, std::min(Stat(id) + 300.0f, 998.0f) - Stat(id));  // short of the hitman level: no 2 guns
            Stat(id) += added;
        } else if (!on && added) {
            Stat(id) = std::max(0.0f, Stat(id) - added);
            added = 0;
        }
    }
}
void SetStamina(bool on) {
    if (on && g_staminaAdded == 0) {
        g_staminaAdded = std::max(0.0f, std::min(Stat(STAT_STAMINA) + 400.0f, 1000.0f) - Stat(STAT_STAMINA));
        Stat(STAT_STAMINA) += g_staminaAdded;
    } else if (!on && g_staminaAdded) {
        Stat(STAT_STAMINA) = std::max(0.0f, Stat(STAT_STAMINA) - g_staminaAdded);
        g_staminaAdded = 0;
    }
}
void SetHealth(bool on) {
    CPlayerInfo& info = PlayerInfos()[0];
    CPlayerPed* me = FindPlayerPed(0);
    if (on && g_healthAdded && info.maxHealth != g_healthRaised) g_healthAdded = 0;  // the game set it anew: on again
    if (on && !g_healthAdded) {
        g_healthAdded = std::min(25, 255 - (int)info.maxHealth);
        info.maxHealth = (uint8_t)(info.maxHealth + g_healthAdded);
        g_healthRaised = info.maxHealth;
        if (me) me->Health() = std::min<float>(me->Health() + g_healthAdded, info.maxHealth);
    } else if (!on && g_healthAdded) {
        if (info.maxHealth == g_healthRaised) info.maxHealth = (uint8_t)std::max(1, info.maxHealth - g_healthAdded);
        if (me) me->Health() = std::min<float>(me->Health(), info.maxHealth);
        g_healthAdded = 0;
    }
}

void Drunk(int level, uint64_t forMs) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || !me->PlayerData()) return;
    Cmd(0x052C).I(0).I(level).Run();  // SET_PLAYER_DRUNKENNESS
    g_soberAt = NowMs() + forMs;
}

bool On(uint64_t until) { return until && NowMs() < until; }

std::string Left(uint64_t until) {
    uint64_t s = (until - std::min(until, NowMs())) / 1000;
    return Format("%d:%02d", (int)(s / 60), (int)(s % 60));
}

// walk and run faster: the move animations play quicker (their steps carry the player)
void Quicken(CPlayerPed* me, float k) {
    if (!me || me->InVehicle() || k <= 1.0f) return;
    for (void* a = anim::First(me->RwObject()); a; a = anim::Next(a)) {
        int id = anim::Id(a);
        if (id != 0 && id != 1) continue;  // ANIM_ID_WALK, ANIM_ID_RUN (the sprint's speed is the game's own)
        float& speed = Field<float>(a, 0x24);  // CAnimBlendAssociation::m_fSpeed
        if (std::fabs(speed - 1.0f) < 0.02f || std::fabs(speed - k) < 0.02f) speed = k;
    }
}

// CGenericGameStorage::GenericSave: a save (the menu's, the world sent to the others) gets the stats without the
// effects; the next frame puts them on again
using SaveFn = bool(__cdecl*)(int);
SaveFn g_origSave = nullptr;
bool __cdecl HookGenericSave(int arg) {
    if (core::IsPlayerReady()) {
        SetSkills(false);
        SetStamina(false);
        SetHealth(false);
    }
    return g_origSave(arg);
}

class BuffsModule : public Module {
public:
    const char* Name() const override { return "Buffs"; }
    void OnInstallHooks() override { mem::HookFn(0x5D13E0, &HookGenericSave, &g_origSave, "CGenericGameStorage::GenericSave"); }
    void OnGameShutdown() override { EndAll(); }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        uint64_t now = NowMs();
        SetSkills(On(g_aimUntil[0]) || On(g_aimUntil[1]));
        SetStamina(On(g_staminaUntil));
        SetHealth(On(g_healthUntil));
        float k = 1.0f + (On(g_speedUntil[0]) ? 0.10f : 0.0f) + (On(g_speedUntil[1]) ? 0.05f : 0.0f);
        Quicken(me, k);
        // the drink's after-effect: now and then a minute drunk
        if (On(g_drinkUntil) && now >= g_nextDrunkRoll) {
            g_nextDrunkRoll = now + 4 * kMin;
            if (g_rng() % 100 < 32) {
                Drunk(140, kMin);
                ui::TheChat().AddSystem(TR("Выпивка ударила в голову...", "The drink goes to your head..."));
            }
        }
        if (g_soberAt && now >= g_soberAt && me && me->PlayerData()) {
            g_soberAt = 0;
            Field<uint8_t>(me->PlayerData(), 0x41) = 1;  // CPlayerPedData::m_nFadeDrunkenness: sobers up slowly
        }
    }
    void EndAll() {
        if (core::IsPlayerReady()) {
            SetSkills(false);
            SetStamina(false);
            SetHealth(false);
        } else {
            std::fill(std::begin(g_skillAdded), std::end(g_skillAdded), 0.0f);
            g_staminaAdded = 0, g_healthAdded = 0;
        }
        g_aimUntil[0] = g_aimUntil[1] = g_speedUntil[0] = g_speedUntil[1] = 0;
        g_healthUntil = g_staminaUntil = g_drinkUntil = g_soberAt = 0;
    }
};

BuffsModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

std::string Use(Item item) {
    CPlayerPed* me = FindPlayerPed(0);
    uint64_t now = NowMs();
    switch (item) {
        case DRINK: {
            g_drinkUntil = now + 20 * kMin;
            g_nextDrunkRoll = now + 4 * kMin;
            int pick = (int)(g_rng() % 3);
            if (pick == 0) {
                g_aimUntil[0] = g_drinkUntil;
                return TR("Выпивка: меткость выше на 20 минут.", "A drink: steadier aim for 20 minutes.");
            }
            if (pick == 1) {
                g_speedUntil[0] = g_drinkUntil;
                return TR("Выпивка: вы быстрее на 20 минут.", "A drink: quicker on your feet for 20 minutes.");
            }
            g_healthUntil = g_drinkUntil;
            return TR("Выпивка: здоровья больше на 20 минут.", "A drink: more health for 20 minutes.");
        }
        case FOOD:
            if (me) me->Health() = std::max<float>(me->Health(), PlayerInfos()[0].maxHealth);
            return TR("Вы поели: здоровье восстановлено.", "You ate: health restored.");
        case SODA:
            g_staminaUntil = std::max(g_staminaUntil, now + 30 * kMin);
            g_speedUntil[1] = now + 30 * kMin;
            return TR("Газировка: больше выносливости и чуть быстрее на 30 минут.",
                      "A soda: more stamina and a little quicker for 30 minutes.");
        case CIGARETTES:
            g_aimUntil[1] = now + 30 * kMin;
            return TR("Сигареты: меткость выше на 30 минут.", "Cigarettes: steadier aim for 30 minutes.");
    }
    return {};
}

void Undo(uint8_t* block, uint8_t& maxHealth) {
    for (int id = kSkillFirst; id <= kSkillLast; ++id) {
        float& v = *(float*)(block + (kFloatStats - kStatsBlock) + (uintptr_t)id * 4);
        v = std::max(0.0f, v - g_skillAdded[id - kSkillFirst]);
    }
    float& st = *(float*)(block + (kFloatStats - kStatsBlock) + (uintptr_t)STAT_STAMINA * 4);
    st = std::max(0.0f, st - g_staminaAdded);
    if (g_healthAdded && maxHealth == g_healthRaised) maxHealth = (uint8_t)std::max(1, maxHealth - g_healthAdded);
}

std::string Summary() {
    std::string s;
    auto add = [&](const char* name, uint64_t until) {
        if (!On(until)) return;
        if (!s.empty()) s += "\n";  // one under another
        s += name;
        s += " " + Left(until);
    };
    add(TR("Меткость", "Aim"), std::max(g_aimUntil[0], g_aimUntil[1]));
    add(TR("Скорость", "Speed"), std::max(g_speedUntil[0], g_speedUntil[1]));
    add(TR("Здоровье", "Health"), g_healthUntil);
    add(TR("Выносливость", "Stamina"), g_staminaUntil);
    return s;
}

}  // namespace coop::buffs
