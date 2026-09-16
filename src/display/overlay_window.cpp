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
        if (msg != WM_NCHITTEST && self->onMsg_(hwnd, msg, wParam, lParam)) {
            return 0;
        }
    }

    switch (msg) {
        case WM_CLOSE:
            std::cerr << "[OverlayWindow] WM_CLOSE received" << std::endl;
            if (self && self->onClose_) {
                self->onClose_();
            }
            PostQuitMessage(0);
            return 0;

        case WM_NCHITTEST: {
            if (self && !self->clickThrough_ && self->onHitTest_) {
                POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                ScreenToClient(hwnd, &pt);
                if (self->onHitTest_(pt.x, pt.y)) {
                    return HTCLIENT;
                }
                return HTTRANSPARENT;
            }
            return (self && self->clickThrough_) ? HTTRANSPARENT : HTCLIENT;
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

    // Start as Click-Through Ghost (Game Mode)
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT;
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

    // Still exclude from capture to prevent infinite mirror of desktop
    SetWindowDisplayAffinity(hwnd_, 0x00000011); // WDA_EXCLUDEFROMCAPTURE

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    return true;
}

void OverlayWindow::Show(bool visible) {
    if (!hwnd_) return;
    isVisible_ = visible;
    if (visible) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        // Force immediate topmost Z-order placement
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void OverlayWindow::BringToTop() {
    if (hwnd_ && isVisible_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void OverlayWindow::SetPositionAndSize(int x, int y, int w, int h) {
    if (hwnd_) {
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER);
        width_ = w;
        height_ = h;
    }
}

void OverlayWindow::SetClickThrough(bool enable) {
    if (!hwnd_) return;
    clickThrough_ = enable;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (enable) {
        // Game Mode: Ghost, Topmost, NoActivate, Toolwindow
        exStyle |= (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    } else {
        // Menu Mode: Solid, Standard, Activatable
        exStyle &= ~(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, exStyle);

    if (enable) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }
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
