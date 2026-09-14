#include "neural_upscaler.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <windows.h>

namespace fsrng {

NeuralUpscaler::NeuralUpscaler() {
}

NeuralUpscaler::~NeuralUpscaler() {
    ShutdownNGX();
}

bool NeuralUpscaler::Initialize() {
    // 1. Initialize D3D12 Device and Command Queue natively
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        error_ = "CreateDXGIFactory1 failed";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> selectedAdapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        selectedAdapter = adapter;
        break; 
    }

    if (!selectedAdapter || FAILED(D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        error_ = "No hardware DirectX 12 adapter found or D3D12CreateDevice failed";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC directDesc{};
    directDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&directDesc, IID_PPV_ARGS(&directQueue_)))) {
        error_ = "CreateCommandQueue failed";
        return false;
    }

    // 2. Load NGX/OptiScaler
    hNvngx_ = LoadLibraryA("nvngx.dll");
    if (!hNvngx_) {
        hNvngx_ = LoadLibraryA("dxgi.dll");
        if (!hNvngx_) {
            error_ = "Failed to load nvngx.dll or dxgi.dll proxy for OptiScaler.";
            return false;
        }
    }

    pfnInit = (PFN_NVSDK_NGX_D3D12_Init_with_ProjectID)GetProcAddress(hNvngx_, "NVSDK_NGX_D3D12_Init_with_ProjectID");
    pfnShutdown = (PFN_NVSDK_NGX_D3D12_Shutdown)GetProcAddress(hNvngx_, "NVSDK_NGX_D3D12_Shutdown");
    pfnGetCapParams = (PFN_NVSDK_NGX_D3D12_GetCapabilityParameters)GetProcAddress(hNvngx_, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    pfnAllocParams = (PFN_NVSDK_NGX_D3D12_AllocateParameters)GetProcAddress(hNvngx_, "NVSDK_NGX_D3D12_AllocateParameters");
    pfnDestroyParams = (PFN_NVSDK_NGX_D3D12_DestroyParameters)GetProcAddress(hNvngx_, "NVSDK_NGX_D3D12_DestroyParameters");
    pfnCreateFeature = (PFN_NVSDK_NGX_D3D12_CreateFeature)GetProcAddress(hNvngx_, "NVSDK_NGX_D3D12_CreateFeature");
    pfnReleaseFeature = (PFN_NVSDK_NGX_D3D12_ReleaseFeature)GetProcAddress(hNvngx_, "NVSDK_NGX_D3D12_ReleaseFeature");
    pfnEvaluateFeature = (PFN_NVSDK_NGX_D3D12_EvaluateFeature)GetProcAddress(hNvngx_, "NVSDK_NGX_D3D12_EvaluateFeature");

    if (!pfnInit || !pfnAllocParams || !pfnCreateFeature || !pfnEvaluateFeature) {
        error_ = "Failed to get NGX function pointers.";
        return false;
    }

    std::wstring dataPath = std::filesystem::current_path().wstring();
    NVSDK_NGX_Result res = pfnInit("FSR-NG", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataPath.c_str(), device_.Get(), nullptr, (NVSDK_NGX_Version)NVSDK_NGX_VERSION_API_MACRO);
    if (NVSDK_NGX_FAILED(res)) {
        error_ = "NVSDK_NGX_D3D12_Init failed.";
        return false;
    }

    res = pfnAllocParams(&ngxParameters_);
    if (NVSDK_NGX_FAILED(res)) {
        error_ = "NVSDK_NGX_D3D12_AllocateParameters failed.";
        return false;
    }

    initialized_ = true;
    return true;
}

void NeuralUpscaler::ShutdownNGX() {
    if (ngxFeature_ && pfnReleaseFeature) {
        pfnReleaseFeature(ngxFeature_);
        ngxFeature_ = nullptr;
    }
    if (ngxParameters_ && pfnDestroyParams) {
        pfnDestroyParams(ngxParameters_);
        ngxParameters_ = nullptr;
    }
    if (pfnShutdown) {
        pfnShutdown();
    }
    if (hNvngx_) {
        FreeLibrary(hNvngx_);
        hNvngx_ = nullptr;
    }
    ngxInitialized_ = false;
}

