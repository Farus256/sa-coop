// Every cheat typed (on the keyboard, or "/code HESOYAM" in the chat) is told to everyone: who, which code and what it
// does. Cheats that change the shared world (weather, game speed, traffic, pedestrians, themes, car behaviour) work in both
// games: the other game applies the same cheat (CCheat::ApplyCheat), a switch only when its own state differs. Cheats
// for the player alone (weapons, money, health, wanted level, vehicles, body, skills) stay in the game where they were
// typed, and so do a theme's clothes and katana.
// The host can forbid cheat codes in its game (the "No cheats" box in F9, no_cheats in coop.ini, /cheats on|off):
// then no code works for anyone, the host included, and the one who typed it is told. The keys still go into the typed
// string (CCheat::DoCheats), but with the codes' hashes blanked for the call nothing matches; a code typed whole is
// then recognised here and refused. (AddToCheatString applies a cheat by itself, not through CCheat::ApplyCheat.)
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "entities/session/Flow.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kCheatString = 0x969110;   // CCheat::m_CheatString[30], the newest key first
constexpr uintptr_t kCheatsActive = 0x969130;  // CCheat::m_aCheatsActive[92]
constexpr uintptr_t kHashKeys = 0x8A5CC8;      // CCheat::m_aCheatHashKeys[92]
constexpr int kCheats = 92;

bool WorldCheat(int id) {
    switch (id) {
    case 6: case 7: case 8: case 9: case 10:  // weather
    case 11: case 12: case 13:                // faster clock, faster and slower gameplay
    case 14: case 15: case 16:                // mayhem, bounty, everyone armed
    case 26: case 27: case 28:                // blow up all cars, invisible cars, perfect handling
    case 30: case 31: case 32: case 33: case 34: case 35:  // green lights, aggressive drivers, pink/black traffic, water
    case 39: case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48: case 49:  // themes, gangs
    case 53: case 54: case 55: case 56: case 57: case 58:  // nitro, floating cars, midnight, orange sky, storms
    case 69: case 70: case 74: case 75:       // riot, funhouse, reduced and country traffic
        return true;
    }
    return false;
}
bool OneShot(int id) { return (id >= 6 && id <= 10) || id == 12 || id == 13 || id == 26 || id == 57 || id == 58; }
bool Active(int id) { return Ref<uint8_t>(kCheatsActive + id) != 0; }

