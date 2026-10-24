#include "shared/ui/input/Input.h"

#include <windows.h>

#include <cstring>
#include <string>

#include "common/lib/log/Log.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop::ui {

Input& In() {
    static Input i;
    return i;
}

static WNDPROC g_origProc = nullptr;

// The game's DirectInput mouse (psGlobal: window, instance, fullScreen, lastMousePos, ?, the DirectInput interface,
// then the mouse). Acquired, it holds the system's cursor where it is (exclusive), and our menu's and the chat's
// cursor did not move: while the mouse is ours it is let go (Unacquire, every frame: the game acquires it again on
// its own), and taken again after. Found as the second pointer there that is a DirectInput object (its vtable in
// dinput8.dll), so a wrong guess at the layout leaves it alone.
static bool InDinput(const void* p) {
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &mod) ||
        !GetModuleFileNameW(mod, path, MAX_PATH))
        return false;
    std::wstring name = path;
    CharLowerBuffW(name.data(), (DWORD)name.size());
    return name.find(L"dinput") != std::wstring::npos;
}
static void* GameMouse() {
    void* ps = game::Ref<void*>(game::addr::RsGlobal + 0x14);
    if (!ps || IsBadReadPtr(ps, 0x30)) return nullptr;
    int found = 0;
    for (uintptr_t off = 0x10; off <= 0x28; off += 4) {
        void* obj = game::Field<void*>(ps, off);
        if (!obj || IsBadReadPtr(obj, 4) || IsBadReadPtr(*(void**)obj, 0x40) || !InDinput(*(void**)obj)) continue;
        if (++found == 2) return obj;
    }
    return nullptr;
}
static void MouseDevice(bool acquire) {
    void* dev = GameMouse();
    if (!dev) return;
    using Fn = long(__stdcall*)(void*);
    Fn fn = (Fn)(*(void***)dev)[acquire ? 7 : 8];  // IDirectInputDevice8::Acquire / Unacquire
    fn(dev);
}

static LRESULT CALLBACK CoopWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (In().OnMessage(msg, wp, lp)) return 0;
    return CallWindowProcW(g_origProc, hwnd, msg, wp, lp);
}

void Input::Install(void* hwnd) {
    if (m_hwnd || !hwnd) return;
    m_hwnd = hwnd;
    g_origProc = (WNDPROC)SetWindowLongPtrW((HWND)hwnd, GWLP_WNDPROC, (LONG_PTR)&CoopWndProc);
    LOGI("Window procedure subclassed (hwnd %p)", hwnd);
}

void Input::ClearGameKeys() {
    // the new, old and the "temp" state the window messages write and the pads copy every frame: a key held when the
    // chat opened would stay down there for good
    game::Call<0x53F1E0>();  // CPad::ClearKeyBoardHistory
    // also release the pad buttons of the local player
    game::CPad* pad = game::GetPad(0);
    memset(&pad->NewState, 0, sizeof(pad->NewState));
    memset(&pad->OldState, 0, sizeof(pad->OldState));
}

void Input::SetKeyboardCapture(bool on) {
    if (on && !m_kbCapture) ClearGameKeys();
    m_kbCapture = on;
}

void Input::SetMouseCapture(bool on) {
    if (on == m_mouseCapture) return;
    m_mouseCapture = on;
    MouseDevice(!on);
    if (on) ClipCursor(nullptr);
    if (on) {
        // put the cursor in the middle so the user sees it
        RECT rc;
        if (m_hwnd && GetClientRect((HWND)m_hwnd, &rc)) {
            POINT p{(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
            ClientToScreen((HWND)m_hwnd, &p);
            SetCursorPos(p.x, p.y);
        }
    }
}

void Input::EndFrame() {
    m_state.keyDowns.clear();
    m_state.chars.clear();
    m_state.lmbPressed = false;
    m_state.lmbReleased = false;
    m_state.wheel = 0;
}

void Input::UpdateMouse(float bbW, float bbH) {
    if (!m_hwnd) return;
    if (m_mouseCapture && game::IsForeground()) MouseDevice(false);  // the game may have taken it again
    POINT p;
    GetCursorPos(&p);
    ScreenToClient((HWND)m_hwnd, &p);
    RECT rc;
    GetClientRect((HWND)m_hwnd, &rc);
    float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
    if (cw <= 0 || ch <= 0) return;
    m_state.mouseX = (float)p.x * bbW / cw;
    m_state.mouseY = (float)p.y * bbH / ch;
    m_state.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    m_state.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 && game::IsForeground();
    if (down && !m_state.lmb) m_state.lmbPressed = true;
    if (!down && m_state.lmb) m_state.lmbReleased = true;
    m_state.lmb = down;
}

bool Input::OnMessage(unsigned msg, uintptr_t wParam, intptr_t lParam) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = (int)wParam;
            m_state.keyDowns.push_back(vk);
            // Global hotkeys are handled by the core; it tells us whether to swallow the key.
            bool consumed = core::OnKeyDown(vk, (lParam & (1 << 30)) != 0);
            if (consumed) return true;
            if (m_kbCapture) {
                // keep Alt+F4 / Alt+Tab working
                if (msg == WM_SYSKEYDOWN && (vk == VK_F4 || vk == VK_TAB)) return false;
                return true;
            }
            return false;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            return false;  // releases reach the game even while we type: nothing it does starts on a release
        case WM_CHAR:
            if (m_kbCapture) {
                wchar_t c = (wchar_t)wParam;
                if (c >= 0x20 && c != 0x7F) m_state.chars.push_back(c);
                return true;
            }
            return false;
        case WM_MOUSEWHEEL:
            m_state.wheel += GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            return m_mouseCapture;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return m_mouseCapture;
        case WM_SETFOCUS:
            // Vanilla opens the pause menu when the window regains focus; in co-op that would pause the
            // shared world for everyone, so only restore the foreground flag.
            if (core::IsInCoopSession() && game::GameState() == game::GAME_STATE_IDLE) {
                game::Ref<uint8_t>(game::addr::ForegroundApp) = 1;
                return true;
            }
            return false;
        case WM_SETCURSOR:
            if (m_mouseCapture && LOWORD(lParam) == HTCLIENT) {
                SetCursor(nullptr);  // we draw our own cursor
                return true;
            }
            return false;
        default:
            return false;
    }
}

}  // namespace coop::ui
