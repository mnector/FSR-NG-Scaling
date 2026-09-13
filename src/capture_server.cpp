#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <iostream>

typedef HRESULT(WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY)(REFIID, void**);

int main() {
    HMODULE hD3D11 = LoadLibraryExA("C:\\Windows\\System32\\d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE hDXGI = LoadLibraryExA("C:\\Windows\\System32\\dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (!hD3D11 || !hDXGI) return 1;

    PFN_D3D11_CREATE_DEVICE pfnD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pfnD3D11CreateDevice) return 1;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(pfnD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context))) {
        return 1;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    IDXGIOutput* output = nullptr;
    adapter->EnumOutputs(0, &output);
    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);

    IDXGIOutputDuplication* duplication = nullptr;
    if (FAILED(output1->DuplicateOutput(device, &duplication))) {
        std::cout << "HANDLE:0" << std::endl;
        return 2;
    }

    DXGI_OUTDUPL_DESC desc;
    duplication->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = desc.ModeDesc.Width;
    texDesc.Height = desc.ModeDesc.Height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ID3D11Texture2D* sharedTex = nullptr;
    if (FAILED(device->CreateTexture2D(&texDesc, nullptr, &sharedTex))) return 3;

    IDXGIResource1* res = nullptr;
    sharedTex->QueryInterface(__uuidof(IDXGIResource1), (void**)&res);

    HANDLE sharedHandle = nullptr;
    res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle);

    IDXGIKeyedMutex* keyedMutex = nullptr;
    sharedTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&keyedMutex);

    std::cout << "HANDLE:" << (unsigned long long)sharedHandle << std::endl;

    while (true) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo;
        IDXGIResource* desktopRes = nullptr;
        HRESULT hr = duplication->AcquireNextFrame(100, &frameInfo, &desktopRes);
        if (hr == S_OK) {
            ID3D11Texture2D* desktopTex = nullptr;
            desktopRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTex);
            
            keyedMutex->AcquireSync(0, INFINITE);
            context->CopyResource(sharedTex, desktopTex);
            keyedMutex->ReleaseSync(0);
            context->Flush();

            desktopTex->Release();
            desktopRes->Release();
            duplication->ReleaseFrame();
        }
    }
    return 0;
}
 
