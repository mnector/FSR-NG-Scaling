#include "depth_worker.h"
#include <iostream>

namespace fsrng {

DepthWorker::DepthWorker() {}

DepthWorker::~DepthWorker() {
    Shutdown();
}

bool DepthWorker::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width, uint32_t height, const std::wstring& modelPath) {
    device_ = device;
    queue_ = queue;
    width_ = width;
    height_ = height;

    if (!depthManager_.Initialize(modelPath)) {
        return false;
    }

    // Create readback buffer for DXGI_FORMAT_B8G8R8A8_UNORM
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Alignment = 0;
    // We assume 4 bytes per pixel (B8G8R8A8). Need to align row pitch to 256 bytes for D3D12.
    uint32_t rowPitch = (width * 4 + 255) & ~255;
    bufferDesc.Width = rowPitch * height;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(device_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc, 
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuffer_)))) {
        return false;
    }

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readbackFence_)))) {
        return false;
    }

    readbackEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!readbackEvent_) return false;

    processingBuffer_.resize(bufferDesc.Width);
    latestDepthMap_.resize(518 * 518, 1.0f);
    latestVisPixels_.resize(518 * 518, 0xFF000000); // Black opaque

    running_ = true;
    thread_ = std::thread(&DepthWorker::WorkerThread, this);

    return true;
}

void DepthWorker::Shutdown() {
    running_ = false;
    newFrameReady_ = false; // Wake up thread if waiting
    if (readbackEvent_) SetEvent(readbackEvent_);
    if (thread_.joinable()) thread_.join();

    if (readbackEvent_) {
        CloseHandle(readbackEvent_);
        readbackEvent_ = nullptr;
    }
}

void DepthWorker::Update(ID3D12GraphicsCommandList* cmd, ID3D12Resource* inputResource) {
    if (!running_ || processing_.load() || newFrameReady_.load()) return;

    // Issue copy from texture to readback buffer
    D3D12_RESOURCE_DESC td = inputResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSize, totalBytes;
    device_->GetCopyableFootprints(&td, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readbackBuffer_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = inputResource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = inputResource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &barrier);

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1, &barrier);

    // Signal fence after this command list executes
    readbackFenceValue_++;
    pendingSubmit_ = true;
}

void DepthWorker::PostSubmit(ID3D12CommandQueue* queue) {
    if (pendingSubmit_.load()) {
        queue->Signal(readbackFence_.Get(), readbackFenceValue_);
        pendingSubmit_ = false;
        newFrameReady_ = true;
    }
}

std::vector<uint32_t> DepthWorker::GetLatestVisPixels() {
    std::lock_guard<std::mutex> lock(depthMutex_);
    return latestVisPixels_;
}

std::vector<float> DepthWorker::GetLatestDepthMap() {
    std::lock_guard<std::mutex> lock(depthMutex_);
    return latestDepthMap_;
}

void DepthWorker::WorkerThread() {
    while (running_) {
        if (!newFrameReady_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        processing_ = true;
        
        // Wait for GPU to finish the copy.
        // The queue will be signaled by main.cpp immediately after ExecuteCommandLists.
        // Wait, main.cpp doesn't signal readbackFence_. 
        // We can just signal it right now on the queue!
        queue_->Signal(readbackFence_.Get(), readbackFenceValue_);
        
        if (readbackFence_->GetCompletedValue() < readbackFenceValue_) {
            readbackFence_->SetEventOnCompletion(readbackFenceValue_, readbackEvent_);
            WaitForSingleObject(readbackEvent_, INFINITE);
        }

        if (!running_) break;

        // Map readback buffer
        void* pData = nullptr;
        if (SUCCEEDED(readbackBuffer_->Map(0, nullptr, &pData))) {
            memcpy(processingBuffer_.data(), pData, processingBuffer_.size());
            readbackBuffer_->Unmap(0, nullptr);
        }

        // Run depth manager
        std::vector<float> depthOut;
        
        // We pass the raw BGR bytes as float? No!
        // DepthManager::Process expects float RGB!
        // But processingBuffer_ is bytes B8G8R8A8.
        // We need to convert it here or in DepthManager.
        // In this implementation, let's just let DepthManager handle it if we modify it,
        // or we convert it here to float array.
        std::vector<float> floatInput(width_ * height_ * 3);
        uint32_t rowPitch = (width_ * 4 + 255) & ~255;
        for (uint32_t y = 0; y < height_; ++y) {
            uint8_t* row = processingBuffer_.data() + y * rowPitch;
            for (uint32_t x = 0; x < width_; ++x) {
                floatInput[(y * width_ + x) * 3 + 0] = row[x * 4 + 0]; // B
                floatInput[(y * width_ + x) * 3 + 1] = row[x * 4 + 1]; // G
                floatInput[(y * width_ + x) * 3 + 2] = row[x * 4 + 2]; // R
            }
        }

        if (depthManager_.Process(floatInput.data(), width_, height_, depthOut)) {
            std::vector<uint32_t> vis(518 * 518);
            for (size_t i = 0; i < 518 * 518 && i < depthOut.size(); ++i) {
                float d = depthOut[i];
                // Visualization: human-intuitive (near = bright, far = dark)
                // Since d is already 0=near, 1=far, we invert it for visualization only
                uint8_t c = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, 1.0f - d)) * 255.0f);
                // BGRA format: B=c, G=c, R=c, A=255
                vis[i] = 0xFF000000 | (c << 16) | (c << 8) | c;
            }

            std::lock_guard<std::mutex> lock(depthMutex_);
            latestDepthMap_ = std::move(depthOut);
            latestVisPixels_ = std::move(vis);
        }

        newFrameReady_ = false;
        processing_ = false;
    }
}

} // namespace fsrng