// what each cheat does, in a few words (eCheats order)
struct Info {
    const char* ru;
    const char* en;
};
constexpr Info kInfo[kCheats] = {
    TRP("набор оружия 1 (бандитский)", "weapon set 1 (thug's tools)"),
    TRP("набор оружия 2 (профессиональный)", "weapon set 2 (professional tools)"),
    TRP("набор оружия 3 (безумный)", "weapon set 3 (nutter tools)"),
    TRP("здоровье, броня и $250 000", "health, armour and $250,000"),
    TRP("розыск +2 звезды", "wanted level +2 stars"),
    TRP("розыск снят", "wanted level cleared"),
    TRP("солнечная погода", "sunny weather"),
    TRP("очень солнечная погода", "very sunny weather"),
    TRP("пасмурная погода", "overcast weather"),
    TRP("дождь", "rainy weather"),
    TRP("туман", "foggy weather"),
    TRP("часы идут быстрее", "faster clock"),
    TRP("игра быстрее", "faster gameplay"),
    TRP("игра медленнее", "slower gameplay"),
    TRP("прохожие дерутся клюшками для гольфа", "peds attack each other with golf clubs"),
    TRP("награда за вашу голову", "a bounty on your head"),
    TRP("все прохожие вооружены", "everyone is armed"),
    TRP("танк Rhino", "Rhino tank"),
    TRP("машина Bloodring Banger", "Bloodring Banger car"),
    TRP("машина Rancher", "Rancher car"),
    TRP("машина Hotring Racer A", "Hotring Racer A car"),
    TRP("машина Hotring Racer B", "Hotring Racer B car"),
    TRP("катафалк Romero", "Romero hearse"),
    TRP("лимузин Stretch", "Stretch limousine"),
    TRP("мусоровоз Trashmaster", "Trashmaster"),
    TRP("гольф-кар Caddy", "Caddy golf cart"),
    TRP("взорвать все машины вокруг", "blow up all cars around"),
    TRP("невидимые машины", "invisible cars"),
    TRP("идеальная управляемость", "perfect handling"),
    TRP("самоубийство", "suicide"),
    TRP("везде зелёный свет", "all green lights"),
    TRP("агрессивные водители", "aggressive drivers"),
    TRP("розовые машины", "pink traffic"),
    TRP("чёрные машины", "black traffic"),
    TRP("машины ездят по воде", "cars drive on water"),
    TRP("лодки летают", "boats fly"),
    TRP("CJ толстый", "fat CJ"),
    TRP("CJ мускулистый", "muscular CJ"),
    TRP("CJ худой", "skinny CJ"),
    TRP("Элвисы повсюду", "Elvis is everywhere"),
    TRP("прохожие нападают с ракетницами", "peds attack you with rockets"),
    TRP("пляжная вечеринка", "beach party"),
    TRP("бандиты повсюду", "gang members everywhere"),
    TRP("улицы во власти банд", "gangs control the streets"),
    TRP("ниндзя-тема", "ninja theme"),
    TRP("прохожие липнут к CJ", "slut magnet"),
    TRP("дешёвые машины", "cheap traffic"),
    TRP("быстрые машины", "fast traffic"),
    TRP("машины летают", "cars fly"),
    TRP("огромный прыжок на велосипеде", "huge bunny hop"),
    TRP("самолёт Hydra", "Hydra jet"),
    TRP("ховеркрафт Vortex", "Vortex hovercraft"),
    TRP("машины разбиваются от касания", "smash n' boom"),
    TRP("у всех машин нитро", "all cars have nitro"),
    TRP("машины улетают от удара", "cars float away when hit"),
    TRP("вечная полночь", "always midnight"),
    TRP("часы стоят, оранжевое небо", "clock stops, orange sky"),
    TRP("гроза", "thunderstorm"),
    TRP("песчаная буря", "sandstorm"),
    TRP("не используется", "unused"),
    TRP("мегапрыжок", "mega jump"),
    TRP("бесконечное здоровье", "infinite health"),
    TRP("бесконечный кислород", "infinite oxygen"),
    TRP("парашют", "parachute"),
    TRP("реактивный ранец", "jetpack"),
    TRP("розыск не растёт", "never wanted"),
    TRP("шесть звёзд розыска", "six wanted stars"),
    TRP("мегаудар", "mega punch"),
    TRP("никогда не голоден", "never hungry"),
    TRP("бунт", "riot mode"),
    TRP("тема «комната смеха»", "funhouse theme"),
    TRP("режим адреналина", "adrenaline mode"),
    TRP("бесконечные патроны без перезарядки", "infinite ammo, no reload"),
    TRP("полное прицеливание за рулём", "full weapon aiming while driving"),
    TRP("меньше машин", "reduced traffic"),
    TRP("сельский транспорт", "country traffic"),
    TRP("прохожие вступают в банду", "recruit anyone (9mm)"),
    TRP("прохожие вступают в банду с ракетницами", "recruit anyone (rockets)"),
    TRP("ракетный хаос", "rocket mayhem"),
    TRP("максимальное уважение", "max respect"),
    TRP("максимальная привлекательность", "max sex appeal"),
    TRP("максимальная выносливость", "max stamina"),
    TRP("максимальные навыки оружия", "hitman level for all weapons"),
    TRP("максимальные навыки вождения", "max driving skills"),
    TRP("вертолёт Hunter", "Hunter helicopter"),
    TRP("квадроцикл", "quad bike"),
    TRP("бензовоз", "tanker truck"),
    TRP("бульдозер Dozer", "Dozer"),
    TRP("самолёт Stuntplane", "Stuntplane"),
    TRP("монстр-трак", "Monster truck"),
    TRP("проститутки платят вам", "prostitutes pay you"),
    TRP("у такси нитро", "all taxis have nitro"),
};

// the cheat the game just matched, as CCheat::AddToCheatString searches the typed string (the newest key first, the
// shortest run first), and that run's length
int Matched(const char* s, int* lenOut = nullptr) {
    int typed = (int)strnlen(s, 29);
    for (int len = 6; len <= typed; ++len) {
        uint32_t hash = Call<0x53CED0, uint32_t>(s, len);  // CKeyGen::GetKey(const char*, int)
        int found = -1;
        for (int j = 0; j < kCheats; ++j)
            if (Ref<uint32_t>(kHashKeys + (uintptr_t)j * 4) == hash) found = j;
        if (found >= 0) {
            if (lenOut) *lenOut = len;
            return found;
        }
    }
    return -1;
}

std::string Describe(int id, const std::string& code, bool toggled, bool on) {
    std::string what = TR(kInfo[id].ru, kInfo[id].en);
    if (toggled) what += on ? TR(" (включено)", " (on)") : TR(" (выключено)", " (off)");
    return code + ": " + what;
}

