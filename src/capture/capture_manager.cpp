#include "capture_manager.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <iostream>

typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(
    IDXGIAdapter*,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL*,
    UINT,
    UINT,
    ID3D11Device**,
    D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**
);

namespace fsrng {

CaptureManager::CaptureManager() = default;

CaptureManager::~CaptureManager() {
    Stop();
    if (d3d11Dll_) {
        FreeLibrary(d3d11Dll_);
        d3d11Dll_ = nullptr;
    }
}

bool CaptureManager::Initialize(ID3D12Device* d3d12Device) {
    if (!d3d12Device) {
        error_ = "Null D3D12 device provided to CaptureManager";
        return false;
    }
    d3d12Device_ = d3d12Device;

    if (!InitializeD3D11()) {
        return false;
    }

    return true;
}

bool CaptureManager::InitializeD3D11() {
    if (d3d11Device_ && d3d11Context_) return true;

    d3d11Dll_ = LoadLibraryA("d3d11.dll");
    if (!d3d11Dll_) {
        error_ = "Failed to load d3d11.dll";
        return false;
    }

    auto pfnCreateDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
        GetProcAddress(d3d11Dll_, "D3D11CreateDevice")
    );
    if (!pfnCreateDevice) {
        error_ = "Failed to find D3D11CreateDevice entry point";
        return false;
    }

    // Match the exact adapter that D3D12 is running on using LUID
    LUID luid = d3d12Device_->GetAdapterLuid();
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    Microsoft::WRL::ComPtr<IDXGIAdapter1> matchedAdapter;
    if (factory) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> curAdapter;
        for (UINT i = 0; factory->EnumAdapters1(i, curAdapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            curAdapter->GetDesc1(&desc);
            if (desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
                matchedAdapter = curAdapter;
                break;
            }
        }
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL createdLevel;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    IDXGIAdapter* pAdapter = matchedAdapter.Get();
    D3D_DRIVER_TYPE driverType = pAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

    HRESULT hr = pfnCreateDevice(
        pAdapter,
        driverType,
        nullptr,
        flags,
        featureLevels,
        2,
        D3D11_SDK_VERSION,
        d3d11Device_.GetAddressOf(),
        &createdLevel,
        d3d11Context_.GetAddressOf()
    );

    if (FAILED(hr)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = pfnCreateDevice(
            pAdapter,
            driverType,
            nullptr,
            flags,
            featureLevels,
            2,
            D3D11_SDK_VERSION,
            d3d11Device_.GetAddressOf(),
            &createdLevel,
            d3d11Context_.GetAddressOf()
        );
    }

    if (FAILED(hr)) {
        error_ = "D3D11CreateDevice failed: " + std::to_string(hr);
        return false;
    }

    return true;
}

bool CaptureManager::SetupDuplication() {
    if (!d3d11Device_) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = d3d11Device_.As(&dxgiDevice);
    if (FAILED(hr)) {
        error_ = "Failed to query IDXGIDevice from D3D11 device";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
    if (FAILED(hr)) {
        error_ = "Failed to get DXGI adapter";
        return false;
    }

    // Find the output attached to desktop
    Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
    Microsoft::WRL::ComPtr<IDXGIOutput1> dxgiOutput1;

    for (UINT i = 0; dxgiAdapter->EnumOutputs(i, dxgiOutput.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC odesc;
        dxgiOutput->GetDesc(&odesc);
        if (odesc.AttachedToDesktop) {
            hr = dxgiOutput.As(&dxgiOutput1);
            if (SUCCEEDED(hr)) break;
        }
    }

    if (!dxgiOutput1) {
        error_ = "No active desktop display output found on adapter";
        return false;
    }

    duplication_.Reset();
    hr = dxgiOutput1->DuplicateOutput(d3d11Device_.Get(), duplication_.GetAddressOf());
    if (FAILED(hr)) {
        error_ = "DuplicateOutput failed with HRESULT: " + std::to_string(hr);
        return false;
    }

    DXGI_OUTDUPL_DESC duplDesc{};
    duplication_->GetDesc(&duplDesc);
    width_ = duplDesc.ModeDesc.Width;
    height_ = duplDesc.ModeDesc.Height;

    if (!CreateSharedTexture(width_, height_)) {
        return false;
    }

    return true;
}

bool CaptureManager::CreateSharedTexture(uint32_t w, uint32_t h) {
    if (!d3d11Device_ || !d3d12Device_ || w == 0 || h == 0) return false;

    d3d11SharedTexture_.Reset();
    d3d12SharedResource_.Reset();
    keyedMutex_.Reset();

    D3D11_TEXTURE2D_DESC desc11{};
    desc11.Width = w;
    desc11.Height = h;
    desc11.MipLevels = 1;
    desc11.ArraySize = 1;
    desc11.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc11.SampleDesc.Count = 1;
    desc11.Usage = D3D11_USAGE_DEFAULT;
    desc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc11.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex11;
    HRESULT hr = d3d11Device_->CreateTexture2D(&desc11, nullptr, tex11.GetAddressOf());
    if (FAILED(hr)) {
        error_ = "Failed to create shared D3D11 capture texture: " + std::to_string(hr);
        return false;
    }

    hr = tex11.As(&keyedMutex_);
    if (SUCCEEDED(hr) && keyedMutex_) {
        // Initialize mutex state
        keyedMutex_->AcquireSync(0, 0);
        keyedMutex_->ReleaseSync(0);
    }

    d3d11SharedTexture_ = tex11;

    HANDLE sharedHandle = nullptr;
    Microsoft::WRL::ComPtr<IDXGIResource1> dxgiRes1;
    hr = tex11.As(&dxgiRes1);
    if (SUCCEEDED(hr)) {
        hr = dxgiRes1->CreateSharedHandle(
            nullptr,
            GENERIC_ALL,
            nullptr,
            &sharedHandle
        );
    }

    if (FAILED(hr) || !sharedHandle) {
        error_ = "Failed to export shared NT handle from D3D11 texture";
        return false;
    }

    hr = d3d12Device_->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(d3d12SharedResource_.GetAddressOf()));
    CloseHandle(sharedHandle);

    if (FAILED(hr)) {
        error_ = "D3D12 OpenSharedHandle failed: " + std::to_string(hr);
        return false;
    }

    return true;
}

bool CaptureManager::Start(HWND targetWindow) {
    targetWindow_ = targetWindow;
    if (!SetupDuplication()) {
        return false;
    }
    active_ = true;

    // Grab initial frame to populate shared texture immediately
    for (int retry = 0; retry < 10; ++retry) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
        HRESULT hr = duplication_->AcquireNextFrame(100, &frameInfo, desktopResource.GetAddressOf());
        if (SUCCEEDED(hr) && desktopResource) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTex;
            if (SUCCEEDED(desktopResource.As(&desktopTex))) {
                Microsoft::WRL::ComPtr<ID3D11Resource> dstRes;
                d3d11SharedTexture_.As(&dstRes);
                if (dstRes) {
                    if (keyedMutex_) keyedMutex_->AcquireSync(0, INFINITE);
                    d3d11Context_->CopyResource(dstRes.Get(), desktopTex.Get());
                    if (keyedMutex_) keyedMutex_->ReleaseSync(0);
                    d3d11Context_->Flush();
                    frameCount_++;
                }
            }
            duplication_->ReleaseFrame();
            break;
        }
        Sleep(20);
    }

