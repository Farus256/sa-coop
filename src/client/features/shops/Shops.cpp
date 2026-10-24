#include "features/shops/Shops.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/buffs/Buffs.h"
#include "entities/player/Players.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::shops {

namespace {
using namespace game;
using game::script::Cmd;

enum Kind : uint8_t { K_247, K_BAR, K_CLUB };
struct Place {
    int8_t area;
    CVector at;  // the interior's middle (where its door puts the player)
    Kind kind;
};
// the game's interiors with a counter (their area codes and places, as its map has them)
const Place kPlaces[] = {
    {4, {-27.38f, -27.64f, 1003.56f}, K_247},   {6, {-26.72f, -55.99f, 1003.55f}, K_247},
    {10, {6.08f, -28.63f, 1003.55f}, K_247},    {16, {-25.37f, -139.65f, 1003.55f}, K_247},
    {17, {-25.39f, -185.91f, 1003.55f}, K_247}, {18, {-30.95f, -89.61f, 1003.55f}, K_247},
    {11, {502.33f, -70.68f, 998.76f}, K_BAR},   {18, {-227.03f, 1401.23f, 27.77f}, K_BAR},
    {1, {681.65f, -452.86f, -25.62f}, K_BAR},
    // the night clubs: the Alhambra's and Gaydar Station's room, Jizzy's, the strip clubs
    {17, {493.47f, -23.01f, 1000.68f}, K_CLUB},  {3, {-2636.72f, 1402.92f, 906.46f}, K_CLUB},
    {2, {1213.43f, -6.68f, 1000.92f}, K_CLUB},   {3, {1210.26f, -29.30f, 1000.88f}, K_CLUB},
};
constexpr float kPlaceRadius = 35.0f;
constexpr float kCounterReach = 5.0f;  // the clerk stands behind the counter: this near is at the counter

struct Text {
    const char* ru;
    const char* en;
};
struct Ware {
    buffs::Item item;
    int price;
    Text name;
};
const Ware k247[] = {{buffs::DRINK, 50, TRP("Выпивка", "A drink")},
                     {buffs::FOOD, 25, TRP("Еда", "Food")},
                     {buffs::SODA, 150, TRP("Газировка", "A soda")},
                     {buffs::CIGARETTES, 250, TRP("Сигареты", "Cigarettes")}};
const Ware kBar[] = {{buffs::DRINK, 50, TRP("Выпивка", "A drink")}, {buffs::FOOD, 25, TRP("Закуска", "A snack")}};

const Place* CurrentPlace(CPlayerPed* me) {
    int8_t area = me->AreaCode();
    if (!area) return nullptr;
    for (const Place& p : kPlaces)
        if (p.area == area && (me->GetPosition() - p.at).Length() < kPlaceRadius) return &p;
    return nullptr;
}

// Someone behind the counter: any person of the interior, this game's own or the copy of the one the player who came in
// first brought (the interior's people come in that player's game, Population): not a player, not one of their copies.
// A test build took only this game's own, so a player who came in second never found the clerk, and a bar's barman (not an
// interior group's person) was found by nobody.
CPed* NearestStaff(CPlayerPed* me, float within) {
    CPoolHeader* pool = PedPool();
    CPed* best = nullptr;
    float bestD = within;
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p == me || p->IsPlayer() || Players::Instance().FindByPed(p) || p->Health() <= 0.0f || p->InVehicle() ||
            p->AreaCode() != me->AreaCode())
            continue;
        CVector d = p->GetPosition() - me->GetPosition();
        if (std::fabs(d.z) > 2.0f) continue;
        float dist = d.Length2D();
        if (dist < bestD) bestD = dist, best = p;
    }
    return best;
}

bool HasAnim(const char* anim, const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    uintptr_t b = 0xB5D4A0 + (uintptr_t)idx * 0x20;  // CAnimManager::ms_aAnimBlocks
    if (!Ref<uint8_t>(b + 0x10)) {
        RequestModel(25575 + idx, 2);
        LoadAllRequestedModels(false);
    }
    if (!Ref<uint8_t>(b + 0x10)) return false;
    void* blk = Call<0x4D3940, void*>(block);       // CAnimManager::GetAnimationBlock
    return blk && Call<0x4D42F0, void*>(anim, blk);  // CAnimManager::GetAnimation
}
void PlayOnce(CPed* ped, const char* anim, const char* block) {
    if (ped && HasAnim(anim, block))
        Cmd(0x0605).I(GetPedRef(ped)).S(anim).S(block).F(4.0f).I(0).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM
}

