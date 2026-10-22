#include "pages/connect-menu/ConnectMenu.h"

#include <windows.h>

#include <algorithm>

#include "common/config/version/Version.h"
#include "common/lib/util/Util.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

namespace {
using w::kSaBlue, w::kSaSlate, w::kSaMuted, w::kSaRed, w::kSaLine, w::kSaGreen;
constexpr uint32_t kText = 0xFFE6E9EF;
constexpr uint32_t kGold = 0xFFE2C063;  // the game's mission-title cream

bool g_visible = false;
bool g_autoShown = false;
int g_tab = 0;  // 0 "connect", 1 "create"
std::wstring g_nick, g_addr, g_port, g_password;
std::string g_formError;
int g_slotIndex = 0;  // index into the slot list
std::vector<flow::SaveSlotInfo> g_slots;
uint64_t g_slotsTime = 0;
uint64_t g_copiedMs = 0;

void CopyToClipboard(const std::string& utf8) {
    std::wstring text = Utf8ToWide(utf8);
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        else g_copiedMs = NowMs();
    }
    CloseClipboard();
}

// the save slots and, after them, a new game (the host plays its prologue alone, partners join at the bike)
void RefreshSlots() {
    g_slots = flow::ListSaveSlots();
    flow::SaveSlotInfo fresh;
    fresh.slot = -1;
    fresh.exists = true;
    fresh.title = TR("Новая игра", "New game");
    g_slots.push_back(fresh);
    g_slotsTime = NowMs();
}

void LoadFields() {
    auto& cfg = Cfg();
    g_nick = Widen(cfg.nickname);
    g_addr = Widen(cfg.host);
    g_port = std::to_wstring(cfg.port);
    g_password = Widen(cfg.password);
    g_tab = cfg.wantsHost ? 1 : 0;
    g_formError.clear();
    RefreshSlots();
    // preselect the first save; without any, the new game
    g_slotIndex = 0;
    for (size_t i = 0; i < g_slots.size(); ++i)
        if (g_slots[i].exists) {
            g_slotIndex = (int)i;
            break;
        }
}

bool ApplyNick() {
    std::string nick = SanitizeNickname(Narrow(g_nick));
    if (nick.size() < 2) {
        g_formError = TR("В нике должно быть от 2 до 20 символов (буквы, цифры, знаки _-.[]).",
                         "A nickname needs 2 to 20 characters (letters, digits, _-.[]).");
        w::SetFocus("nick");
        return false;
    }
    g_formError.clear();
    Cfg().nickname = nick;
    g_nick = Widen(nick);
    return true;
}

uint16_t PortValue() {
    int p = _wtoi(g_port.c_str());
    if (p <= 0 || p > 65535) p = 7788;
    return (uint16_t)p;
}
}  // namespace

void SetConnectMenuVisible(bool visible) {
    if (visible == g_visible) return;
    g_visible = visible;
    if (visible) {
        LoadFields();
        if (TheChat().IsOpen()) TheChat().Close();
        In().SetKeyboardCapture(true);
        In().SetMouseCapture(true);
    } else {
        In().SetKeyboardCapture(false);
        In().SetMouseCapture(false);
    }
}

bool IsConnectMenuVisible() { return g_visible; }

void ProcessConnectMenu() {
    // Show the window once on the first main-menu frame so the player sees how to start co-op.
    if (!g_autoShown && game::GameState() == game::GAME_STATE_FRONTEND_IDLE && !Cfg().autoConnect) {
        g_autoShown = true;
        SetConnectMenuVisible(true);
    }
    if (g_visible && NowMs() - g_slotsTime > 3000) RefreshSlots();
}

