#include "neural_upscaler.h"
#include <iostream>

namespace fsrng {

NeuralUpscaler::NeuralUpscaler() {
    memset(&params, 0, sizeof(ScaleParams));
}

NeuralUpscaler::~NeuralUpscaler() {
    ShutdownNGX();
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
}

bool NeuralUpscaler::Initialize() {
    if (!engine_.Initialize()) {
        error_ = "Failed to initialize D3D12 compute engine.";
        return false;
    }
    
    // We ignore failure here since it might just be missing or we are not using downscaling
    engine_.CompileShader("shaders/downscale_cs.hlsl");

    hNvngx_ = LoadLibraryA("nvngx.dll");
    if (!hNvngx_) {
        // Fallback to OptiScaler
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
        error_ = "OptiScaler proxy loaded, but missing DLSS NVSDK NGX exports.";
        return false;
    }

    // Initialize NGX with dummy ProjectID
    const wchar_t* dataPath = L".";
    NVSDK_NGX_Result res = pfnInit("FSR-NG", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataPath, engine_.device(), nullptr, (NVSDK_NGX_Version)NVSDK_NGX_VERSION_API_MACRO);
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

bool NeuralUpscaler::CreateDummyTextures(int w, int h) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Motion Vectors (R16G16_FLOAT)
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    if (FAILED(engine_.device()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummyMVs_)))) {
        return false;
    }
    dummyMVs_->SetName(L"Dummy Motion Vectors");

    // Depth (R32_FLOAT)
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(engine_.device()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummyDepth_)))) {
        return false;
    }
    dummyDepth_->SetName(L"Dummy Depth");

    // Albedo / Color Mask (R8G8B8A8_UNORM)
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(engine_.device()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummyAlbedo_)))) {
        return false;
    }
    dummyAlbedo_->SetName(L"Dummy Albedo");

    // Normals (R16G16_FLOAT or R8G8B8A8_UNORM)
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(engine_.device()->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dummyNormal_)))) {
        return false;
    }
    dummyNormal_->SetName(L"Dummy Normal");

    // Dummy textures remain uninitialized. FSR2/Envy-Diamond will use them as zero/garbage.
    return true;
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
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = engine_.device()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&outputResource_)
    );
    if (SUCCEEDED(hr) && outputResource_) {
        outputResource_->SetName(L"NeuralUpscaler Output Resource");
    } else {
        std::cerr << "[NeuralUpscaler] Failed to create Output Resource! HR: " << hr << std::endl;
    }
}

bool NeuralUpscaler::Resize(int inW, int inH, int outW, int outH, DXGI_FORMAT format, float desktopScale) {
    if (inW == inW_ && inH == inH_ && outW == outW_ && outH == outH_ && format == format_ && desktopScale == desktopScale_) return true;
    inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH; format_ = format; desktopScale_ = desktopScale;

    scaledW_ = static_cast<int>(inW_ * desktopScale_);
    scaledH_ = static_cast<int>(inH_ * desktopScale_);
    if (scaledW_ < 16) scaledW_ = 16;
    if (scaledH_ < 16) scaledH_ = 16;

    std::cout << "[NeuralUpscaler] Resizing pipeline. Input: " << inW_ << "x" << inH_ 
              << " -> Scaled Input: " << scaledW_ << "x" << scaledH_ 
              << " -> Output: " << outW_ << "x" << outH_ << std::endl;

    if (ngxFeature_) {
        if (pfnReleaseFeature) pfnReleaseFeature(ngxFeature_);
        ngxFeature_ = nullptr;
    }

    CreateDummyTextures(scaledW_, scaledH_);
    CreateOutputResource(outW, outH, format);

    // Create Downscaled Resource if needed
    if (desktopScale_ < 1.0f) {
        D3D12_RESOURCE_DESC downDesc = {};
        downDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        downDesc.Width = scaledW_;
        downDesc.Height = scaledH_;
        downDesc.DepthOrArraySize = 1;
        downDesc.MipLevels = 1;
        downDesc.SampleDesc.Count = 1;
        downDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        downDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        downDesc.Format = format;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        HRESULT hrDown = engine_.device()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &downDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&downscaledResource_));
        if (SUCCEEDED(hrDown)) downscaledResource_->SetName(L"NeuralUpscaler Downscaled Resource");
    }

    // Create Crop Resource
    D3D12_RESOURCE_DESC cropDesc = {};
    cropDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    cropDesc.Width = inW;
    cropDesc.Height = inH;
    cropDesc.DepthOrArraySize = 1;
    cropDesc.MipLevels = 1;
    cropDesc.SampleDesc.Count = 1;
    cropDesc.Flags = D3D12_RESOURCE_FLAG_NONE; // Can be a regular texture
    cropDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    cropDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hrCrop = engine_.device()->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &cropDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cropResource_));
        
    if (SUCCEEDED(hrCrop) && cropResource_) {
        cropResource_->SetName(L"NeuralUpscaler Crop Resource");
    } else {
        std::cerr << "[NeuralUpscaler] Failed to create Crop Resource! HR: " << hrCrop << std::endl;
    }

    // Create DLSS Feature
    ngxParameters_->Set(NVSDK_NGX_Parameter_Width, scaledW_);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Height, scaledH_);
    ngxParameters_->Set(NVSDK_NGX_Parameter_OutWidth, outW);
    ngxParameters_->Set(NVSDK_NGX_Parameter_OutHeight, outH);
    ngxParameters_->Set(NVSDK_NGX_Parameter_PerfQualityValue, NVSDK_NGX_PerfQuality_Value_MaxQuality);

    // We must create a temporary command list for feature creation
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmd;
    engine_.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    engine_.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));

    TransitionResource(cmd.Get(), dummyMVs_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmd.Get(), dummyDepth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmd.Get(), dummyAlbedo_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmd.Get(), dummyNormal_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    NVSDK_NGX_Result res = pfnCreateFeature(cmd.Get(), NVSDK_NGX_Feature_SuperSampling, ngxParameters_, &ngxFeature_);
    
    cmd->Close();
    ID3D12CommandList* lists[] = { cmd.Get() };
    engine_.directQueue()->ExecuteCommandLists(1, lists);
    
    // Wait for idle
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    engine_.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    engine_.directQueue()->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, event);
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);

    if (NVSDK_NGX_FAILED(res)) {
        error_ = "NVSDK_NGX_D3D12_CreateFeature failed.";
        return false;
    }

    return true;
}

