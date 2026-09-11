#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include "neural_engine/d3d12_compute_engine.h"

namespace fsrng {

class ImGuiOverlay {
public:
    ImGuiOverlay();
    ~ImGuiOverlay();

    bool Initialize(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* commandQueue, DXGI_FORMAT rtvFormat);
    void Shutdown();

    // Forward Win32 messages to Dear ImGui
    bool ProcessMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Build and render the DLSS 5 Neural Rendering ImGui window
    void Render(ID3D12GraphicsCommandList* cmdList, ScaleParams& params, float fps, uint64_t frameCount, int width, int height);

    bool isVisible() const { return visible_; }
    void SetVisible(bool visible) { visible_ = visible; }
    void ToggleVisibility() { visible_ = !visible_; }

private:
    void SetupDarkTheme();

    HWND hwnd_ = nullptr;
    ID3D12Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;

    bool initialized_ = false;
    bool visible_ = true; // Shown by default for testing/tuning; toggleable with Insert / Home

    // UI state mirroring RenoDX / DLSS 5 ReShade addon
    bool enableNR_ = true;
    bool enableUpscaling_ = false;
    int  presetIdx_ = 0;
    int  styleIdx_ = 0;
    bool autoMask_ = true;
    bool uiCorrection_ = true;
    int  depthConventionIdx_ = 0;
    float motionScaleX_ = 1.000f;
    float motionScaleY_ = 1.000f;
    uint64_t totalEvaluations_ = 0;
};

} // namespace fsrng