// a cheat typed here: told once the game applied it (whether a switch went on or off shows only then)
bool g_noCheats = false;      // the host's rule here
bool NoCheats() { return Net().Online() && (Net().IsHost() ? Cfg().noCheats : g_noCheats); }

using DoCheatsFn = void(__cdecl*)();
DoCheatsFn g_origDoCheats = nullptr;
void __cdecl HookDoCheats() {
    if (!NoCheats()) return g_origDoCheats();
    uint32_t keys[92];
    memcpy(keys, (void*)0x8A5CC8, sizeof keys);  // CCheat::m_aCheatHashKeys
    memset((void*)0x8A5CC8, 0, sizeof keys);
    g_origDoCheats();
    memcpy((void*)0x8A5CC8, keys, sizeof keys);
    const char* typed = (const char*)0x969110;  // CCheat::m_CheatString
    int len = 0;
    if (Matched(typed, &len) >= 0) {
        Ref<uint8_t>(0x969110) = 0;  // gone, so that it is told once
        ui::TheChat().AddError(TR("Чит-коды отключены хостом.", "The host turned cheat codes off."));
    }
}

void SendRule(proto::PlayerMask to = proto::kAllPlayers) {
    if (!Net().Online() || !Net().IsHost()) return;
    Writer w(proto::G_CHEAT_RULE);
    w.U8(Cfg().noCheats);
    Net().SendTo(to, proto::CH_RELIABLE, w);
}

void HandleRule(uint8_t from, Reader& r) {
    uint8_t on = r.U8();
    if (!r.Ok() || from != Net().HostId()) return;
    if ((on != 0) != g_noCheats)
        ui::TheChat().AddSystem(on ? TR("Хост отключил чит-коды.", "The host turned cheat codes off.")
                                   : TR("Хост разрешил чит-коды.", "The host allowed cheat codes."));
    g_noCheats = on != 0;
}

struct Typed {
    bool pending = false;
    int id = -1;
    bool wasActive = false;
    std::string code;
} g_typed;

// CCheat::AddToCheatString counts a matched cheat (CStats::IncrementStat(STAT_TIMES_CHEATED)) right before applying
// it, with the typed string still whole. The function's own first bytes stay as they are: GInput reads the
// cutscene flag's address out of them and crashed on a hook there.
using IncrementStatFn = void(__cdecl*)(uint16_t, float);
IncrementStatFn g_origIncrementStat = nullptr;
void __cdecl HookCheatCounted(uint16_t stat, float value) {
    g_origIncrementStat(stat, value);
    const char* s = (const char*)kCheatString;
    int len = 0, id = Matched(s, &len);
    if (id < 0) return;
    std::string code(s, (size_t)len);
    std::reverse(code.begin(), code.end());  // the newest key is first
    g_typed = {true, id, Active(id), code};
}

void Announce() {
    if (!g_typed.pending) return;
    g_typed.pending = false;
    int id = g_typed.id;
    bool on = Active(id), toggled = on != g_typed.wasActive;
    ui::TheChat().AddSystem(Format(TR("Вы ввели чит-код %s", "You entered the cheat %s"),
                                   Describe(id, g_typed.code, toggled, on).c_str()));
    LOGI("Cheat %d (%s) typed", id, g_typed.code.c_str());
    if (!Net().Online()) return;
    Writer w(proto::G_CHEAT);
    w.U8((uint8_t)id);
    w.U8(on);
    w.Str(g_typed.code, 32);
    w.U8(toggled);
    w.U8(flow::InSharedWorld() && WorldCheat(id));  // the others apply it too
    Net().Send(proto::CH_RELIABLE, w);
}

