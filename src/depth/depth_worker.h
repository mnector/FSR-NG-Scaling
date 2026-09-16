#pragma once
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include "depth_manager.h"

namespace fsrng {

class DepthWorker {
public:
    DepthWorker();
    ~DepthWorker();

    bool Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width, uint32_t height, const std::wstring& modelPath, const std::wstring& provider);
    void Shutdown();

    // Call this every frame to queue a readback if idle
    void Update(ID3D12GraphicsCommandList* cmd, ID3D12Resource* inputResource);
    void PostSubmit(ID3D12CommandQueue* queue);

    // Returns the latest depth map. Size is 518x518.
    std::vector<float> GetLatestDepthMap();
    std::vector<uint32_t> GetLatestVisPixels();
    std::string GetVersion() const { return depthManager_.GetVersion(); }

private:
    void WorkerThread();

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    
    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Fence> readbackFence_;
    uint64_t readbackFenceValue_ = 0;
    HANDLE readbackEvent_ = nullptr;

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    DepthManager depthManager_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> processing_{false};
    std::atomic<bool> newFrameReady_{false};
    std::atomic<bool> pendingSubmit_{false};

    std::vector<float> latestDepthMap_;
    std::vector<uint32_t> latestVisPixels_;
    std::vector<uint8_t> processingBuffer_; // To hold raw BGR data
    std::mutex depthMutex_;
};

} // namespace fsrng

