// Every cheat typed (on the keyboard, or "/code HESOYAM" in the chat) is told to everyone: who, which code and what it
// does. Cheats that change the shared world (weather, game speed, traffic, pedestrians, themes, car behaviour) work in both
// games: the other game applies the same cheat (CCheat::ApplyCheat), a switch only when its own state differs. Cheats
// for the player alone (weapons, money, health, wanted level, vehicles, body, skills) stay in the game where they were
// typed.
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
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;

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
        if (OneShot(id) || Active(id) != on) Call<0x438370>(id);  // CCheat::ApplyCheat
        line += TR(" · действует и у вас", " · works for you too");
        LOGI("Cheat %d from player %d applied", id, from);
    }
    ui::TheChat().AddSystem(line);
}

// "/code HESOYAM": the letters go to the game as if typed on the keyboard
void TypeCode(const std::string& text) {
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
    }
    void OnPostProcess() override { Announce(); }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
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
