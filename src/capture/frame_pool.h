#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <mutex>

namespace fsrng {

struct CapturedFrame {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint64_t frameIndex = 0;
    uint64_t timestamp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class FramePool {
public:
    FramePool() = default;
    ~FramePool() = default;

    void Push(const CapturedFrame& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = frame;
        hasFrame_ = true;
    }

    bool Pop(CapturedFrame& outFrame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasFrame_) return false;
        outFrame = latest_;
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.resource.Reset();
        hasFrame_ = false;
    }

private:
    std::mutex mutex_;
    CapturedFrame latest_{};
    bool hasFrame_ = false;
};

} // namespace fsrng
