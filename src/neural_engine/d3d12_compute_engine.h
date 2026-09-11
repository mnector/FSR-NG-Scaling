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

// Parameters uploaded to the compute shader each frame.
// 16-byte aligned matching cbuffer Params in neural_scale_cs.hlsl.
struct alignas(16) ScaleParams {
    float2 inSize;              // offset 0 (8 bytes)
    float2 outSize;             // offset 8 (8 bytes)
    float  intensity = 0.75f;   // offset 16 (4 bytes)
    float  structureIntensity = 0.40f; // offset 20 (4 bytes)
    float  toneIntensity = 0.10f;      // offset 24 (4 bytes)
    float  splitScreen = 0.0f;         // offset 28 (4 bytes)
    float  hasWeights = 0.0f;          // offset 32 (4 bytes)
    float  pad1 = 0.0f;                // offset 36 (4 bytes)
    float2 pad2{};              // offset 40 (8 bytes) -> 48 bytes total (multiple of 16)
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

    // Prepare descriptor heap for input SRV (t0), weights SRV (t1), and output UAV (u0)
    bool BindDescriptors(ID3D12Resource* input, ID3D12Resource* output);

    // Dispatch compute shader
    void Upscale(ID3D12GraphicsCommandList* cmd,
                 ID3D12Resource* input,
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
