#pragma once
#include "d3d12_compute_engine.h"
#include "safetensors_loader.h"
#include <string>
#include <vector>
#include <memory>

namespace fsrng {

class NeuralUpscaler {
public:
    NeuralUpscaler();
    ~NeuralUpscaler();

    // Initialize D3D12 compute engine and optionally load SafeTensors model weights
    bool Initialize(const std::string& hlslPath, const std::string& modelPath = "", const std::string& expectedSha = "");

    // Resize or allocate the output UAV texture and internal staging resources
    bool Resize(int inW, int inH, int outW, int outH, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM);

    // Run upscale compute shader on the provided command list
    void Process(ID3D12GraphicsCommandList* cmd, ID3D12Resource* inputResource);

    ID3D12Resource* output() const { return outputResource_.Get(); }
    int outWidth() const { return outW_; }
    int outHeight() const { return outH_; }

    D3D12ComputeEngine& engine() { return engine_; }
    const D3D12ComputeEngine& engine() const { return engine_; }

    bool initialized() const { return initialized_; }
    const std::string& error() const { return error_; }

    ScaleParams params;

private:
    D3D12ComputeEngine engine_;
    SafetensorsLoader  modelLoader_;
    Microsoft::WRL::ComPtr<ID3D12Resource> outputResource_;

    int inW_ = 0;
    int inH_ = 0;
    int outW_ = 0;
    int outH_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    bool initialized_ = false;
    std::string error_;
};

} // namespace fsrng
