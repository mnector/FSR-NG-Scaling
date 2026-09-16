#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace fsrng {

class DepthManager {
public:
    DepthManager() = default;
    ~DepthManager() = default;

    bool Initialize(const std::wstring& modelPath, const std::wstring& provider);
    bool Process(const float* inputFrame, uint32_t width, uint32_t height, std::vector<float>& depthMap);
    std::string GetVersion() const { return "Depth-Anything-V2-Small"; }

private:
    std::wstring modelPath_;
    std::wstring provider_ = L"CUDA";
    bool initialized_ = false;
    bool hasExternalData_ = false;
};

} // namespace fsrng
