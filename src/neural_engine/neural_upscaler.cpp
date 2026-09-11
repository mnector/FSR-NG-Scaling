#include "neural_upscaler.h"
#include <iostream>
#include <iomanip>

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
            size_t totalModelTensors = modelLoader_.tensors().size();
            std::cout << "[NeuralUpscaler] Loaded model from: " << modelPath << " ("
                      << totalModelTensors << " neural tensors)" << std::endl;

            // Deep Tensor Mapping: Pack structured layers and deep cascade into unified GPU buffer
            // Header: 64 floats (table of offsets and strides)
            // Indices 0..12: Offsets for each functional layer
            constexpr size_t HEADER_SIZE = 64;
            std::vector<float> packedWeights(HEADER_SIZE, 0.0f);

            auto appendTensor = [&](const std::string& name, size_t maxElements, size_t headerOffsetIndex) {
                std::vector<float> data;
                if (modelLoader_.GetTensorF32(name, data, maxElements) && !data.empty()) {
                    packedWeights[headerOffsetIndex] = static_cast<float>(packedWeights.size());
                    packedWeights.insert(packedWeights.end(), data.begin(), data.end());
                    return true;
                }
                packedWeights[headerOffsetIndex] = 0.0f; // indicates not available
                return false;
            };

            // 1. Primary Transformer & Convolutional Layers
            appendTensor("layer0.conv",   4096, 0);
            appendTensor("layer0.weight", 4096, 1);
            appendTensor("layer1.weight", 4096, 2);
            appendTensor("layer2.qkv",   16384, 3);
            appendTensor("layer2.attn",   8192, 4);
            appendTensor("layer3.attn",   8192, 5);
            appendTensor("layer3.proj",   8192, 6);
            appendTensor("layer4.weight", 4096, 7);
            appendTensor("layer4.attn",   8192, 8);
            appendTensor("layer4.proj",   8192, 9);

            // 2. Deep Cascade Stages (open_nr_layer_010 through open_nr_layer_041)
            size_t cascadeStart = packedWeights.size();
            packedWeights[10] = static_cast<float>(cascadeStart);
            size_t cascadeLayersLoaded = 0;
            constexpr size_t CASCADE_STRIDE = 1024;
            packedWeights[12] = static_cast<float>(CASCADE_STRIDE);

            for (int i = 10; i <= 41; ++i) {
                char layerName[64];
                snprintf(layerName, sizeof(layerName), "open_nr_layer_%03d.weight", i);
                std::vector<float> layerData;
                if (modelLoader_.GetTensorF32(layerName, layerData, CASCADE_STRIDE)) {
                    if (layerData.size() < CASCADE_STRIDE) {
                        layerData.resize(CASCADE_STRIDE, 0.0f);
                    }
                    packedWeights.insert(packedWeights.end(), layerData.begin(), layerData.end());
                    cascadeLayersLoaded++;
                }
            }
            packedWeights[11] = static_cast<float>(cascadeLayersLoaded);

            engine_.SetWeights(packedWeights.data(), packedWeights.size() * sizeof(float));
            params.hasWeights = 1.0f;

            std::cout << "[NeuralUpscaler] Deep Tensor Mapping ready: "
                      << packedWeights.size() << " parameters uploaded ("
                      << cascadeLayersLoaded << " cascaded residual layers mapped)." << std::endl;
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
    if (inW_ == inW && inH_ == inH && outW_ == outW && outH_ == outH && outputResource_ && historyResource_ && format_ == format) {
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
    historyResource_.Reset();

    // 1. Create Output UAV Resource
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
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(outputResource_.GetAddressOf())
    );

    if (FAILED(hr)) {
        error_ = "Failed to create output UAV texture: " + std::to_string(hr);
        return false;
    }

    // 2. Create History Resource for Temporal Accumulation ($S_{t-1}$)
    D3D12_RESOURCE_DESC histDesc = desc;
    histDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &histDesc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(historyResource_.GetAddressOf())
    );

    if (FAILED(hr)) {
        error_ = "Failed to create history texture: " + std::to_string(hr);
        return false;
    }

    // First frame must reset history
    params.resetHistory = 1.0f;
    return true;
}

void NeuralUpscaler::Process(ID3D12GraphicsCommandList* cmd, ID3D12Resource* inputResource) {
    if (!initialized_ || !cmd || !inputResource || !outputResource_) return;

    params.inSize  = float2{ static_cast<float>(inW_), static_cast<float>(inH_) };
    params.outSize = float2{ static_cast<float>(outW_), static_cast<float>(outH_) };

    // 1. Transition output texture from COMMON to UNORDERED_ACCESS
    D3D12_RESOURCE_BARRIER preBarrier{};
    preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preBarrier.Transition.pResource = outputResource_.Get();
    preBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    preBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmd->ResourceBarrier(1, &preBarrier);

    // 2. Execute Compute Shader Pass with history input
    engine_.Upscale(cmd, inputResource, historyResource_.Get(), outputResource_.Get(), params);

    // Reset single-frame history reset flag
    if (params.resetHistory > 0.5f) {
        params.resetHistory = 0.0f;
    }

    // 3. Copy output to history buffer for next frame and transition to COMMON for presentation
    if (historyResource_) {
        D3D12_RESOURCE_BARRIER copyBarriers[2]{};
        // Output: UNORDERED_ACCESS -> COPY_SOURCE
        copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[0].Transition.pResource = outputResource_.Get();
        copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        copyBarriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;

        // History: NON_PIXEL_SHADER_RESOURCE -> COPY_DEST
        copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[1].Transition.pResource = historyResource_.Get();
        copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        copyBarriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;

        cmd->ResourceBarrier(2, copyBarriers);

        cmd->CopyResource(historyResource_.Get(), outputResource_.Get());

        D3D12_RESOURCE_BARRIER postBarriers[2]{};
        // History: COPY_DEST -> NON_PIXEL_SHADER_RESOURCE
        postBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarriers[0].Transition.pResource = historyResource_.Get();
        postBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        postBarriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // Output: COPY_SOURCE -> COMMON
        postBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarriers[1].Transition.pResource = outputResource_.Get();
        postBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        postBarriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;

        cmd->ResourceBarrier(2, postBarriers);
    } else {
        D3D12_RESOURCE_BARRIER postBarrier{};
        postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        postBarrier.Transition.pResource = outputResource_.Get();
        postBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        postBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        cmd->ResourceBarrier(1, &postBarrier);
    }
}

} // namespace fsrng
