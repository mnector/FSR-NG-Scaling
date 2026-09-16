#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>
#include <windows.h>
#include "ngx/nvsdk_ngx.h"
#include "ngx/nvsdk_ngx_defs.h"
#include "ngx/nvsdk_ngx_params.h"

namespace fsrng {

class NeuralUpscaler {
public:
    NeuralUpscaler();
    ~NeuralUpscaler();

    bool Initialize();
    bool Resize(int inW, int inH, int outW, int outH, DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM, float desktopScale = 1.0f);
    void Process(ID3D12GraphicsCommandList* cmd, ID3D12Resource* inputResource, int cropX = 0, int cropY = 0, const float* depthMap = nullptr);
    void ResetHistory() { params.resetHistory = 1.0f; }

    ID3D12Resource* output() const { return outputResource_.Get(); }
    int inWidth() const { return inW_; }
    int inHeight() const { return inH_; }
    int outWidth() const { return outW_; }
    int outHeight() const { return outH_; }

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* directQueue() const { return directQueue_.Get(); }
    bool initialized() const { return initialized_; }
    const std::string& error() const { return error_; }

    struct UpscalerParams {
        
        float splitScreen = 0.0f;
        float resetHistory = 0.0f;
        
    } params;

private:
    void ShutdownNGX();
    bool CreateDummyTextures(int w, int h);
    void CreateOutputResource(int w, int h, DXGI_FORMAT format);
    void TransitionResource(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> directQueue_;
    Microsoft::WRL::ComPtr<ID3D12Resource> outputResource_;
    Microsoft::WRL::ComPtr<ID3D12Resource> cropResource_;
    Microsoft::WRL::ComPtr<ID3D12Resource> downscaledResource_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dummyDepth_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dummyMVs_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dummyAlbedo_;
    Microsoft::WRL::ComPtr<ID3D12Resource> dummyNormal_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthUploadBuffer_;

    HMODULE hNvngx_ = nullptr;
    NVSDK_NGX_Parameter* ngxParameters_ = nullptr;
    NVSDK_NGX_Handle* ngxFeature_ = nullptr;
    bool ngxInitialized_ = false;

    // NGX Function Pointers
    typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_D3D12_Init_with_ProjectID)(const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
    typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_D3D12_Shutdown)(void);
    typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_D3D12_GetCapabilityParameters)(NVSDK_NGX_Parameter**);
    typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_D3D12_AllocateParameters)(NVSDK_NGX_Parameter**);
    typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_D3D12_DestroyParameters)(NVSDK_NGX_Parameter*);
    typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_D3D12_CreateFeature)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_D3D12_ReleaseFeature)(NVSDK_NGX_Handle*);
    typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_D3D12_EvaluateFeature)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);

    PFN_NVSDK_NGX_D3D12_Init_with_ProjectID pfnInit = nullptr;
    PFN_NVSDK_NGX_D3D12_Shutdown pfnShutdown = nullptr;
    PFN_NVSDK_NGX_D3D12_GetCapabilityParameters pfnGetCapParams = nullptr;
    PFN_NVSDK_NGX_D3D12_AllocateParameters pfnAllocParams = nullptr;
    PFN_NVSDK_NGX_D3D12_DestroyParameters pfnDestroyParams = nullptr;
    PFN_NVSDK_NGX_D3D12_CreateFeature pfnCreateFeature = nullptr;
    PFN_NVSDK_NGX_D3D12_ReleaseFeature pfnReleaseFeature = nullptr;
    PFN_NVSDK_NGX_D3D12_EvaluateFeature pfnEvaluateFeature = nullptr;

    int inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;
    int scaledW_ = 0, scaledH_ = 0;
    float desktopScale_ = 1.0f;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;

    bool initialized_ = false;
    std::string error_;
};

} // namespace fsrng

