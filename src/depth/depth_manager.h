#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <memory>

namespace fsrng {

class DepthManager {
public:
    DepthManager() = default;
    ~DepthManager() = default;

    bool Initialize(const std::wstring& modelPath);
    bool Process(const float* inputFrame, uint32_t width, uint32_t height, std::vector<float>& depthMap);
    std::string GetVersion() const { return "Depth-Anything-V2-Small (Placeholder)"; }

private:
    std::wstring modelPath_;
    bool initialized_ = false;
};

} // namespace fsrng
