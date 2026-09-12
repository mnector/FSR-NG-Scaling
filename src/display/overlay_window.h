#pragma once
#include <windows.h>
#include <string>
#include <functional>

namespace fsrng {

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    // Create borderless fullscreen overlay window
    bool Create(const std::string& title = "FSR-NG-Overlay", int width = 0, int height = 0);

    // Show / Hide
    void Show(bool visible = true);

    // Set click-through (pass input through to underlying game)
    void SetClickThrough(bool enable);

    void SetPositionAndSize(int x, int y, int w, int h);

    // Process pending Windows messages. Returns false on WM_QUIT
    bool ProcessMessages();

    HWND hwnd() const { return hwnd_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool isVisible() const { return isVisible_; }

    // Callback for window events (resize, close, hotkeys, UI interaction)
    void SetCloseCallback(std::function<void()> cb) { onClose_ = std::move(cb); }
    void SetHotKeyCallback(std::function<void(WPARAM, LPARAM)> cb) { onHotKey_ = std::move(cb); }
    void SetMsgCallback(std::function<bool(HWND, UINT, WPARAM, LPARAM)> cb) { onMsg_ = std::move(cb); }
    void SetHitTestCallback(std::function<bool(int, int)> cb) { onHitTest_ = std::move(cb); }

    bool isClickThrough() const { return clickThrough_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool isVisible_ = false;
    bool clickThrough_ = true;
    std::function<void()> onClose_;
    std::function<void(WPARAM, LPARAM)> onHotKey_;
    std::function<bool(HWND, UINT, WPARAM, LPARAM)> onMsg_;
    std::function<bool(int, int)> onHitTest_;
};

} // namespace fsrng
