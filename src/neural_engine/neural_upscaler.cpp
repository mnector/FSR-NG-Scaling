#include "neural_upscaler.h"
#include <iostream>

namespace fsrng {

NeuralUpscaler::NeuralUpscaler() = default;
NeuralUpscaler::~NeuralUpscaler() = default;

bool NeuralUpscaler::Initialize(const std::string& hlslPath, const std::string& modelPath, const std::string& expectedSha) {
    if (!engine_.Initialize()) {
        error_ = "Failed to initialize D3D12ComputeEngine: " + engine_.error();
        return false;
    }

    if (!engine_.CompileShader(hlslPath)) {
        error_ = "Failed to compile compute shader: " + engine_.error();
        return false;
    }

    if (!modelPath.empty()) {
        if (modelLoader_.Load(modelPath, expectedSha)) {
            std::cout << "[NeuralUpscaler] Loaded model from: " << modelPath << " ("
                      << modelLoader_.tensors().size() << " neural tensors)" << std::endl;

            // Extract weights and convert F16 to F32 for GPU compute shader
            const uint8_t* rawData = nullptr;
            size_t numFloats = 0;
            auto it = modelLoader_.tensors().find("layer0.weight");
            if (it != modelLoader_.tensors().end()) {
                rawData = modelLoader_.GetData("layer0.weight");
                numFloats = it->second.byteLen / 2; // F16 is 2 bytes
            } else if (!modelLoader_.tensors().empty()) {
                auto first = modelLoader_.tensors().begin();
                rawData = modelLoader_.GetData(first->first);
                numFloats = first->second.byteLen / 2;
            }

            if (rawData && numFloats > 0) {
                // Convert F16 (half) to F32 (float)
                const uint16_t* hf = reinterpret_cast<const uint16_t*>(rawData);
                size_t uploadCount = (std::min)(numFloats, size_t(65536));
                std::vector<float> f32Weights(uploadCount);
                for (size_t i = 0; i < uploadCount; ++i) {
                    uint16_t h = hf[i];
                    uint32_t sign = (h >> 15) & 0x0001;
                    uint32_t exp  = (h >> 10) & 0x001f;
                    uint32_t mant = h & 0x03ff;
                    if (exp == 0) {
                        f32Weights[i] = (sign ? -0.0f : 0.0f);
                    } else if (exp == 31) {
                        f32Weights[i] = (sign ? -1.0f : 1.0f);
                    } else {
                        uint32_t fExp = exp + (127 - 15);
                        uint32_t fMant = mant << 13;
                        uint32_t fBits = (sign << 31) | (fExp << 23) | fMant;
                        float val = 0.0f;
                        memcpy(&val, &fBits, sizeof(float));
                        f32Weights[i] = val;
                    }
                }
                engine_.SetWeights(f32Weights.data(), f32Weights.size() * sizeof(float));
                params.hasWeights = 1.0f;
                std::cout << "[NeuralUpscaler] Uploaded " << uploadCount << " neural parameters to D3D12 Compute Engine." << std::endl;
            }
        } else {
            std::cout << "[NeuralUpscaler] Warning: Could not load model: " << modelLoader_.lastError()
                      << " (falling back to adaptive procedural neural kernel)" << std::endl;
        }
    }

    initialized_ = true;
    return true;
}

bool NeuralUpscaler::Resize(int inW, int inH, int outW, int outH, DXGI_FORMAT format) {
    if (inW <= 0 || inH <= 0 || outW <= 0 || outH <= 0) return false;
    if (inW_ == inW && inH_ == inH && outW_ == outW && outH_ == outH && outputResource_ && format_ == format) {
        return true;
    }

    inW_ = inW;
    inH_ = inH;
    outW_ = outW;
    outH_ = outH;
    format_ = format;

    params.inSize  = float2{ static_cast<float>(inW), static_cast<float>(inH) };
    params.outSize = float2{ static_cast<float>(outW), static_cast<float>(outH) };

    ID3D12Device* device = engine_.device();
    if (!device) return false;

    outputResource_.Reset();

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = outW;
    desc.Height = outH;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(outputResource_.GetAddressOf())
    );

    if (FAILED(hr)) {
        error_ = "Failed to create output UAV texture: " + std::to_string(hr);
        return false;
    }

    return true;
}

void NeuralUpscaler::Process(ID3D12GraphicsCommandList* cmd, ID3D12Resource* inputResource) {
    if (!initialized_ || !cmd || !inputResource || !outputResource_) return;

    params.inSize  = float2{ static_cast<float>(inW_), static_cast<float>(inH_) };
    params.outSize = float2{ static_cast<float>(outW_), static_cast<float>(outH_) };

    engine_.Upscale(cmd, inputResource, outputResource_.Get(), params);
}

} // namespace fsrng
