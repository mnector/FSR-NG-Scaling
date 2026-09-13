#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace fsrng {

struct WindowClientInfo {
    bool valid = false;
    int x = 0;
    int y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string title = "";
    HWND hwnd = nullptr;
};

class CaptureManager {
public:
    CaptureManager();
    ~CaptureManager();

    bool Initialize(ID3D12Device* d3d12Device);
    bool Start(HWND targetWindow = nullptr);
    void Stop();

    ID3D12Resource* AcquireLatestFrame(bool* newFrame = nullptr);
    void* GetD3D12KeyedMutex() { return nullptr; }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const std::string& error() const { return error_; }

    WindowClientInfo GetForegroundClientArea(HWND excludeHwnd);

private:
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device_;
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12SharedResource_;
    HANDLE serverProcess_ = nullptr;
    
    uint32_t width_ = 1920;
    uint32_t height_ = 1080;
    std::string error_;
};

}
