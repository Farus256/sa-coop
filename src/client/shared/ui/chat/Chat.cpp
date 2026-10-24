#include "shared/ui/chat/Chat.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

Chat& TheChat() {
    static Chat c;
    return c;
}

static constexpr size_t kMaxLines = 200;
static constexpr int kVisibleLines = 10;
static constexpr size_t kMaxInput = 160;
static constexpr uint64_t kFadeAfterMs = 30000;

void Chat::Add(const std::string& text, uint32_t argb) {
    LOGI("[chat] %s", text.c_str());
    m_lines.push_back({Widen(text), argb, NowMs(), m_nextId++});
    while (m_lines.size() > kMaxLines) m_lines.pop_front();
}

void Chat::Open() {
    if (m_open) return;
    m_open = true;
    m_openTime = NowMs();
    m_input.clear();
    m_cursor = 0;
    m_histPos = -1;
    m_scroll = 0;
    m_selFrom = m_selTo = 0;
    m_dragging = false;
    In().SetKeyboardCapture(true);
    In().SetMouseCapture(true);  // the mouse is the chat's: scroll its history, pick messages to copy
}

void Chat::Close() {
    if (!m_open) return;
    m_open = false;
    m_scroll = 0;
    m_selFrom = m_selTo = 0;
    m_dragging = false;
    In().SetKeyboardCapture(false);
    In().SetMouseCapture(false);
}

