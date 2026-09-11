#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <functional>
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
    void SetVisible(bool visible) {
        if (visible_ != visible) {
            visible_ = visible;
            if (onVisibilityChanged_) {
                onVisibilityChanged_(visible_);
            }
        }
    }
    void ToggleVisibility() { SetVisible(!visible_); }
    void SetVisibilityChangedCallback(std::function<void(bool)> cb) { onVisibilityChanged_ = std::move(cb); }

    bool IsPointInsideMenu(int x, int y) const {
        if (!visible_) return false;
        return (x >= menuPosX_ && x <= (menuPosX_ + menuWidth_) &&
                y >= menuPosY_ && y <= (menuPosY_ + menuHeight_));
    }

private:
    std::function<void(bool)> onVisibilityChanged_;
    void SetupDarkTheme();

    HWND hwnd_ = nullptr;
    ID3D12Device* device_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;

    bool initialized_ = false;
    bool visible_ = false; // Hidden by default; toggle with Insert / Home

    float menuPosX_ = 24.0f;
    float menuPosY_ = 24.0f;
    float menuWidth_ = 520.0f;
    float menuHeight_ = 620.0f;

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
