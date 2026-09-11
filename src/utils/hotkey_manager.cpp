#include "hotkey_manager.h"

namespace fsrng {

HotkeyManager::HotkeyManager(HWND hwnd) : hwnd_(hwnd) {
    for (int i = 0; i < 8; ++i) {
        ids_[i] = -1;
        registered_[i] = false;
    }
}

HotkeyManager::~HotkeyManager() {
    UnregisterAll();
}

int HotkeyManager::Register(int vkCode, UINT modifiers, std::function<void()> callback) {
    for (int i = 0; i < 8; ++i) {
        if (!registered_[i]) {
            int newId = nextId_++;
            BOOL ok = ::RegisterHotKey(hwnd_, newId, modifiers, static_cast<UINT>(vkCode));
            if (ok) {
                ids_[i] = newId;
                callbacks_[i] = std::move(callback);
                registered_[i] = true;
                return ids_[i];
            }
        }
    }
    return -1; // no free slot
}

bool HotkeyManager::OnHotKey(WPARAM wParam, LPARAM) {
    for (int i = 0; i < 8; ++i) {
        if (registered_[i] && ids_[i] == static_cast<int>(wParam)) {
            if (callbacks_[i]) {
                callbacks_[i]();
            }
            return true;
        }
    }
    return false;
}

void HotkeyManager::UnregisterAll() {
    if (!hwnd_) return;
    for (int i = 0; i < 8; ++i) {
        if (registered_[i]) {
            ::UnregisterHotKey(hwnd_, ids_[i]);
            registered_[i] = false;
        }
    }
}

} // namespace fsrng
