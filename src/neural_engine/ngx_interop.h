#pragma once
#include <d3d12.h>
#include <string>
#include <windows.h>

namespace fsrng {

class NgxInterop {
public:
    NgxInterop();
    ~NgxInterop();

    // Dynamically probes nvngx_dlssnr.dll, inspects exported symbols and checks device capability
    bool ProbeAndInitialize(ID3D12Device* device, const std::string& dllPath = "nvngx_dlssnr.dll");

    bool isLoaded() const { return hModule_ != nullptr; }
    bool isSupported() const { return supported_; }
    const std::string& statusMessage() const { return statusMessage_; }

    void Shutdown();

private:
    HMODULE hModule_ = nullptr;
    bool supported_ = false;
    std::string statusMessage_;
};

} // namespace fsrng
