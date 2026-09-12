#include "ngx_interop.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iostream>

namespace fsrng {

NgxInterop::NgxInterop() = default;

NgxInterop::~NgxInterop() {
    Shutdown();
}

bool NgxInterop::ProbeAndInitialize(ID3D12Device* device, const std::string& dllPath) {
    hModule_ = LoadLibraryA(dllPath.c_str());
    if (!hModule_) {
        // Fallback to local backend directory
        hModule_ = LoadLibraryA("backend/OptiScaler.dll");
    }

    if (!hModule_) {
        statusMessage_ = "Envy-Diamond (OptiScaler.dll) no encontrado en backend/.";
        return false;
    }

    // Inspect critical NGX D3D12 entry points exported by OptiScaler
    FARPROC pfnInit = GetProcAddress(hModule_, "NVSDK_NGX_D3D12_Init");
    FARPROC pfnCreate = GetProcAddress(hModule_, "NVSDK_NGX_D3D12_CreateFeature");
    FARPROC pfnEval = GetProcAddress(hModule_, "NVSDK_NGX_D3D12_EvaluateFeature");

    if (!pfnInit || !pfnCreate || !pfnEval) {
        statusMessage_ = "OptiScaler.dll loaded, but missing standard NGX D3D12 entry points";
        return false;
    }

    statusMessage_ = "Envy-Diamond (OptiScaler) Engine cargado correctamente. Usando DLSS-NR Multipass AMD.";
    supported_ = true;
    return true;
}

void NgxInterop::Shutdown() {
    if (hModule_) {
        FreeLibrary(hModule_);
        hModule_ = nullptr;
    }
    supported_ = false;
}

} // namespace fsrng
