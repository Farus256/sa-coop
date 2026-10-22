// Draw distance. The game's far clip and fog come from the time cycle (CTimeCycle::m_CurrentColours, set for the
// camera's place every frame by CTimeCycle::Update), and the range where models show in detail instead of their low
// LODs is the menu's draw distance (CRenderer::ms_lodDistScale, 0.925..1.8 on the slider). Both are stretched by the
// factors of coop.ini ([game] draw_distance, lod_distance; /draw in the chat changes the first one while playing).
//
// A longer view puts more entities in the frame, and the renderer's lists do not check their room
// (CRenderer::AddEntityToRenderList writes ms_aVisibleEntityPtrs[1000] / ms_aVisibleLodPtrs[1000] and
// AddToLodRenderList the 16 KB scratch block with no bound): what would not fit is left out of the frame instead
// of writing past them. The streaming memory the game gives models (50 MB) fills quickly with a long view: it is raised.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kFarClip = 0xB7C4F0;          // CTimeCycle::m_CurrentColours.m_fFarClip
constexpr uintptr_t kFogStart = 0xB7C4F4;         // CTimeCycle::m_CurrentColours.m_fFogStart
constexpr uintptr_t kLodDistScale = 0x8CD800;     // CRenderer::ms_lodDistScale
constexpr uintptr_t kMenuDrawDistance = addr::MenuManager + 0x40;  // CMenuManager::m_fDrawDistance
constexpr uintptr_t kVisibleLods = 0xB76840;      // CRenderer::ms_nNoOfVisibleLods
constexpr uintptr_t kVisibleEntities = 0xB76844;  // CRenderer::ms_nNoOfVisibleEntities
constexpr uintptr_t kLodRenderList = 0xB745D0;    // CRenderer::ms_pLodRenderList, entries of 8 bytes
constexpr uintptr_t kLodRenderListEnd = 0xC8E0C8 + 16384;  // PC_Scratch's end (the "don't render" list there is unused)
constexpr uintptr_t kStreamingMemory = 0x8A5A80;  // CStreaming::ms_memoryAvailable
constexpr int kVisibleMax = 1000;
// the far plane: a few kilometres keep the depth buffer's precision (beyond, distant buildings flicker)
constexpr float kMaxFarClip = 3600.0f;

using VoidFn = void(__cdecl*)();
VoidFn g_origTimeCycleUpdate = nullptr;
void __cdecl HookTimeCycleUpdate() {
    g_origTimeCycleUpdate();
    const auto& cfg = Cfg();
    float k = cfg.drawDistance;
    if (std::fabs(k - 1.0f) > 0.01f) {
        float& far = Ref<float>(kFarClip);
        float& fog = Ref<float>(kFogStart);
        if (far > 0.0f && far < 100000.0f) {
            float stretched = std::min(far * k, std::max(far, kMaxFarClip));
            fog = fog * (stretched / far);  // the fog starts as far off in proportion
            far = stretched;
        }
    }
    float menu = Ref<float>(kMenuDrawDistance);
    if (menu > 0.1f && menu < 10.0f) Ref<float>(kLodDistScale) = menu * cfg.lodDistance;
}

using AddToRenderListFn = void(__cdecl*)(CEntity*, float);
AddToRenderListFn g_origAddEntity = nullptr, g_origAddLod = nullptr;
uint32_t g_dropped = 0;

void __cdecl HookAddEntityToRenderList(CEntity* entity, float distance) {
    // which of the two lists it would go to is decided inside (a LOD with children, else an entity): either full, it waits
    if (Ref<int32_t>(kVisibleEntities) >= kVisibleMax - 1 || Ref<int32_t>(kVisibleLods) >= kVisibleMax - 1) {
        ++g_dropped;
        return;
    }
    g_origAddEntity(entity, distance);
}

void __cdecl HookAddToLodRenderList(CEntity* entity, float distance) {
    if (Ref<uintptr_t>(kLodRenderList) + 8 > kLodRenderListEnd) {
        ++g_dropped;
        return;
    }
    g_origAddLod(entity, distance);
}

void RaiseStreamingMemory() {
    uint32_t want = (uint32_t)Cfg().streamingMemoryMb * 1024u * 1024u;
    uint32_t& have = Ref<uint32_t>(kStreamingMemory);
    if (want && have < want) {
        LOGI("Streaming memory %u MB -> %u MB (draw distance)", have >> 20, want >> 20);
        have = want;
    }
}

class DrawDistance : public Module {
public:
    const char* Name() const override { return "DrawDistance"; }
    void OnInstallHooks() override {
        mem::HookFn(0x561760, &HookTimeCycleUpdate, &g_origTimeCycleUpdate, "CTimeCycle::Update");
        mem::HookFn(0x5534B0, &HookAddEntityToRenderList, &g_origAddEntity, "CRenderer::AddEntityToRenderList");
        mem::HookFn(0x553710, &HookAddToLodRenderList, &g_origAddLod, "CRenderer::AddToLodRenderList");
        LOGI("Draw distance x%.2f, model detail x%.2f", Cfg().drawDistance, Cfg().lodDistance);
    }
    void OnGameStarted() override { RaiseStreamingMemory(); }
    void OnPreProcess() override {
        static uint64_t last = 0;
        if (NowMs() - last < 5000) return;
        last = NowMs();
        RaiseStreamingMemory();  // CStreaming::Init and stream.ini set it on their own
        if (g_dropped) {
            LOGD("Draw distance: %u entities left out of full render lists in the last 5 s", g_dropped);
            g_dropped = 0;
        }
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd != "draw") return false;
        auto& cfg = Cfg();
        if (args.empty()) {
            ui::TheChat().AddSystem(Format(TR("Дальность прорисовки: %.1f (детализация %.1f). /draw 1.0–4.0",
                                              "Draw distance: %.1f (model detail %.1f). /draw 1.0-4.0"),
                                           cfg.drawDistance, cfg.lodDistance));
            return true;
        }
        float v = (float)std::atof(args[0].c_str());
        if (v < 0.5f || v > 4.0f) {
            ui::TheChat().AddError(TR("Укажите число от 0.5 до 4.0, например /draw 2.5", "Give a number from 0.5 to 4.0, e.g. /draw 2.5"));
            return true;
        }
        cfg.drawDistance = v;
        if (args.size() > 1) {
            float l = (float)std::atof(args[1].c_str());
            if (l >= 0.5f && l <= 3.0f) cfg.lodDistance = l;
        }
        Ini ini;
        ini.Load(cfg.iniPath);
        ini.Set("game", "draw_distance", Format("%.1f", cfg.drawDistance));
        ini.Set("game", "lod_distance", Format("%.1f", cfg.lodDistance));
        ini.Save(cfg.iniPath);
        ui::TheChat().AddSystem(Format(TR("Дальность прорисовки: %.1f (детализация %.1f).", "Draw distance: %.1f (model detail %.1f)."),
                                       cfg.drawDistance, cfg.lodDistance));
        return true;
    }
};

DrawDistance g_drawDistance;
struct Reg {
    Reg() { core::RegisterModule(&g_drawDistance); }
} g_reg;
}  // namespace

}  // namespace coop
