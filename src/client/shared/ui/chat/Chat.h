#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace coop::ui {

class Chat {
public:
    // text: UTF-8, may contain colour codes {RRGGBB}; argb: base colour.
    void Add(const std::string& text, uint32_t argb = 0xFFFFFFFF);
    void AddSystem(const std::string& text) { Add(text, 0xFFFFC864); }
    void AddError(const std::string& text) { Add(text, 0xFFFF6464); }

    void Open();
    void Close();
    bool IsOpen() const { return m_open; }

    // Feeds per-frame input while open. Returns true if the chat should close.
    void ProcessInput();
    void Render();

    std::function<void(const std::string& utf8)> onSubmit;

private:
    struct Line {
        std::wstring text;
        uint32_t color;
        uint64_t time;
        uint64_t id;  // its number (the selection names the messages by it)
    };
    void Copy();
    uint64_t m_nextId = 1;
    // the messages picked with the mouse (from the one pressed on to the one under the mouse), 0: none
    uint64_t m_selFrom = 0, m_selTo = 0;
    bool m_dragging = false;
    uint64_t m_copiedAt = 0;
    std::deque<Line> m_lines;
    std::wstring m_input;
    size_t m_cursor = 0;
    std::vector<std::wstring> m_history;
    int m_histPos = -1;
    int m_scroll = 0;
    bool m_open = false;
    uint64_t m_openTime = 0;
};

Chat& TheChat();

}  // namespace coop::ui
