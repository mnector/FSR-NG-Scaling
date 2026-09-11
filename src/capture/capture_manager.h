#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include "frame_pool.h"

// Forward declaration of D3D11 types to avoid requiring D3D11 headers in public interface
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Query;

namespace fsrng {

struct WindowClientInfo {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool valid = false;
    HWND hwnd = nullptr;
    std::string title;
};

class CaptureManager {
public:
    CaptureManager();
    ~CaptureManager();

    // Initialize capture engine with the target D3D12 device
    bool Initialize(ID3D12Device* d3d12Device);

    // Start desktop or target window capture via IDXGIOutputDuplication
    bool Start(HWND targetWindow = nullptr);
    bool StartByTitle(const std::string& windowTitle);

    void Stop();

    // Acquire the latest available frame as a D3D12 resource
    ID3D12Resource* AcquireLatestFrame();

    // Query active foreground window client rect in screen coordinates (excluding titlebar/borders)
    WindowClientInfo GetForegroundClientArea(HWND excludeHwnd = nullptr);

    bool active() const { return active_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const std::string& error() const { return error_; }

private:
    bool InitializeD3D11();
    bool SetupDuplication();
    bool CreateSharedTexture(uint32_t w, uint32_t h);

    ID3D12Device* d3d12Device_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11Context_;
    Microsoft::WRL::ComPtr<ID3D11Query> flushQuery_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyedMutex_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12SharedResource_;
    Microsoft::WRL::ComPtr<IUnknown> d3d11SharedTexture_;

    HWND targetWindow_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t frameCount_ = 0;
    bool active_ = false;
    std::string error_;

    HMODULE d3d11Dll_ = nullptr;
};

} // namespace fsrng
