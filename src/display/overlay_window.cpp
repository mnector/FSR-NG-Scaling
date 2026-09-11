#include "overlay_window.h"
#include <dwmapi.h>
#include <iostream>

namespace fsrng {

OverlayWindow::OverlayWindow() = default;

OverlayWindow::~OverlayWindow() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (self && self->onMsg_) {
        if (self->onMsg_(hwnd, msg, wParam, lParam)) {
            return 0;
        }
    }

    switch (msg) {
        case WM_CLOSE:
            if (self && self->onClose_) {
                self->onClose_();
            }
            PostQuitMessage(0);
            return 0;

        case WM_NCHITTEST: {
            if (self && self->onHitTest_) {
                int screenX = static_cast<short>(LOWORD(lParam));
                int screenY = static_cast<short>(HIWORD(lParam));
                if (self->onHitTest_(screenX, screenY)) {
                    return HTCLIENT;
                }
            }
            return HTTRANSPARENT;
        }

        case WM_HOTKEY:
            if (self && self->onHotKey_) {
                self->onHotKey_(wParam, lParam);
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_ERASEBKGND:
            return 1; // Prevent background flickering

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool OverlayWindow::Create(const std::string& title, int width, int height) {
    hInstance_ = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"FSRNG_OverlayClass";

    RegisterClassExW(&wc);

    int screenW = width > 0 ? width : GetSystemMetrics(SM_CXSCREEN);
    int screenH = height > 0 ? height : GetSystemMetrics(SM_CYSCREEN);
    width_ = screenW;
    height_ = screenH;

    // Extended styles:
    // - WS_EX_LAYERED: Enables GPU alpha blending and non-intrusive compositing
    // - WS_EX_NOACTIVATE: Never steals keyboard/mouse focus from the game
    // - WS_EX_TOOLWINDOW: Prevents Windows 11 Focus Assist from triggering "No Molestar" (fullscreen gaming mode)
    // - WS_EX_TOPMOST: Stays as an overlay on top of the game
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    DWORD style = WS_POPUP;

    std::wstring wTitle(title.begin(), title.end());

    hwnd_ = CreateWindowExW(
        exStyle,
        L"FSRNG_OverlayClass",
        wTitle.c_str(),
        style,
        0, 0,
        screenW, screenH,
        nullptr,
        nullptr,
        hInstance_,
        nullptr
    );

    if (!hwnd_) {
        std::cerr << "[OverlayWindow] CreateWindowExW failed: " << GetLastError() << std::endl;
        return false;
    }

    // Exclude overlay from DXGI / WGC screen capture to prevent recursive black screen
    SetWindowDisplayAffinity(hwnd_, 0x00000011); // WDA_EXCLUDEFROMCAPTURE

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    return true;
}

void OverlayWindow::Show(bool visible) {
    if (!hwnd_) return;
    isVisible_ = visible;
    if (visible) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, width_, height_, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void OverlayWindow::SetClickThrough(bool enable) {
    if (!hwnd_) return;
    clickThrough_ = enable;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enable) {
        exStyle |= WS_EX_TRANSPARENT;
    } else {
        exStyle &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);
}

bool OverlayWindow::ProcessMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

} // namespace fsrng