// ---------------------------------------------------------------- the menu
enum MenuKind { M_NONE, M_SHOP, M_BAR, M_CLUB };
MenuKind g_menu = M_NONE, g_offer = M_NONE;
uint64_t g_noteMs = 0, g_offerCheckMs = 0;
std::string g_note;

const Ware* Wares(MenuKind m, int& n) {
    switch (m) {
        case M_SHOP: n = 4; return k247;
        case M_BAR:
        case M_CLUB: n = 2; return kBar;
        default: n = 0; return nullptr;
    }
}

void Note(const std::string& s) {
    g_note = s;
    g_noteMs = NowMs();
}

void Buy(int index) {
    CPlayerPed* me = FindPlayerPed(0);
    int n = 0;
    const Ware* list = Wares(g_menu, n);
    if (!me || !list || index < 0 || index >= n) return;
    const Ware& w = list[index];
    CPlayerInfo& info = PlayerInfos()[0];
    if (info.money < w.price) {
        Note(TR("Не хватает денег.", "Not enough money."));
        return;
    }
    info.money -= w.price;
    std::string said = buffs::Use(w.item);
    Note(said);
    ui::TheChat().AddSystem(said);
    // the others see it as this player's activity (Anims: FOOD, BAR, VENDING, SMOKING)
    switch (w.item) {
        case buffs::FOOD: PlayOnce(me, "EAT_Burger", "FOOD"); break;
        case buffs::DRINK: PlayOnce(me, "dnk_stndM_loop", "BAR"); break;
        case buffs::SODA: PlayOnce(me, "VEND_Drink2_P", "VENDING"); break;
        case buffs::CIGARETTES: PlayOnce(me, "M_smk_in", "SMOKING"); break;
        default: break;
    }
    LOGI("Shop: %s for $%d", w.name.en, w.price);
    g_menu = M_NONE;
}

// The map's own between two points at a height (the walls, the counter, the furniture; no people, no cars)
bool Open(const CVector& a, const CVector& b) {
    return Call<0x56A490, bool>(&a, &b, true, false, false, true, true, false, false);  // CWorld::GetIsLineOfSightClear
}

// At the counter: the barman across it, within reach over it. Between the two the counter stands at waist height
// (the line there is cut) and over it the way is open (the line at the head is clear): a guest in the room, a table or
// a wall between them is none of that.
bool AcrossCounter(CPlayerPed* me, CPed* staff) {
    CVector a = me->GetPosition(), b = staff->GetPosition();
    if ((b - a).Length2D() > 4.0f || std::fabs(b.z - a.z) > 1.0f) return false;
    CVector waist{0, 0, -0.15f}, head{0, 0, 0.65f};
    return !Open(a + waist, b + waist) && Open(a + head, b + head);
}

// A counter right in front of the player (waist high, nothing over it): a bar with nobody behind the counter in this
// game (its people not made here yet) still serves there, and only there
bool FacingCounter(CPlayerPed* me) {
    float h = me->CurrentRotation();
    CVector fwd{-std::sin(h), std::cos(h), 0.0f}, a = me->GetPosition();
    return !Open(a + CVector{0, 0, -0.15f}, a + fwd * 1.1f + CVector{0, 0, -0.15f}) &&
           Open(a + CVector{0, 0, 0.65f}, a + fwd * 1.6f + CVector{0, 0, 0.65f});
}

// what can be bought where the player stands
MenuKind Offer(CPlayerPed* me) {
    if (me->InVehicle() || me->Health() <= 0.0f || GetPad(0)->DisablePlayerControls) return M_NONE;
    const Place* p = CurrentPlace(me);
    if (!p) return M_NONE;
    if (p->kind == K_247) {
        // by the clerk; a shop with nobody in it in this game (its people not made yet) sells anywhere inside
        if (!NearestStaff(me, kCounterReach) && NearestStaff(me, kPlaceRadius)) return M_NONE;
        return M_SHOP;
    }
    // a bar or a club: only at the counter, by the barman (it once served anywhere in the room)
    CPed* barman = NearestStaff(me, 4.0f);
    bool atCounter = barman ? AcrossCounter(me, barman) : false;
    if (!atCounter) {
        // the nearest one is a guest: perhaps someone else across the counter
        CPoolHeader* pool = PedPool();
        for (int i = 0; pool && i < pool->size && !atCounter; ++i) {
            if (pool->byteMap[i] < 0) continue;
            auto* q = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
            if (q == me || q == barman || q->IsPlayer() || Players::Instance().FindByPed(q) || q->Health() <= 0.0f ||
                q->InVehicle() || q->AreaCode() != me->AreaCode())
                continue;
            atCounter = AcrossCounter(me, q);
        }
    }
    if (!atCounter && !NearestStaff(me, kPlaceRadius)) atCounter = FacingCounter(me);  // nobody in the room here
    if (!atCounter) return M_NONE;
    return p->kind == K_CLUB ? M_CLUB : M_BAR;
}