void RenderConnectMenu() {
    if (!g_visible) return;
    auto& g = Gfx();
    float s = g.Scale();
    auto& net = Net();
    bool online = net.Online();
    auto phase = flow::GetPhase();
    bool busy = phase == flow::Phase::StartingServer || phase == flow::Phase::Connecting ||
                phase == flow::Phase::WaitingWorld || phase == flow::Phase::LoadingWorld;
    bool inGame = core::IsPlayerReady();

    float lh = g.LineHeight(FONT_NORMAL), sh = g.LineHeight(FONT_SMALL), mh = g.LineHeight(FONT_MENU),
          ch = g.LineHeight(FONT_MENU_SMALL), bh = g.LineHeight(FONT_BIG);
    float rowH = lh + 14 * s;
    float W = 820 * s, H = 690 * s;
    float x = (g.Width() - W) * 0.5f, y = (g.Height() - H) * 0.5f;
    float cx = x + 52 * s, cw = W - 104 * s;
    float room = mh * 1.6f;  // space for the pointer left of a menu item

    float fy = y + H - 20 * s - sh;       // key legend
    float by = fy - 14 * s - rowH;        // footer items
    float bodyBottom = by - 12 * s;

    g.Rect(0, 0, g.Width(), g.Height(), 0x99000000);
    w::BeginFrame();
    float cy = w::Panel(x, y, W, H, "SA:COOP") + 8 * s;
    float vx = x + W - 52 * s;
    g.Text(FONT_MENU_SMALL, vx, y + 36 * s, "v" COOP_VERSION_STR, kSaSlate, TEXT_RIGHT);
    // the language switch left of the version, the pointer at the active one; the choice is saved at once
    float lx = vx - g.TextWidth(FONT_MENU_SMALL, "v" COOP_VERSION_STR) - 34 * s;
    for (int en = 1; en >= 0; --en) {
        const char* label = en ? "EN" : "RU";
        float tw = g.TextWidth(FONT_MENU_SMALL, label);
        lx -= tw;
        if (w::Tab(en ? "lang1" : "lang0", lx - 6 * s, y + 32 * s, tw + 12 * s, ch + 8 * s, label, Cfg().english == (en != 0), true)) {
            Cfg().english = en != 0;
            flow::SaveConnectSettings();
            RefreshSlots();
        }
        lx -= 52 * s;  // room for the pointer of EN
    }

    auto caption = [&](float px, const std::string& text) { g.Text(FONT_MENU_SMALL, px, cy, text, kSaMuted); };
    // wrapped paragraph, advances cy
    auto para = [&](const std::string& text, uint32_t color, FontId f = FONT_SMALL, bool centre = false) {
        for (const std::wstring& line : g.Wrap(f, Widen(text), cw)) {
            g.TextW(f, centre ? cx + cw * 0.5f : cx, cy, line, color, TEXT_SHADOW | (centre ? TEXT_CENTER : 0));
            cy += g.LineHeight(f);
        }
    };
    // a menu item whose text starts at px (the pointer hangs to the left of it)
    auto item = [&](const char* id, float px, float py, const std::string& label, bool enabled = true, bool lit = false) {
        return w::Button(id, px - room, py, g.TextWidth(FONT_MENU, label) + 2 * room, rowH, label, enabled, lit);
    };

    if (online) {
        // ---- in a session: who we are, the address for friends, the players
        bool host = net.IsHost();
        g.Text(FONT_BIG, cx, cy, host ? TR("Сервер запущен", "Server running") : TR("Вы в игре", "Connected"), kSaBlue,
               TEXT_SHADOW);
        cy += bh + 2 * s;
        if (!host) para(Format(TR("Сервер «%s»", "Server \"%s\""), net.ServerName().c_str()), kSaMuted, FONT_NORMAL);
        cy += 22 * s;
        if (host) {
            const Session::Address& a = net.ServerAddress();
            caption(cx, TR("Адрес для друзей", "Address for your friends"));
            cy += ch + 6 * s;
            if (!a.done && a.external.empty()) {
                g.Text(FONT_NORMAL, cx, cy, TR("Узнаём внешний IP...", "Finding out the external IP..."), kSaMuted, TEXT_SHADOW);
                cy += bh;
            } else {
                std::string shown = !a.external.empty() ? a.external : a.lan;
                // our wide capitals, not the game's pricedown: translations swap letters in that font
                g.Text(FONT_BIG, cx, cy, shown.empty() ? TR("не определён", "unknown") : shown, kGold, TEXT_OUTLINE);
                if (!shown.empty()) {
                    bool copied = NowMs() - g_copiedMs < 1500;
                    const std::string label = copied ? TR("Скопировано", "Copied") : TR("Скопировать", "Copy");
                    if (item("copy", cx + cw - g.TextWidth(FONT_MENU, label), cy + (bh - rowH) * 0.5f, label, true, copied))
                        CopyToClipboard(shown);
                }
                cy += bh + 4 * s;
                if (!a.external.empty() && !a.lan.empty()) para(Format(TR("В локальной сети: %s", "In your local network: %s"), a.lan.c_str()), kSaMuted);
                if (a.cgnat)
                    para(TR("Роутер за NAT провайдера (CGNAT): из интернета к вам не подключиться. Нужен белый IP у провайдера "
                            "или VPN (Radmin VPN, ZeroTier); в локальной сети всё работает.",
                            "Your router is behind the provider's NAT (CGNAT): nobody can connect from the internet. You need a "
                            "public IP from the provider or a VPN (Radmin VPN, ZeroTier); the local network works."),
                         kSaRed);
                else if (a.done && a.external.empty())
                    para(TR("Внешний IP не определён: нет доступа в интернет. Подключиться можно только из локальной сети.",
                            "The external IP is unknown: no internet access. Only players in your local network can connect."),
                         kSaRed);
                else if (a.done && !a.upnp)
                    para(Format(TR("UPnP не сработал (%s): откройте UDP-порт %u на роутере вручную, иначе друзья из интернета "
                                   "не подключатся.",
                                   "UPnP did not work (%s): open UDP port %u on your router by hand, or friends from the internet "
                                   "cannot connect."),
                                a.upnpError.empty() ? "?" : a.upnpError.c_str(), (unsigned)Cfg().port),
                         kSaRed);
                else if (a.done)
                    para(Format(TR("Порт %u открыт на роутере через UPnP.", "Port %u is open on your router through UPnP."), (unsigned)Cfg().port),
                         kSaMuted);
            }
            cy += 16 * s;
        }
        int rows = 1 + (int)net.Peers().size();
        caption(cx, Format(TR("Игроки: %d из %d", "Players: %d of %d"), rows, net.MaxPlayers()));
        cy += ch + 8 * s;
        // up to 16: two columns of smaller rows once one column of big ones would not fit
        bool compact = rows > 5;
        int columns = compact ? 2 : 1;
        float gap = 10 * s, colW = (cw - gap * (columns - 1)) / columns;
        float rh = compact ? std::min(sh + 10 * s, (bodyBottom - cy) / ((rows + 1) / 2) - 4 * s) : lh + 14 * s;
        FontId font = compact ? FONT_SMALL : FONT_NORMAL;
        float top = cy;
        int index = 0;
        auto row = [&](uint8_t id, const std::string& name, bool isHost, uint32_t ping, bool me) {
            int col = index % columns, line = index / columns;
            ++index;
            float rx = cx + col * (colW + gap), ry = top + line * (rh + 4 * s);
            g.Rect(rx, ry, colW, rh, 0xFF090C10);
            char dot[24];
            snprintf(dot, sizeof dot, "{%06X}●{FFFFFF} ", (unsigned)(PlayerColor(id) & 0xFFFFFF));
            float fh = g.LineHeight(font);
            g.Text(font, rx + 10 * s, ry + (rh - fh) * 0.5f, dot + (me ? name + TR(" (вы)", " (you)") : name),
                   me ? 0xFFFFFFFF : kText, TEXT_SHADOW);
            if (isHost) g.Text(FONT_MENU_SMALL, rx + colW * (compact ? 0.62f : 0.55f), ry + (rh - ch) * 0.5f, TR("хост", "host"), kSaBlue);
            g.Text(FONT_SMALL, rx + colW - 10 * s, ry + (rh - sh) * 0.5f, Format(TR("%u мс", "%u ms"), ping), kSaMuted, TEXT_RIGHT);
            cy = std::max(cy, ry + rh + 4 * s);
        };
        row(net.MyId(), Cfg().nickname, host, net.Rtt(), true);
        for (auto& [id, p] : net.Peers()) row(id, p.name, p.isHost, p.ping, false);
        if (host && net.Peers().empty()) para(TR("Ждём друзей...", "Waiting for your friends..."), kSaMuted);
    } else if (busy) {
        // ---- connecting / starting / loading, in the middle of the body
        float barH = 6 * s;
        cy += std::max(0.0f, (bodyBottom - cy - (mh * 2 + 18 * s + barH + 16 * s + sh)) * 0.5f);
        para(flow::StatusText(), kSaBlue, FONT_MENU, true);
        cy += 18 * s;
        g.Rect(cx, cy, cw, barH, 0xFF141A21);
        float p = phase == flow::Phase::WaitingWorld ? net.WorldProgress() : -1.0f;
        if (p >= 0) {
            g.Rect(cx, cy, cw * std::clamp(p, 0.0f, 1.0f), barH, kSaBlue);
        } else {  // nothing to measure: a block sliding back and forth
            float t = (float)(NowMs() % 1600) / 800.0f;
            float bw = cw * 0.25f;
            g.Rect(cx + (cw - bw) * (t < 1 ? t : 2 - t), cy, bw, barH, kSaBlue);
        }
        cy += barH + 16 * s;
        if (phase == flow::Phase::WaitingWorld && p < 0)
            para(TR("Хост сохраняет мир. Скоро он загрузится у вас.", "The host is saving the world. It will load for you soon."),
                 kSaMuted, FONT_SMALL, true);
    } else {
        // ---- the form: the tabs are menu items, the active one carries the pointer
        float tx = cx;
        for (int t = 0; t < 2; ++t) {
            const char* label = t ? TR("Создать игру", "Host a game") : TR("Подключиться", "Join a game");
            float tw = g.TextWidth(FONT_MENU, label);
            if (w::Tab(t ? "tab1" : "tab0", tx - room, cy, tw + 2 * room, rowH, label, g_tab == t)) {
                g_tab = t;
                g_formError.clear();
            }
            tx += tw + room * 2.2f;
        }
        cy += rowH + 4 * s;
        g.Rect(cx, cy, cw, std::max(1.0f, s), kSaLine);
        cy += 16 * s;

        caption(cx, TR("Ник", "Nickname"));
        cy += ch + 6 * s;
        bool enter = w::TextBox("nick", cx, cy, 320 * s, rowH, g_nick, 20);
        cy += rowH + 14 * s;

        int hostSlot = 0;
        if (g_tab == 0) {
            caption(cx, TR("IP-адрес хоста", "Host's IP address"));
            cy += ch + 6 * s;
            const char* paste = TR("Вставить", "Paste");
            float pw = g.TextWidth(FONT_MENU, paste) + room * 1.9f;
            enter |= w::TextBox("addr", cx, cy, cw - pw, rowH, g_addr, 64, false, false,
                                TR("например, 85.12.34.56 или 85.12.34.56:7788", "for example, 85.12.34.56 or 85.12.34.56:7788"));
            if (item("paste", cx + cw - g.TextWidth(FONT_MENU, paste), cy, paste)) {
                std::wstring clip = Widen(Trim(Narrow(w::ClipboardText())));
                if (!clip.empty()) g_addr = clip.substr(0, 64);
            }
        } else {
            caption(cx, TR("Сохранение", "Save"));
            cy += ch + 6 * s;
            if (inGame) {
                g.Rect(cx, cy, cw, rowH, 0xFF090C10);
                g.Text(FONT_NORMAL, cx + 12 * s, cy + (rowH - lh) * 0.5f, TR("Текущая игра", "Current game"), kText, TEXT_SHADOW);
            } else {
                int d = w::Selector("slot", cx, cy, cw, rowH, g_slots[g_slotIndex].title);
                if (d) g_slotIndex = (g_slotIndex + d + (int)g_slots.size()) % (int)g_slots.size();
                if (g_slots[g_slotIndex].exists) hostSlot = g_slots[g_slotIndex].slot;
            }
        }
        cy += rowH + 14 * s;

        caption(cx, TR("Порт", "Port"));
        caption(cx + 170 * s, TR("Пароль", "Password"));
        cy += ch + 6 * s;
        enter |= w::TextBox("port", cx, cy, 150 * s, rowH, g_port, 5, false, true);
        enter |= w::TextBox("pass", cx + 170 * s, cy, cw - 170 * s, rowH, g_password, 32, true, false, TR("необязательно", "optional"));
        cy += rowH + 22 * s;
        if (g_tab == 1) {  // the host decides for the whole game; saved with the other settings when the server starts
            w::Checkbox("ff", cx, cy, TR("Урон по напарнику", "Friendly fire"), Cfg().friendlyFire);
            // the places on the server, the host's own among them
            int& places = Cfg().maxPlayers;
            float sw = 300 * s;
            int d = w::Selector("places", cx + cw - sw, cy - (rowH - lh * 0.8f) * 0.5f, sw, rowH,
                                Format(TR("Игроков: до %d", "Players: up to %d"), places));
            if (d) places = (places - 2 + d + 15) % 15 + 2;  // 2..16 round
            cy += lh * 0.8f + 22 * s;
        }

        // the main action: a big menu item in the middle, lit like the selected entry of the game's menus
        bool can = g_tab == 0 ? !Trim(Narrow(g_addr)).empty() : inGame || hostSlot != 0;
        if ((w::Button("go", cx, cy, cw, bh + 10 * s, g_tab == 0 ? TR("Подключиться", "Connect") : TR("Создать сервер", "Start the server"),
                       can, true, true) ||
             (enter && can)) &&
            ApplyNick()) {
            if (g_tab == 0) flow::Join(Trim(Narrow(g_addr)), PortValue(), Narrow(g_password));
            else flow::Host(inGame ? 0 : hostSlot, PortValue(), Narrow(g_password));
        }
        cy += bh + 10 * s + 14 * s;

        std::string error = !g_formError.empty() ? g_formError : flow::LastError();
        if (!error.empty()) para(error, kSaRed, FONT_NORMAL, true);
        else if (g_tab == 0 && inGame)
            para(TR("Текущая игра закроется. Вместо неё загрузится мир друга.",
                    "Your current game will close, and your friend's world will load instead."),
                 kSaMuted, FONT_SMALL, true);
        else if (g_tab == 1 && !inGame && !hostSlot)
            para(TR("В этом слоте нет сохранения.", "This slot has no save."), kSaMuted, FONT_SMALL, true);
        else if (g_tab == 1 && !inGame && hostSlot < 0)
            para(TR("Пролог вы пройдёте сами, а друг появится рядом, когда CJ сядет на велосипед.",
                    "You play the prologue alone, and your friend appears next to you when CJ gets on the bike."),
                 kSaMuted, FONT_SMALL, true);
    }

    // ---- footer: like "Back" on the game's pages
    g.Rect(cx, bodyBottom, cw, std::max(1.0f, s), kSaLine);
    if (online || busy) {
        if (item("leave", cx, by, online ? TR("Отключиться", "Disconnect") : TR("Отмена", "Cancel"))) flow::Leave();
    }
    std::string close = online || busy || inGame ? TR("Закрыть", "Close") : TR("Одиночная игра", "Single player");
    if (item("close", cx + cw - g.TextWidth(FONT_MENU, close), by, close)) SetConnectMenuVisible(false);
    g.Text(FONT_SMALL, cx, fy,
           TR("{ACCBF1}F9{8D9CAD}  окно     {ACCBF1}T{8D9CAD}  чат     {ACCBF1}B{8D9CAD}  голос     "
              "{ACCBF1}G{8D9CAD}  сесть пассажиром     {ACCBF1}F2{8D9CAD}  игроки",
              "{ACCBF1}F9{8D9CAD}  window     {ACCBF1}T{8D9CAD}  chat     {ACCBF1}B{8D9CAD}  voice     "
              "{ACCBF1}G{8D9CAD}  passenger     {ACCBF1}F2{8D9CAD}  players"),
           kSaMuted, TEXT_SHADOW);
    w::EndFrame();
    w::DrawCursor();
    // Esc closes even when a text box is focused (handled in OnKeyDown)
}

}  // namespace coop::ui