bool NeuralUpscaler::Resize(int inW, int inH, int outW, int outH, DXGI_FORMAT format, float desktopScale) {
    if (!initialized_) return false;

    if (inW == inW_ && inH == inH_ && outW == outW_ && outH == outH_ && format == format_) {
        return true;
    }

    inW_ = inW;
    inH_ = inH;
    outW_ = outW;
    outH_ = outH;
    format_ = format;

    scaledW_ = static_cast<int>(inW * desktopScale);
    scaledH_ = static_cast<int>(inH * desktopScale);

    std::cout << "[NeuralUpscaler] Resizing pipeline. Input: " << inW << "x" << inH 
              << " -> Scaled Input: " << scaledW_ << "x" << scaledH_ 
              << " -> Output: " << outW << "x" << outH << std::endl;

    if (ngxFeature_) {
        pfnReleaseFeature(ngxFeature_);
        ngxFeature_ = nullptr;
    }

    
    CreateOutputResource(outW, outH, format);
    
    
    if (!CreateDummyTextures(scaledW_, scaledH_)) {
        error_ = "Failed to create dummy textures for DLSS.";
        return false;
    }

    
    ngxParameters_->Set(NVSDK_NGX_Parameter_Width, scaledW_);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Height, scaledH_);
    ngxParameters_->Set(NVSDK_NGX_Parameter_OutWidth, outW);
    ngxParameters_->Set(NVSDK_NGX_Parameter_OutHeight, outH);
    ngxParameters_->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, scaledW_);
    ngxParameters_->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, scaledH_);
    ngxParameters_->Set(NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_MaxQuality);
    
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd;
    device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));

    
    NVSDK_NGX_Result res = pfnCreateFeature(cmd.Get(), NVSDK_NGX_Feature_SuperSampling, ngxParameters_, &ngxFeature_);
    
    
    cmd->Close();
    ID3D12CommandList* lists[] = { cmd.Get() };
    
    directQueue_->ExecuteCommandLists(1, lists);
    
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    directQueue_->Signal(fence.Get(), 1);
    
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    
    WaitForSingleObject(event, INFINITE);
    
    CloseHandle(event);

    if (NVSDK_NGX_FAILED(res)) {
        error_ = "Failed to create NGX feature.";
        std::cout << "[NGX] CreateFeature failed with code: " << std::hex << res << std::endl;
        return false;
    }

    ngxInitialized_ = true;
    return true;
}

void NeuralUpscaler::Process(ID3D12GraphicsCommandList* cmd, ID3D12Resource* inputResource, int cropX, int cropY) {
    if (!initialized_) return;

    ID3D12Resource* activeInput = inputResource;
    TransitionResource(cmd, activeInput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (!ngxInitialized_ || !ngxFeature_) {
        TransitionResource(cmd, activeInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        return;
    }

    TransitionResource(cmd, outputResource_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ngxParameters_->Set(NVSDK_NGX_Parameter_Color, activeInput);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Output, outputResource_.Get());
    ngxParameters_->Set(NVSDK_NGX_Parameter_Depth, dummyDepth_.Get());
    ngxParameters_->Set(NVSDK_NGX_Parameter_MotionVectors, dummyMVs_.Get());
    ngxParameters_->Set("Albedo", dummyAlbedo_.Get());
    ngxParameters_->Set("Roughness", dummyNormal_.Get()); 
    ngxParameters_->Set("DLSS.Input.Bias.Current.Color.Mask", dummyAlbedo_.Get());
    ngxParameters_->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    ngxParameters_->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    ngxParameters_->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Reset, params.resetHistory > 0.0f ? 1 : 0);
    params.resetHistory = 0.0f;

    NVSDK_NGX_Result res = pfnEvaluateFeature(cmd, ngxFeature_, ngxParameters_, nullptr);
    if (NVSDK_NGX_FAILED(res)) {
        // Output failure but don't crash
    }

    TransitionResource(cmd, activeInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    TransitionResource(cmd, outputResource_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
}

void NeuralUpscaler::CreateOutputResource(int w, int h, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&outputResource_));
    outputResource_->SetName(L"NeuralUpscaler_Output");
}

bool NeuralUpscaler::CreateDummyTextures(int w, int h) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    if (FAILED(device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummyMVs_)))) return false;
    dummyMVs_->SetName(L"Dummy Motion Vectors");

    desc.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummyDepth_)))) return false;

    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummyAlbedo_)))) return false;
    
    if (FAILED(device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummyNormal_)))) return false;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd;
    device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));

    TransitionResource(cmd.Get(), dummyMVs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmd.Get(), dummyDepth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmd.Get(), dummyAlbedo_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmd.Get(), dummyNormal_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    
    
    cmd->Close();
    ID3D12CommandList* lists[] = { cmd.Get() };
    
    directQueue_->ExecuteCommandLists(1, lists);
    
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    directQueue_->Signal(fence.Get(), 1);
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    
    WaitForSingleObject(event, INFINITE);
    
    CloseHandle(event);

    return true;
}

void NeuralUpscaler::TransitionResource(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!res) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
}

} // namespace fsrng
