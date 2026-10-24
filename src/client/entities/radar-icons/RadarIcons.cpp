#include "entities/radar-icons/RadarIcons.h"

#include <map>
#include <vector>

#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop::radar {

namespace {
using namespace game;

struct Icon {
    CVector at;
    int sprite;
};
std::map<int, Icon> g_icons;
std::vector<Drawer> g_drawers;

// both the radar (a tail jump from CRadar::DrawRadar) and the menu map draw their blips here
using DrawBlipsFn = void(__cdecl*)();
DrawBlipsFn g_origDrawBlips = nullptr;
void __cdecl HookDrawBlips() {
    g_origDrawBlips();
    if (!core::IsPlayerReady()) return;
    for (auto& [key, icon] : g_icons) {
        float x, y;
        bool clamped = false;
        if (!ToScreen({icon.at.x, icon.at.y}, x, y, &clamped) || clamped) continue;
        Call<0x585FF0>(icon.sprite, x, y, (uint8_t)255);  // CRadar::DrawRadarSprite
    }
    for (Drawer& d : g_drawers) d();
}

class RadarIcons : public Module {
public:
    const char* Name() const override { return "RadarIcons"; }
    void OnInstallHooks() override { mem::HookFn(0x588050, &HookDrawBlips, &g_origDrawBlips, "CRadar::DrawBlips"); }
};

RadarIcons g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

void SetIcon(int key, const CVector& at, int sprite) { g_icons[key] = {at, sprite}; }
void RemoveIcon(int key) { g_icons.erase(key); }
void AddDrawer(Drawer d) { g_drawers.push_back(std::move(d)); }

bool DrawingMap() { return Ref<uint8_t>(addr::MenuManager + 0x59) != 0; }  // m_bDrawingMap

bool ToScreen(const CVector2D& world, float& x, float& y, bool* clamped) {
    CVector2D in = world, radar, screen;
    Call<0x583530>(&radar, &in);                   // CRadar::TransformRealWorldPointToRadarSpace
    float dist = Call<0x5832F0, float>(&radar);    // CRadar::LimitRadarPoint: onto the radar's edge; its distance
    Call<0x583480>(&screen, &radar);               // CRadar::TransformRadarPointToScreenSpace
    if (DrawingMap()) Call<0x583350>(&screen.x, &screen.y);  // CRadar::LimitToMap
    x = screen.x, y = screen.y;
    if (clamped) *clamped = !DrawingMap() && dist > 1.0f;
    return true;
}

}  // namespace coop::radar