void HandleCheat(uint8_t from, Reader& r) {
    int id = r.U8();
    bool on = r.U8() != 0;
    std::string code = r.Str(32);
    bool toggled = r.U8() != 0, world = r.U8() != 0;
    if (!r.Ok() || id < 0 || id >= kCheats) return;
    std::string line = Format(TR("%s ввёл чит-код %s", "%s entered the cheat %s"), Net().PeerName(from).c_str(),
                              Describe(id, code, toggled, on).c_str());
    if (world && WorldCheat(id) && core::IsPlayerReady() && flow::InSharedWorld()) {
        CPlayerPed* me = FindPlayerPed(0);
        CPedClothesDesc look = *me->ClothesDesc();
        int melee = me->Weapons()[1].type, current = me->ActiveWeapon().type;  // the hand weapon's slot
        if (OneShot(id) || Active(id) != on) Call<0x438370>(id);  // CCheat::ApplyCheat
        // A theme dresses whoever applies it (the beach party, the gimp suit, the funhouse, country) and the ninja one
        // arms him with a katana: the theme is the world's, the look and the katana the typing player's (original #43)
        if (memcmp(me->ClothesDesc(), &look, sizeof look) != 0) {
            *me->ClothesDesc() = look;
            if (me->PedState() != 50) Call<0x5A82C0>(me, false);  // not PEDSTATE_DRIVING: CClothes::RebuildPlayer
        }
        if (int h = GetPedRef(me); me->Weapons()[1].type == 8 && melee != 8) {  // WEAPON_KATANA
            Cmd(0x0555).I(h).I(8).Run();                      // REMOVE_WEAPON_FROM_CHAR
            if (melee) Cmd(0x01B2).I(h).I(melee).I(1).Run();  // GIVE_WEAPON_TO_CHAR: its own back
            Cmd(0x01B9).I(h).I(current).Run();                // SET_CURRENT_CHAR_WEAPON
        }
        line += TR(" · действует и у вас", " · works for you too");
        LOGI("Cheat %d from player %d applied", id, from);
    }
    ui::TheChat().AddSystem(line);
}

// "/code HESOYAM": the letters go to the game as if typed on the keyboard
void TypeCode(const std::string& text) {
    if (NoCheats()) {
        ui::TheChat().AddError(TR("Чит-коды отключены хостом.", "The host turned cheat codes off."));
        return;
    }
    if (!core::IsPlayerReady()) {
        ui::TheChat().AddError(TR("Чит-коды вводятся в игре.", "Cheats are entered in the game."));
        return;
    }
    std::string letters;
    for (char c : text)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letters += (char)toupper((unsigned char)c);
    if (letters.size() < 6) {
        ui::TheChat().AddError(TR("Напишите код целиком: /code HESOYAM", "Type the whole code: /code HESOYAM"));
        return;
    }
    Ref<uint8_t>(kCheatString) = 0;  // what was typed before does not join this one
    for (char c : letters) Call<0x438480>(c);  // CCheat::AddToCheatString
    if (!g_typed.pending)
        ui::TheChat().AddError(Format(TR("Чит-кода %s нет.", "There is no cheat %s."), letters.c_str()));
}

class Cheats : public Module {
public:
    const char* Name() const override { return "Cheats"; }

    void OnInstallHooks() override {
        mem::RedirectCallFn(0x438565, &HookCheatCounted, &g_origIncrementStat);  // in CCheat::AddToCheatString
        Net().SetHandler(proto::G_CHEAT, &HandleCheat);
        Net().SetHandler(proto::G_CHEAT_RULE, &HandleRule);
        mem::HookFn(0x439AF0, &HookDoCheats, &g_origDoCheats, "CCheat::DoCheats");
    }
    void OnSessionStart() override { SendRule(); }
    void OnSessionEnd() override { g_noCheats = false; }
    void OnPeerJoin(uint8_t id) override { SendRule(proto::Bit(id)); }
    void OnPeerResync(uint8_t id) override { SendRule(proto::Bit(id)); }
    void OnPostProcess() override {
        Announce();
        static int sent = -1;  // the host's box ticked in F9 during the game: the rule goes out at once
        if (Net().Online() && Net().IsHost() && sent != (int)Cfg().noCheats) {
            sent = Cfg().noCheats;
            SendRule();
        }
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd == "cheats") {  // the host: /cheats off | on
            std::string a = args.empty() ? "" : ToLower(args[0]);
            if (!Net().Online() || !Net().IsHost()) {
                ui::TheChat().AddError(TR("Чит-коды разрешает и запрещает хост.", "The host allows or forbids cheat codes."));
            } else if (a == "off" || a == "on") {
                Cfg().noCheats = a == "off";
                flow::SaveConnectSettings();
                SendRule();
                ui::TheChat().AddSystem(Cfg().noCheats ? TR("Чит-коды отключены у всех.", "Cheat codes are off for everyone.")
                                                       : TR("Чит-коды снова работают.", "Cheat codes work again."));
            } else {
                ui::TheChat().AddSystem(Cfg().noCheats ? TR("Чит-коды отключены. /cheats on — разрешить.", "Cheat codes are off. /cheats on allows them.")
                                                       : TR("Чит-коды работают. /cheats off — отключить у всех.", "Cheat codes work. /cheats off turns them off for all."));
            }
            return true;
        }
        if (cmd != "code" && cmd != "cheat") return false;
        std::string all;
        for (auto& a : args) all += a;
        TypeCode(all);
        return true;
    }

    static Cheats& Instance() {
        static Cheats m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Cheats::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
