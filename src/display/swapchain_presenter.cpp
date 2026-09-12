#include "swapchain_presenter.h"
#include <iostream>

namespace fsrng {

SwapchainPresenter::SwapchainPresenter() = default;

SwapchainPresenter::~SwapchainPresenter() {
    WaitForGpu();
    if (fenceEvent_) {
        CloseHandle(fenceEvent_);
        fenceEvent_ = nullptr;
    }
}

bool SwapchainPresenter::Initialize(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* directQueue, int width, int height) {
    if (!hwnd || !device || !directQueue || width <= 0 || height <= 0) {
        error_ = "Invalid parameters for SwapchainPresenter::Initialize";
        return false;
    }

    hwnd_ = hwnd;
    device_ = device;
    queue_ = directQueue;
    width_ = width;
    height_ = height;
    format_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    HRESULT hr = device_->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(cmdAlloc_.GetAddressOf())
    );
    if (FAILED(hr)) {
        error_ = "CreateCommandAllocator failed: " + std::to_string(hr);
        return false;
    }

    hr = device_->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        cmdAlloc_.Get(),
        nullptr,
        IID_PPV_ARGS(cmdList_.GetAddressOf())
    );
    if (FAILED(hr)) {
        error_ = "CreateCommandList failed: " + std::to_string(hr);
        return false;
    }
    cmdList_->Close();

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "CreateFence failed: " + std::to_string(hr);
        return false;
    }
    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) {
        error_ = "CreateEvent for fence failed";
        return false;
    }

    if (!CreateRtvHeap()) return false;

    // Create DXGI SwapChain
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "CreateDXGIFactory1 failed: " + std::to_string(hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.Stereo = FALSE;
    scDesc.SampleDesc.Count = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = BufferCount;
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(
        queue_,
        hwnd_,
        &scDesc,
        nullptr,
        nullptr,
        swapChain1.GetAddressOf()
    );

    if (FAILED(hr)) {
        // Fallback without tearing flag
        scDesc.Flags = 0;
        hr = factory->CreateSwapChainForHwnd(
            queue_,
            hwnd_,
            &scDesc,
            nullptr,
            nullptr,
            swapChain1.GetAddressOf()
        );
    }

    if (FAILED(hr)) {
        error_ = "CreateSwapChainForHwnd failed: " + std::to_string(hr);
        return false;
    }

    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain1.As(&swapChain_);
    if (FAILED(hr)) {
        error_ = "Failed to query IDXGISwapChain3";
        return false;
    }

    currentBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();

    return CreateBackbufferResources();
}

bool SwapchainPresenter::CreateRtvHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = BufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(rtvHeap_.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "CreateDescriptorHeap (RTV) failed";
        return false;
    }

    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool SwapchainPresenter::CreateBackbufferResources() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < BufferCount; ++i) {
        HRESULT hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(backBuffers_[i].ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            error_ = "Failed to get swapchain backbuffer " + std::to_string(i);
            return false;
        }

        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescriptorSize_;
    }

    return true;
}

void SwapchainPresenter::WaitForGpu() {
    if (!queue_ || !fence_ || !fenceEvent_) return;

    fenceValue_++;
    queue_->Signal(fence_.Get(), fenceValue_);

    if (fence_->GetCompletedValue() < fenceValue_) {
        fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

bool SwapchainPresenter::Present(ID3D12Resource* upscaledSource, std::function<void(ID3D12GraphicsCommandList*)> overlayCb, bool vsync) {
    (void)vsync;
    if (!swapChain_ || !upscaledSource) return false;

    cmdAlloc_->Reset();
    cmdList_->Reset(cmdAlloc_.Get(), nullptr);

    ID3D12Resource* currentBackBuffer = backBuffers_[currentBufferIndex_].Get();

    // Resource Barriers
    D3D12_RESOURCE_BARRIER barriers[2]{};

    // BackBuffer: PRESENT -> COPY_DEST
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = currentBackBuffer;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Upscaled Source: UNORDERED_ACCESS -> COPY_SOURCE
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = upscaledSource;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList_->ResourceBarrier(2, barriers);

    // Blit copy into swapchain backbuffer
    cmdList_->CopyResource(currentBackBuffer, upscaledSource);

    // Revert upscaledSource barrier back to UNORDERED_ACCESS
    D3D12_RESOURCE_BARRIER revertSource{};
    revertSource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    revertSource.Transition.pResource = upscaledSource;
    revertSource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    revertSource.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    revertSource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList_->ResourceBarrier(1, &revertSource);

    // If Dear ImGui overlay callback is present, transition backbuffer to RENDER_TARGET and draw UI
    if (overlayCb) {
        D3D12_RESOURCE_BARRIER rtBarrier{};
        rtBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rtBarrier.Transition.pResource = currentBackBuffer;
        rtBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        rtBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        rtBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &rtBarrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (currentBufferIndex_ * rtvDescriptorSize_);
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        overlayCb(cmdList_.Get());

        // Transition backbuffer: RENDER_TARGET -> PRESENT
        rtBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        rtBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmdList_->ResourceBarrier(1, &rtBarrier);
    } else {
        // Transition backbuffer: COPY_DEST -> PRESENT
        D3D12_RESOURCE_BARRIER presentBarrier{};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource = currentBackBuffer;
        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &presentBarrier);
    }

    cmdList_->Close();

    ID3D12CommandList* cmdLists[] = { cmdList_.Get() };
    queue_->ExecuteCommandLists(1, cmdLists);

    // Present with VSync enabled (1, 0) to synchronize with monitor refresh rate
    // and prevent GPU/CPU saturation from unbounded rendering loops
    HRESULT hr = swapChain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        error_ = "DXGI device removed/reset during present";
        return false;
    }

    WaitForGpu();
    currentBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();

    return true;
}

bool SwapchainPresenter::Resize(int width, int height) {
    if (width <= 0 || height <= 0 || !swapChain_) return false;
    if (width_ == width && height_ == height) return true;

    WaitForGpu();

    for (UINT i = 0; i < BufferCount; ++i) {
        backBuffers_[i].Reset();
    }

    width_ = width;
    height_ = height;

    HRESULT hr = swapChain_->ResizeBuffers(
        BufferCount,
        width,
        height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
    );

    if (FAILED(hr)) {
        hr = swapChain_->ResizeBuffers(
            BufferCount,
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            0
        );
    }

    if (FAILED(hr)) {
        error_ = "ResizeBuffers failed: " + std::to_string(hr);
        return false;
    }

    currentBufferIndex_ = swapChain_->GetCurrentBackBufferIndex();
    return CreateBackbufferResources();
}

} // namespace fsrng