// the picked messages to the clipboard, one a line, without their colour codes
void Chat::Copy() {
    if (!m_selFrom) return;
    uint64_t a = std::min(m_selFrom, m_selTo), b = std::max(m_selFrom, m_selTo);
    std::wstring out;
    for (const Line& l : m_lines) {
        if (l.id < a || l.id > b) continue;
        std::wstring t;
        for (size_t i = 0; i < l.text.size(); ++i) {
            if (l.text[i] == L'{' && i + 7 < l.text.size() && l.text[i + 7] == L'}') {  // {RRGGBB}
                bool hex = true;
                for (size_t k = 1; k <= 6; ++k) hex &= iswxdigit(l.text[i + k]) != 0;
                if (hex) {
                    i += 7;
                    continue;
                }
            }
            t += l.text[i];
        }
        if (!out.empty()) out += L"\r\n";
        out += t;
    }
    if (out.empty() || !OpenClipboard((HWND)In().Hwnd())) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (out.size() + 1) * sizeof(wchar_t))) {
        if (auto* p = (wchar_t*)GlobalLock(h)) {
            std::copy(out.begin(), out.end(), p);
            p[out.size()] = 0;
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    m_copiedAt = NowMs();
}

static std::wstring ReadClipboard() {
    std::wstring out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    for (auto& c : out)
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    return out;
}

void Chat::ProcessInput() {
    if (!m_open) return;
    auto& st = In().State();
    for (int vk : st.keyDowns) {
        switch (vk) {
            case VK_RETURN: {
                std::wstring text = m_input;
                Close();
                // trim
                while (!text.empty() && text.back() == L' ') text.pop_back();
                size_t start = text.find_first_not_of(L' ');
                if (start == std::wstring::npos) return;
                text = text.substr(start);
                if (m_history.empty() || m_history.back() != text) m_history.push_back(text);
                if (m_history.size() > 50) m_history.erase(m_history.begin());
                if (onSubmit) onSubmit(Narrow(text));
                return;
            }
            case VK_ESCAPE:
                Close();
                return;
            case VK_BACK:
                if (m_cursor > 0) {
                    m_input.erase(m_cursor - 1, 1);
                    --m_cursor;
                }
                break;
            case VK_DELETE:
                if (m_cursor < m_input.size()) m_input.erase(m_cursor, 1);
                break;
            case VK_LEFT:
                if (m_cursor > 0) --m_cursor;
                break;
            case VK_RIGHT:
                if (m_cursor < m_input.size()) ++m_cursor;
                break;
            case VK_HOME:
                m_cursor = 0;
                break;
            case VK_END:
                m_cursor = m_input.size();
                break;
            case VK_UP:
                if (!m_history.empty()) {
                    if (m_histPos < 0) m_histPos = (int)m_history.size();
                    if (m_histPos > 0) --m_histPos;
                    m_input = m_history[m_histPos];
                    m_cursor = m_input.size();
                }
                break;
            case VK_DOWN:
                if (m_histPos >= 0) {
                    ++m_histPos;
                    if (m_histPos >= (int)m_history.size()) {
                        m_histPos = -1;
                        m_input.clear();
                    } else {
                        m_input = m_history[m_histPos];
                    }
                    m_cursor = m_input.size();
                }
                break;
            case VK_PRIOR:
                m_scroll = std::min<int>(m_scroll + 5, std::max<int>(0, (int)m_lines.size() - kVisibleLines));
                break;
            case VK_NEXT:
                m_scroll = std::max(0, m_scroll - 5);
                break;
            case 'C':
                if (st.ctrl && m_selFrom) Copy();
                break;
            case 'A':
                if (st.ctrl && !m_lines.empty() && m_input.empty()) m_selFrom = m_lines.front().id, m_selTo = m_lines.back().id;
                break;
            case 'V':
                if (st.ctrl) {
                    std::wstring clip = ReadClipboard();
                    size_t room = kMaxInput > m_input.size() ? kMaxInput - m_input.size() : 0;
                    if (clip.size() > room) clip.resize(room);
                    m_input.insert(m_cursor, clip);
                    m_cursor += clip.size();
                }
                break;
            default:
                break;
        }
    }
    for (wchar_t c : st.chars) {
        if (st.ctrl) continue;  // Ctrl+V etc. produce control chars; already filtered
        if (m_input.size() >= kMaxInput) break;
        m_input.insert(m_input.begin() + (ptrdiff_t)m_cursor, c);
        ++m_cursor;
    }
    if (st.wheel) {
        m_scroll = std::clamp(m_scroll + st.wheel, 0, std::max<int>(0, (int)m_lines.size() - kVisibleLines));
    }
}

void Chat::Render() {
    auto& g = Gfx();
    if (!g.Ready()) return;
    float s = g.Scale();
    float x = 24 * s, y = 24 * s;
    float width = std::max(520.0f * s, g.Width() * 0.36f);
    float lh = g.LineHeight(FONT_NORMAL) * 0.95f;
    uint64_t now = NowMs();

    // collect wrapped lines from the end
    struct Vis {
        std::wstring text;
        uint32_t color;
        float alpha;
        uint64_t id;
    };
    std::vector<Vis> vis;
    int skip = m_scroll;
    for (auto it = m_lines.rbegin(); it != m_lines.rend() && (int)vis.size() < kVisibleLines; ++it) {
        float alpha = 1.0f;
        if (!m_open) {
            uint64_t age = now - it->time;
            if (age > kFadeAfterMs + 2000) break;
            if (age > kFadeAfterMs) alpha = 1.0f - (float)(age - kFadeAfterMs) / 2000.0f;
        }
        auto wrapped = g.Wrap(FONT_NORMAL, it->text, width);
        for (auto w = wrapped.rbegin(); w != wrapped.rend() && (int)vis.size() < kVisibleLines; ++w) {
            if (skip > 0) {
                --skip;
                continue;
            }
            vis.push_back({*w, it->color, alpha, it->id});
        }
    }
    if (m_open) {
        g.Rect(x - 8 * s, y - 6 * s, width + 16 * s, lh * kVisibleLines + 12 * s, 0x60000000);
    }
    // the mouse: a press on a message picks it, a drag picks the ones between; Ctrl+C copies them (Ctrl+A: all)
    if (m_open) {
        auto& st = In().State();
        uint64_t under = 0;
        if (st.mouseX >= x - 8 * s && st.mouseX <= x + width + 8 * s) {
            float ry = y + lh * (kVisibleLines - 1);
            for (auto& v : vis) {
                if (st.mouseY >= ry && st.mouseY < ry + lh) under = v.id;
                ry -= lh;
            }
        }
        bool inLog = st.mouseX >= x - 8 * s && st.mouseX <= x + width + 8 * s && st.mouseY >= y - 6 * s && st.mouseY <= y + lh * kVisibleLines + 6 * s;
        if (st.lmbPressed) {
            if (under) m_selFrom = m_selTo = under, m_dragging = true;
            else if (inLog) m_selFrom = m_selTo = 0;
        }
        if (m_dragging && st.lmb && under) m_selTo = under;
        if (!st.lmb) m_dragging = false;
    }
    uint64_t selA = std::min(m_selFrom, m_selTo), selB = std::max(m_selFrom, m_selTo);
    float cy = y + lh * (kVisibleLines - 1);
    for (auto& v : vis) {
        if (m_open && m_selFrom && v.id >= selA && v.id <= selB) g.Rect(x - 6 * s, cy, width + 12 * s, lh, 0x704A78C8);
        g.TextW(FONT_NORMAL, x, cy, v.text, WithAlpha(v.color, v.alpha), TEXT_OUTLINE);
        cy -= lh;
    }
    if (m_open) {
        float iy = y + lh * kVisibleLines + 10 * s;
        float ih = lh + 8 * s;
        g.Rect(x - 8 * s, iy, width + 16 * s, ih, 0xB0101010);
        g.RectOutline(x - 8 * s, iy, width + 16 * s, ih, 0xFF5A82C8);
        const wchar_t* prefix = TR(L"Сказать: ", L"Say: ");
        std::wstring shown = prefix + m_input;
        float prefixW = g.TextWidthW(FONT_NORMAL, prefix);
        // scroll the input horizontally if it is too long
        float caretX = prefixW + g.TextWidthW(FONT_NORMAL, std::wstring_view(m_input).substr(0, m_cursor));
        float offset = std::max(0.0f, caretX - width + 10 * s);
        g.TextW(FONT_NORMAL, x - offset, iy + 4 * s, shown, 0xFFFFFFFF, TEXT_SHADOW);
        if (((now - m_openTime) / 500) % 2 == 0) g.Rect(x - offset + caretX, iy + 5 * s, 2 * s, lh - 2 * s, 0xFFFFFFFF);
        if (m_scroll > 0)
            g.Text(FONT_SMALL, x + width, y - 4 * s, Format("▲ %d", m_scroll), 0xFFB0B0B0, TEXT_RIGHT | TEXT_SHADOW);
        std::string hint = now - m_copiedAt < 1500 ? std::string(TR("Скопировано", "Copied"))
                         : m_selFrom ? std::string(TR("Ctrl+C — копировать", "Ctrl+C: copy"))
                                     : std::string(TR("Колесо — история, мышь — выбрать сообщения", "Wheel: history, mouse: pick messages"));
        g.Text(FONT_SMALL, x + width, iy + ih + 4 * s, hint, 0xFFB0B0B0, TEXT_RIGHT | TEXT_SHADOW);
        w::DrawCursor();
    }
}

}  // namespace coop::ui