std::string KeyName() {
    int vk = Cfg().reviveKey;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    return "E";
}

class Shops : public Module {
public:
    const char* Name() const override { return "Shops"; }
    void OnGameShutdown() override { g_menu = g_offer = M_NONE; }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        if (!me || NowMs() - g_offerCheckMs < 250) return;
        g_offerCheckMs = NowMs();
        g_offer = Offer(me);
        if (g_menu != M_NONE && g_offer != g_menu) g_menu = M_NONE;  // walked away
    }
    void OnRender() override {
        if (MenuActive()) return;
        auto& g = ui::Gfx();
        float s = g.Scale();
        std::string key = KeyName();
        if (g_menu != M_NONE) {
            int n = 0;
            const Ware* list = Wares(g_menu, n);
            const char* title = g_menu == M_SHOP ? "24/7" : g_menu == M_CLUB ? TR("Клуб", "Club") : TR("Бар", "Bar");
            float lh = g.LineHeight(ui::FONT_NORMAL) + 8 * s, W = 420 * s, H = lh * (n + 2) + g.LineHeight(ui::FONT_MENU) + 30 * s;
            float x = 60 * s, y = g.Height() * 0.3f;
            g.Rect(x, y, W, H, 0xE0000000);
            g.RectOutline(x, y, W, H, ui::w::kSaLine, std::max(1.0f, s));
            g.Text(ui::FONT_MENU, x + 20 * s, y + 12 * s, title, ui::w::kSaBlue, ui::TEXT_SHADOW);
            float cy = y + 22 * s + g.LineHeight(ui::FONT_MENU);
            int money = PlayerInfos()[0].money;
            for (int i = 0; i < n; ++i) {
                bool afford = money >= list[i].price;
                g.Text(ui::FONT_NORMAL, x + 20 * s, cy, Format("%d. %s", i + 1, TR(list[i].name.ru, list[i].name.en)),
                       afford ? 0xFFFFFFFF : 0xFF707070, ui::TEXT_SHADOW);
                g.Text(ui::FONT_NORMAL, x + W - 20 * s, cy, Format("$%d", list[i].price), afford ? ui::w::kSaGreen : 0xFF707070,
                       ui::TEXT_SHADOW | ui::TEXT_RIGHT);
                cy += lh;
            }
            g.Text(ui::FONT_SMALL, x + 20 * s, cy + 4 * s, Format(TR("1-%d — купить · %s — закрыть", "1-%d — buy · %s — close"), n, key.c_str()),
                   ui::w::kSaMuted, ui::TEXT_SHADOW);
        } else if (g_offer != M_NONE) {
            std::string hint = Format(g_offer == M_BAR || g_offer == M_CLUB ? TR("%s — заказать у бармена", "%s — order at the bar")
                                                       : TR("%s — купить у кассира", "%s — buy from the clerk"),
                                      key.c_str());
            g.Text(ui::FONT_NORMAL, g.Width() * 0.5f, g.Height() * 0.78f, hint, 0xFFFFFFFF, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        }
        if (!g_note.empty() && NowMs() - g_noteMs < 3500)
            g.Text(ui::FONT_NORMAL, g.Width() * 0.5f, g.Height() * 0.82f, g_note, ui::w::kSaBlue, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        // the effects on, in a column at the right
        float by = g.Height() * 0.28f;
        for (const std::string& line : Split(buffs::Summary(), '\n')) {
            if (line.empty()) continue;
            g.Text(ui::FONT_SMALL, g.Width() - 16 * s, by, line, 0xE0FFFFFF, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
            by += g.LineHeight(ui::FONT_SMALL);
        }
    }
};

Shops g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

bool KeyDown(int vk) {
    if (!core::IsPlayerReady()) return false;
    if (g_menu != M_NONE) {
        if (vk == Cfg().reviveKey || vk == VK_ESCAPE || vk == VK_BACK) {
            g_menu = M_NONE;
            return true;
        }
        if (vk >= '1' && vk <= '9') {
            Buy(vk - '1');
            return true;
        }
        if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) {
            Buy(vk - VK_NUMPAD1);
            return true;
        }
        return false;
    }
    if (vk != Cfg().reviveKey) return false;
    CPlayerPed* me = FindPlayerPed(0);
    MenuKind offer = me ? Offer(me) : M_NONE;  // now, not the last check's: the key comes before the frame
    if (offer == M_NONE) return false;
    g_menu = g_offer = offer;
    return true;
}

}  // namespace coop::shops
