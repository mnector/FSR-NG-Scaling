#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <string>

namespace fsrng {

class SwapchainPresenter {
public:
    static constexpr UINT BufferCount = 2;

    SwapchainPresenter();
    ~SwapchainPresenter();

    // Initialize DXGI Flip Discard SwapChain for the overlay HWND
    bool Initialize(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* directQueue, int width, int height);

    // Copy upscaled texture to current backbuffer, transition states, and present
    bool Present(ID3D12Resource* upscaledSource, bool vsync = false);

    // Handle window resize
    bool Resize(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    const std::string& error() const { return error_; }

private:
    bool CreateRtvHeap();
    bool CreateBackbufferResources();
    void WaitForGpu();

    HWND hwnd_ = nullptr;
    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain_;
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffers_[BufferCount];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    UINT rtvDescriptorSize_ = 0;
    UINT currentBufferIndex_ = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> cmdAlloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;

    int width_ = 0;
    int height_ = 0;
    std::string error_;
};

} // namespace fsrng
