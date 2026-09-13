#include "capture_manager.h"
#include <iostream>
#include <string>

namespace fsrng {

CaptureManager::CaptureManager() {}
CaptureManager::~CaptureManager() { Stop(); }

bool CaptureManager::Initialize(ID3D12Device* d3d12Device) {
    d3d12Device_ = d3d12Device;
    width_ = GetSystemMetrics(SM_CXSCREEN);
    height_ = GetSystemMetrics(SM_CYSCREEN);
    return true;
}

bool CaptureManager::Start(HWND targetWindow) {
    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;
    CreatePipe(&hRead, &hWrite, &saAttr, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.hStdOutput = hWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(nullptr, (LPSTR)"capture_server.exe", nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error_ = "Failed to start capture_server.exe";
        return false;
    }
    serverProcess_ = pi.hProcess;
    CloseHandle(pi.hThread);
    CloseHandle(hWrite);

    char buffer[256];
    DWORD read;
    std::string outStr;
    while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &read, NULL) && read > 0) {
        buffer[read] = '\0';
        outStr += buffer;
        if (outStr.find('\n') != std::string::npos) break;
    }
    CloseHandle(hRead);

    size_t pos = outStr.find("HANDLE:");
    if (pos == std::string::npos) {
        error_ = "Capture server failed to output handle.";
        return false;
    }
    
    unsigned long long handleVal = std::stoull(outStr.substr(pos + 7));
    HANDLE childHandle = (HANDLE)handleVal;

    HANDLE localHandle = nullptr;
    if (!DuplicateHandle(serverProcess_, childHandle, GetCurrentProcess(), &localHandle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        error_ = "Failed to duplicate handle from capture server.";
        return false;
    }

    HRESULT hr = d3d12Device_->OpenSharedHandle(localHandle, IID_PPV_ARGS(&d3d12SharedResource_));
    CloseHandle(localHandle);

    if (FAILED(hr)) {
        error_ = "Failed to open shared handle in D3D12.";
        return false;
    }
    return true;
}

void CaptureManager::Stop() {
    d3d12SharedResource_.Reset();
    if (serverProcess_) {
        TerminateProcess(serverProcess_, 0);
        CloseHandle(serverProcess_);
        serverProcess_ = nullptr;
    }
}

ID3D12Resource* CaptureManager::AcquireLatestFrame(bool* newFrame) {
    if (newFrame) *newFrame = true;
    return d3d12SharedResource_.Get();
}

WindowClientInfo CaptureManager::GetForegroundClientArea(HWND excludeHwnd) {
    WindowClientInfo info{};
    HWND fg = GetForegroundWindow();
    if (!fg || fg == excludeHwnd || fg == GetDesktopWindow() || fg == GetShellWindow()) return info;

    RECT clientRect;
    if (GetClientRect(fg, &clientRect)) {
        POINT pt = { 0, 0 };
        ClientToScreen(fg, &pt);
        info.valid = true;
        info.x = pt.x;
        info.y = pt.y;
        info.width = clientRect.right - clientRect.left;
        info.height = clientRect.bottom - clientRect.top;
        
        char title[256];
        if (GetWindowTextA(fg, title, sizeof(title))) {
            info.title = title;
        }
    }
    return info;
}

}
