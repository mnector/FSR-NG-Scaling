#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
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

    bool Initialize(ID3D12Device* d3d12Device, ID3D12CommandQueue* commandQueue, uint32_t captureWidth, uint32_t captureHeight);
    bool Start(HWND targetWindow = nullptr);
    void Stop();

    ID3D12Resource* AcquireLatestFrame(bool* newFrame = nullptr);
    IDXGIKeyedMutex* GetD3D12KeyedMutex() { return d3d12KeyedMutex_.Get(); }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    const std::string& error() const { return error_; }

    WindowClientInfo GetForegroundClientArea(HWND excludeHwnd);

private:
    bool SetupD3D11AndDuplication();
    bool CreateSharedResources();
    bool SetupComputeDownsampler();

    Microsoft::WRL::ComPtr<ID3D12Device> d3d12Device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue_;
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12SharedResource_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> d3d12KeyedMutex_;

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11Context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11SharedTexture_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> d3d11KeyedMutex_;

    // GPU Downscaler resources if desktop resolution != target width/height
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> downscaleCS_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> sharedTexUAV_;

    uint32_t width_ = 1920;
    uint32_t height_ = 1080;
    uint32_t desktopWidth_ = 0;
    uint32_t desktopHeight_ = 0;
    std::string error_;
    bool active_ = false;
    uint64_t frameCount_ = 0;
};

} // namespace fsrng
