#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>

namespace fsrng {

struct float2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct float4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// Parameters uploaded to the compute shader each frame.
// 16-byte aligned matching cbuffer Params in neural_scale_cs.hlsl (112 bytes total).
struct alignas(16) ScaleParams {
    float2 inSize;                      // offset 0 (8 bytes)
    float2 outSize;                     // offset 8 (8 bytes)
    float  intensity = 1.00f;           // offset 16 (4 bytes) - NR Intensity
    float  structureIntensity = 1.00f;  // offset 20 (4 bytes) - Local Structure Strength
    float  toneIntensity = 1.00f;       // offset 24 (4 bytes) - Local Tone Strength
    float  splitScreen = 0.0f;          // offset 28 (4 bytes)
    float  hasWeights = 0.0f;           // offset 32 (4 bytes)
    float  temporalStability = 0.0f;    // offset 36 (4 bytes) - default 0.0f to eliminate ghosting
    float  resetHistory = 0.0f;         // offset 40 (4 bytes)
    float  modeWindow = 0.0f;           // offset 44 (4 bytes)
    float4 captureCrop{ 0.0f, 0.0f, 1.0f, 1.0f }; // offset 48 (16 bytes)
    float  detailBoost = 1.35f;         // offset 64 (4 bytes)
    float  catmullRom = 1.0f;           // offset 68 (4 bytes)
    float  skinStructureStrength = 0.0f;// offset 72 (4 bytes) - Skin Structure Strength (-1.0 to 1.0)
    float  nrPasses = 1.0f;             // offset 76 (4 bytes) - NR Passes (1 to 4)
    float  scenePaperWhite = 1.0f;      // offset 80 (4 bytes) - Scene Paper-White Scale
    float  hdrTransferStrength = 1.0f;  // offset 84 (4 bytes) - HDR Transfer Strength
    float  colorStrength = 1.0f;        // offset 88 (4 bytes) - Color Strength
    float  enableNR = 1.0f;             // offset 92 (4 bytes) - Enable DLSS Neural Rendering
    float  nrStyle = 0.0f;              // offset 96 (4 bytes) - NR Style (0: Default, 1: Cinematic, 2: Aggressive)
    float  autoMask = 1.0f;             // offset 100 (4 bytes) - Automatic Mask / Zero-Ghosting Motion Gate
    float2 pad{};                       // offset 104 (8 bytes) -> 112 bytes total (16-byte aligned)
};

class D3D12ComputeEngine {
public:
    D3D12ComputeEngine();
    ~D3D12ComputeEngine();

    // Initialize isolated DX12 device and queues
    bool Initialize(int adapterIndex = 0);

    // Compile compute shader and create Compute PSO + Root Signature
    bool CompileShader(const std::string& hlslPath);

    // Upload neural model weights to GPU structured buffer
    bool SetWeights(const void* data, size_t byteSize);

    // Prepare descriptor heap for input SRV (t0), weights SRV (t1), history SRV (t2), and output UAV (u0)
    bool BindDescriptors(ID3D12Resource* input, ID3D12Resource* history, ID3D12Resource* output);

    // Dispatch compute shader
    void Upscale(ID3D12GraphicsCommandList* cmd,
                 ID3D12Resource* input,
                 ID3D12Resource* history,
                 ID3D12Resource* output,
                 const ScaleParams& params);

    bool initialized() const { return initialized_; }
    const std::string& error() const { return error_; }

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    ID3D12CommandQueue* directQueue() const { return directQueue_.Get(); }
    ID3D12RootSignature* rootSig() const { return rootSig_.Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> directQueue_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> weightsBuffer_;

    UINT descriptorSize_ = 0;
    bool initialized_ = false;
    bool hasWeights_ = false;
    std::string error_;
};

} // namespace fsrng