    return true;
}

bool CaptureManager::StartByTitle(const std::string& windowTitle) {
    HWND hwnd = FindWindowA(nullptr, windowTitle.c_str());
    if (!hwnd) {
        error_ = "Window not found: " + windowTitle;
        return false;
    }
    return Start(hwnd);
}

void CaptureManager::Stop() {
    active_ = false;
    keyedMutex_.Reset();
    duplication_.Reset();
    d3d12SharedResource_.Reset();
    d3d11SharedTexture_.Reset();
}

ID3D12Resource* CaptureManager::AcquireLatestFrame() {
    if (!active_ || !duplication_ || !d3d11Context_ || !d3d11SharedTexture_) {
        return nullptr;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;

    HRESULT hr = duplication_->AcquireNextFrame(0, &frameInfo, desktopResource.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // Return previously captured frame on timeout
        return d3d12SharedResource_.Get();
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        std::cout << "[CaptureManager] DXGI Access lost, re-initializing duplication..." << std::endl;
        SetupDuplication();
        return d3d12SharedResource_.Get();
    }

    if (FAILED(hr) || !desktopResource) {
        return d3d12SharedResource_.Get();
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTex;
    hr = desktopResource.As(&desktopTex);
    if (SUCCEEDED(hr)) {
        Microsoft::WRL::ComPtr<ID3D11Resource> dstRes;
        d3d11SharedTexture_.As(&dstRes);
        if (dstRes) {
            if (keyedMutex_) keyedMutex_->AcquireSync(0, INFINITE);
            d3d11Context_->CopyResource(dstRes.Get(), desktopTex.Get());
            if (keyedMutex_) keyedMutex_->ReleaseSync(0);
            d3d11Context_->Flush();
            frameCount_++;
        }
    }

    duplication_->ReleaseFrame();
    return d3d12SharedResource_.Get();
}

WindowClientInfo CaptureManager::GetForegroundClientArea(HWND excludeHwnd) {
    WindowClientInfo info{};
    HWND fg = GetForegroundWindow();
    if (!fg || fg == excludeHwnd || !IsWindow(fg)) {
        return info;
    }

    if (IsIconic(fg) || !IsWindowVisible(fg)) {
        return info;
    }

    char className[256] = { 0 };
    GetClassNameA(fg, className, sizeof(className));
    if (strcmp(className, "Progman") == 0 ||
        strcmp(className, "WorkerW") == 0 ||
        strcmp(className, "Shell_TrayWnd") == 0 ||
        strcmp(className, "Shell_SecondaryTrayWnd") == 0 ||
        strcmp(className, "Windows.UI.Core.CoreWindow") == 0 ||
        strcmp(className, "FSRNG_OverlayClass") == 0) {
        return info;
    }

    RECT rc{};
    if (!GetClientRect(fg, &rc)) {
        return info;
    }

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 128 || h < 128) {
        return info; // Ignore toolbars, popups, or invisible floating windows
    }

    POINT pt{ rc.left, rc.top };
    if (!ClientToScreen(fg, &pt)) {
        return info;
    }

    info.x = pt.x;
    info.y = pt.y;
    info.width = w;
    info.height = h;
    info.hwnd = fg;
    info.valid = true;

    char titleBuf[256] = { 0 };
    GetWindowTextA(fg, titleBuf, sizeof(titleBuf));
    info.title = titleBuf;

    return info;
}

} // namespace fsrng
