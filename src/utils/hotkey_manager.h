#pragma once
#include <windows.h>
#include <functional>
#include <atomic>

namespace fsrng {

class HotkeyManager {
public:
    enum Modifier : UINT {
        MOD_NONE_KEY  = 0,
        MOD_CTRL_KEY  = MOD_CONTROL, // 0x0002
        MOD_SHIFT_KEY = MOD_SHIFT,   // 0x0004
        MOD_ALT_KEY   = MOD_ALT,     // 0x0001
    };

    explicit HotkeyManager(HWND hwnd);
    ~HotkeyManager();

    // Register a hotkey. Returns the assigned id (>=0) or -1 on failure.
    int Register(int vkCode, UINT modifiers, std::function<void()> callback);

    // Translate WM_HOTKEY into callbacks. Call from the message loop.
    bool OnHotKey(WPARAM wParam, LPARAM lParam);

    void UnregisterAll();

private:
    HWND hwnd_ = nullptr;
    std::atomic<int> nextId_{1};
    int ids_[8]{};
    std::function<void()> callbacks_[8]{};
    bool registered_[8]{};
};

} // namespace fsrng
