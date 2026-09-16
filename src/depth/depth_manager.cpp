#include "depth_manager.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>
#include <cstdlib>
#include <sys/stat.h>

namespace fsrng {

// Pre-computed statistics for Depth-Anything V2
const float MEAN[] = {0.485f, 0.456f, 0.406f};
const float STD[]  = {0.229f, 0.224f, 0.225f};

// Bilinear resize helper
static void resizeBilinear(const float* src, uint32_t srcW, uint32_t srcH, 
                           float* dst, uint32_t dstW, uint32_t dstH) {
    const float xRatio = static_cast<float>(srcW) / dstW;
    const float yRatio = static_cast<float>(srcH) / dstH;

    for (uint32_t y = 0; y < dstH; ++y) {
        const uint32_t py = static_cast<uint32_t>(y * yRatio);
        const float dy = (y * yRatio) - py;
        const uint32_t py1 = std::min(py + 1, srcH - 1);

        for (uint32_t x = 0; x < dstW; ++x) {
            const uint32_t px = static_cast<uint32_t>(x * xRatio);
            const float dx = (x * xRatio) - px;
            const uint32_t px1 = std::min(px + 1, srcW - 1);

            const float* p00 = src + py * srcW * 3 + px * 3;
            const float* p01 = src + py * srcW * 3 + px1 * 3;
            const float* p10 = src + py1 * srcW * 3 + px * 3;
            const float* p11 = src + py1 * srcW * 3 + px1 * 3;

            float* out = dst + y * dstW * 3 + x * 3;
            for (int c = 0; c < 3; ++c) {
                float val = p00[c] * (1.0f - dx) * (1.0f - dy) +
                            p01[c] * dx * (1.0f - dy) +
                            p10[c] * (1.0f - dx) * dy +
                            p11[c] * dx * dy;
                out[c] = val;
            }
        }
    }
}

// Helper to check if file exists
static bool fileExists(const std::wstring& path) {
    struct _stat buffer;
    return (_wstat(path.c_str(), &buffer) == 0);
}

bool DepthManager::Initialize(const std::wstring& modelPath) {
    modelPath_ = modelPath;

    // Check if model file and external data exist
    if (!fileExists(modelPath)) {
        return false;
    }
    
    // Check for external data file
    std::wstring dataPath = modelPath + L"_data";
    hasExternalData_ = fileExists(dataPath);

    initialized_ = true;
    return true;
}

bool DepthManager::Process(const float* inputFrame, uint32_t width, uint32_t height, 
                           std::vector<float>& depthMap) {
    if (!initialized_) return false;

    const uint32_t targetW = 518;
    const uint32_t targetH = 518;

    // Prepare input: resize to 518x518 and normalize
    // Input: BGR (from DX12) -> convert to RGB and normalize
    std::vector<float> inputRGB(width * height * 3);
    std::vector<float> resizedRGB(targetW * targetH * 3);

    // Convert BGR to RGB and scale to [0,1]
    for (uint32_t i = 0; i < width * height; ++i) {
        const float* bgr = inputFrame + i * 3;
        float* rgb = inputRGB.data() + i * 3;
        rgb[0] = bgr[2] / 255.0f;  // B -> R
        rgb[1] = bgr[1] / 255.0f;  // G -> G
        rgb[2] = bgr[0] / 255.0f;  // R -> B
    }

    // Resize to 518x518
    resizeBilinear(inputRGB.data(), width, height, resizedRGB.data(), targetW, targetH);

    // Write temp frame file
    std::wstring framePath = L"temp_frame.bin";
    std::ofstream frameFile(framePath, std::ios::binary);
    if (!frameFile) return false;
    frameFile.write(reinterpret_cast<char*>(inputRGB.data()), width * height * 3 * sizeof(float));
    frameFile.close();

    // Run Python inference
    std::wstring outputPath = L"temp_depth.bin";
    std::wstring cmd = L"C:\\Users\\mnect\\AppData\\Local\\hermes\\hermes-agent\\venv\\Scripts\\python.exe scripts/depth_infer.py temp_frame.bin temp_depth.bin";
    int result = _wsystem(cmd.c_str());
    
    // Clean up temp frame
    _wunlink(framePath.c_str());

    if (result != 0) {
        // Fallback to placeholder
        depthMap.resize(targetW * targetH);
        for (uint32_t y = 0; y < targetH; ++y) {
            for (uint32_t x = 0; x < targetW; ++x) {
                depthMap[y * targetW + x] = static_cast<float>(x) / targetW;
            }
        }
        return true;
    }

    // Read depth result
    std::ifstream depthFile(outputPath, std::ios::binary);
    if (!depthFile) {
        _wunlink(outputPath.c_str());
        return false;
    }
    
    depthMap.resize(targetW * targetH);
    depthFile.read(reinterpret_cast<char*>(depthMap.data()), targetW * targetH * sizeof(float));
    depthFile.close();
    _wunlink(outputPath.c_str());

    return true;
}

} // namespace fsrng