void NeuralUpscaler::TransitionResource(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = res;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &barrier);
}

void NeuralUpscaler::Process(ID3D12GraphicsCommandList* cmd, ID3D12Resource* inputResource, int cropX, int cropY) {
    if (!initialized_ || !ngxFeature_ || !inputResource) return;

    D3D12_RESOURCE_DESC inDesc = inputResource->GetDesc();
    ID3D12Resource* activeInput = inputResource;

    if (inDesc.Width != inW_ || inDesc.Height != inH_) {
        // We need to crop from inputResource to cropResource_
        TransitionResource(cmd, inputResource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(cmd, cropResource_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = cropResource_.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = inputResource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        D3D12_BOX box = {};
        box.left = cropX;
        box.top = cropY;
        box.right = cropX + inW_;
        box.bottom = cropY + inH_;
        box.front = 0;
        box.back = 1;

        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

        TransitionResource(cmd, cropResource_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(cmd, inputResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

        activeInput = cropResource_.Get();
    } else {
        TransitionResource(cmd, activeInput, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    if (desktopScale_ < 1.0f && downscaledResource_ && engine_.initialized()) {
        TransitionResource(cmd, downscaledResource_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        
        ScaleParams csParams = {};
        csParams.inSize = { static_cast<float>(inW_), static_cast<float>(inH_) };
        csParams.outSize = { static_cast<float>(scaledW_), static_cast<float>(scaledH_) };
        
        engine_.Upscale(cmd, activeInput, nullptr, downscaledResource_.Get(), csParams);
        
        TransitionResource(cmd, activeInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        TransitionResource(cmd, downscaledResource_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        
        activeInput = downscaledResource_.Get();
    }

    ngxParameters_->Set(NVSDK_NGX_Parameter_Color, activeInput);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Output, outputResource_.Get());
    ngxParameters_->Set(NVSDK_NGX_Parameter_Depth, dummyDepth_.Get());
    ngxParameters_->Set(NVSDK_NGX_Parameter_MotionVectors, dummyMVs_.Get());
    ngxParameters_->Set("Albedo", dummyAlbedo_.Get());
    ngxParameters_->Set("Roughness", dummyNormal_.Get()); // Reusing normal as roughness to save memory
    ngxParameters_->Set("DLSS.Input.Bias.Current.Color.Mask", dummyAlbedo_.Get());
    ngxParameters_->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    ngxParameters_->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    ngxParameters_->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, scaledW_);
    ngxParameters_->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, scaledH_);
    ngxParameters_->Set(NVSDK_NGX_Parameter_Reset, params.resetHistory > 0.0f ? 1 : 0);
    
    params.resetHistory = 0.0f;

    pfnEvaluateFeature(cmd, ngxFeature_, ngxParameters_, nullptr);

    if (activeInput == cropResource_.Get()) {
        TransitionResource(cmd, activeInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    } else {
        TransitionResource(cmd, activeInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
}

} // namespace fsrng
