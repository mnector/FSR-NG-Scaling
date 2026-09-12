#include "d3d12_compute_engine.h"
#include <fstream>
#include <sstream>
#include <cmath>
#include <iostream>

namespace fsrng {

D3D12ComputeEngine::D3D12ComputeEngine() = default;
D3D12ComputeEngine::~D3D12ComputeEngine() = default;

bool D3D12ComputeEngine::Initialize(int adapterIndex) {
    HRESULT hr = S_OK;

    // Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    // if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
    //     debugController->EnableDebugLayer();
    // }

    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "CreateDXGIFactory1 failed: " + std::to_string(hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (static_cast<int>(i) == adapterIndex || !selectedAdapter) {
            selectedAdapter = adapter;
            if (static_cast<int>(i) == adapterIndex) break;
        }
    }

    if (!selectedAdapter) {
        error_ = "No hardware DirectX 12 adapter found";
        return false;
    }

    hr = D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device_.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "D3D12CreateDevice failed: " + std::to_string(hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type     = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    qdesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = device_->CreateCommandQueue(&qdesc, IID_PPV_ARGS(queue_.GetAddressOf()));
    if (FAILED(hr)) {
        qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device_->CreateCommandQueue(&qdesc, IID_PPV_ARGS(queue_.GetAddressOf()));
        if (FAILED(hr)) {
            error_ = "CreateCommandQueue (Compute) failed: " + std::to_string(hr);
            return false;
        }
    }

    // Direct command queue for presentation / copy
    D3D12_COMMAND_QUEUE_DESC directDesc{};
    directDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    directDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    directDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = device_->CreateCommandQueue(&directDesc, IID_PPV_ARGS(directQueue_.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "CreateCommandQueue (Direct) failed: " + std::to_string(hr);
        return false;
    }

    // Descriptor heap for:
    // Slot 0: Input SRV (t0)
    // Slot 1: Weights SRV (t1)
    // Slot 2: History SRV (t2)
    // Slot 3: Output UAV (u0)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 4;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap_.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "CreateDescriptorHeap failed: " + std::to_string(hr);
        return false;
    }

    descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Create default fallback weights buffer
    std::vector<float> defaultWeights(4096);
    for (size_t i = 0; i < defaultWeights.size(); i += 4) {
        defaultWeights[i + 0] = 0.62f;
        defaultWeights[i + 1] = 0.28f;
        defaultWeights[i + 2] = 0.14f;
        defaultWeights[i + 3] = 0.45f;
    }
    SetWeights(defaultWeights.data(), defaultWeights.size() * sizeof(float));

    initialized_ = true;
    return true;
}

bool D3D12ComputeEngine::SetWeights(const void* data, size_t byteSize) {
    if (!device_ || !data || byteSize == 0) return false;

    weightsBuffer_.Reset();

    D3D12_HEAP_PROPERTIES hpUpload{};
    hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC descUpload{};
    descUpload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    descUpload.Width = byteSize;
    descUpload.Height = 1;
    descUpload.DepthOrArraySize = 1;
    descUpload.MipLevels = 1;
    descUpload.Format = DXGI_FORMAT_UNKNOWN;
    descUpload.SampleDesc.Count = 1;
    descUpload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device_->CreateCommittedResource(
        &hpUpload,
        D3D12_HEAP_FLAG_NONE,
        &descUpload,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(weightsBuffer_.GetAddressOf())
    );

    if (FAILED(hr)) {
        error_ = "Failed to create weights buffer: " + std::to_string(hr);
        return false;
    }

    void* mapped = nullptr;
    weightsBuffer_->Map(0, nullptr, &mapped);
    if (mapped) {
        memcpy(mapped, data, byteSize);
        weightsBuffer_->Unmap(0, nullptr);
    }

    hasWeights_ = true;
    return true;
}

bool D3D12ComputeEngine::CompileShader(const std::string& hlslPath) {
    if (!device_) {
        error_ = "Device not initialized";
        return false;
    }

    std::ifstream f(hlslPath, std::ios::binary);
    if (!f) {
        error_ = "Cannot open shader file: " + hlslPath;
        return false;
    }
    std::string hlslSource((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(
        hlslSource.data(),
        hlslSource.size(),
        hlslPath.c_str(),
        nullptr,
        nullptr,
        "CSMain",
        "cs_5_1",
        compileFlags,
        0,
        shaderBlob.GetAddressOf(),
        errorBlob.GetAddressOf()
    );

    if (FAILED(hr)) {
        if (errorBlob) {
            error_ = reinterpret_cast<const char*>(errorBlob->GetBufferPointer());
        } else {
            error_ = "D3DCompile failed with code " + std::to_string(hr);
        }
        return false;
    }

    // Root Signature Layout:
    // Param 0: Descriptor Table with SRV range (t0, t1, t2, count 3)
    // Param 1: Descriptor Table with UAV range (u0, count 1)
    // Param 2: 32-bit Constants (b0, sizeof(ScaleParams)/4)
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3; // t0 (Input Texture) + t1 (Weights Buffer) + t2 (History Texture)
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1; // u0 (Output Texture)
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[3]{};

    // [0] SRV Table (t0, t1, t2)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[0].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] UAV Table (u0)
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] 32-bit Constants
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[2].Constants.ShaderRegister = 0;
    rootParams[2].Constants.RegisterSpace = 0;
    rootParams[2].Constants.Num32BitValues = sizeof(ScaleParams) / 4;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 0;
    rsDesc.pStaticSamplers = nullptr;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> rsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> rsErrorBlob;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, rsBlob.GetAddressOf(), rsErrorBlob.GetAddressOf());
    if (FAILED(hr)) {
        error_ = "D3D12SerializeRootSignature failed";
        return false;
    }

    hr = device_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(rootSig_.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "CreateRootSignature failed";
        return false;
    }

    // Compute Pipeline State Object
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig_.Get();
    psoDesc.CS.pShaderBytecode = shaderBlob->GetBufferPointer();
    psoDesc.CS.BytecodeLength = shaderBlob->GetBufferSize();
    psoDesc.NodeMask = 0;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(pso_.GetAddressOf()));
    if (FAILED(hr)) {
        error_ = "CreateComputePipelineState failed: " + std::to_string(hr);
        return false;
    }

    return true;
}

bool D3D12ComputeEngine::BindDescriptors(ID3D12Resource* input, ID3D12Resource* history, ID3D12Resource* output) {
    if (!device_ || !heap_ || !input || !output) return false;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = heap_->GetCPUDescriptorHandleForHeapStart();

    // Slot 0: SRV for input frame (t0)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    device_->CreateShaderResourceView(input, &srvDesc, cpuHandle);

    // Slot 1: SRV for neural weights buffer (t1)
    cpuHandle.ptr += descriptorSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC weightsSrvDesc{};
    weightsSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    weightsSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    weightsSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (weightsBuffer_) {
        D3D12_RESOURCE_DESC wbDesc = weightsBuffer_->GetDesc();
        weightsSrvDesc.Buffer.FirstElement = 0;
        weightsSrvDesc.Buffer.NumElements = static_cast<UINT>(wbDesc.Width / sizeof(float));
        weightsSrvDesc.Buffer.StructureByteStride = sizeof(float);
        weightsSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        device_->CreateShaderResourceView(weightsBuffer_.Get(), &weightsSrvDesc, cpuHandle);
    } else {
        device_->CreateShaderResourceView(nullptr, &weightsSrvDesc, cpuHandle);
    }

    // Slot 2: SRV for previous frame history (t2)
    cpuHandle.ptr += descriptorSize_;
    D3D12_SHADER_RESOURCE_VIEW_DESC histSrvDesc{};
    histSrvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    histSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    histSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    histSrvDesc.Texture2D.MostDetailedMip = 0;
    histSrvDesc.Texture2D.MipLevels = 1;
    histSrvDesc.Texture2D.PlaneSlice = 0;
    histSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    device_->CreateShaderResourceView(history ? history : input, &histSrvDesc, cpuHandle);

    // Slot 3: UAV for output texture (u0)
    cpuHandle.ptr += descriptorSize_;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    uavDesc.Texture2D.PlaneSlice = 0;
    device_->CreateUnorderedAccessView(output, nullptr, &uavDesc, cpuHandle);

    return true;
}

void D3D12ComputeEngine::Upscale(ID3D12GraphicsCommandList* cmd,
                                 ID3D12Resource* input,
                                 ID3D12Resource* history,
                                 ID3D12Resource* output,
                                 const ScaleParams& params) {
    if (!initialized_ || !cmd || !input || !output) return;

    if (!BindDescriptors(input, history, output)) return;

    ID3D12DescriptorHeap* heaps[] = { heap_.Get() };
    cmd->SetDescriptorHeaps(1, heaps);

    cmd->SetComputeRootSignature(rootSig_.Get());
    cmd->SetPipelineState(pso_.Get());

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = heap_->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootDescriptorTable(0, gpuHandle); // t0, t1, t2

    gpuHandle.ptr += (descriptorSize_ * 3);
    cmd->SetComputeRootDescriptorTable(1, gpuHandle); // u0

    ScaleParams p = params;
    p.hasWeights = hasWeights_ ? 1.0f : 0.0f;
    cmd->SetComputeRoot32BitConstants(2, sizeof(ScaleParams) / 4, &p, 0);

    UINT dispatchX = (static_cast<UINT>(p.outSize.x) + 7) / 8;
    UINT dispatchY = (static_cast<UINT>(p.outSize.y) + 7) / 8;
    cmd->Dispatch(dispatchX, dispatchY, 1);
}

} // namespace fsrng
