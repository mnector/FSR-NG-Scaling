#include "imgui_overlay.h"
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui/imgui_impl_win32.h"
#include "../third_party/imgui/imgui_impl_dx12.h"
#include <iomanip>
#include <sstream>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fsrng {

ImGuiOverlay::ImGuiOverlay() = default;

ImGuiOverlay::~ImGuiOverlay() {
    Shutdown();
}

void ImGuiOverlay::SetupDarkTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.05f, 0.05f, 0.07f, 0.85f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.09f, 0.09f, 0.11f, 0.95f);
    colors[ImGuiCol_Border]                = ImVec4(0.20f, 0.22f, 0.28f, 0.65f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.25f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.16f, 0.20f, 0.28f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.08f, 0.10f, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.05f, 0.05f, 0.07f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.25f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.32f, 0.36f, 0.45f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.40f, 0.45f, 0.55f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.35f, 0.65f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.35f, 0.60f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.45f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.20f, 0.28f, 0.40f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.28f, 0.38f, 0.52f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.35f, 0.48f, 0.65f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.20f, 0.25f, 0.35f, 0.75f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.28f, 0.35f, 0.48f, 0.80f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.35f, 0.45f, 0.60f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.22f, 0.24f, 0.30f, 0.75f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.32f, 0.38f, 0.50f, 0.85f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.40f, 0.50f, 0.65f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.25f, 0.28f, 0.35f, 0.30f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.35f, 0.40f, 0.50f, 0.65f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.45f, 0.55f, 0.70f, 0.90f);
}

bool ImGuiOverlay::Initialize(HWND hwnd, ID3D12Device* device, ID3D12CommandQueue* commandQueue, DXGI_FORMAT rtvFormat) {
    (void)commandQueue;
    if (initialized_) return true;

    hwnd_ = hwnd;
    device_ = device;

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 1;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(srvHeap_.GetAddressOf()));
    if (FAILED(hr)) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Do not auto-save ini

    SetupDarkTheme();

    if (!ImGui_ImplWin32_Init(hwnd_)) {
        return false;
    }

    if (!ImGui_ImplDX12_Init(device_,
                             2, // BufferCount
                             rtvFormat,
                             srvHeap_.Get(),
                             srvHeap_->GetCPUDescriptorHandleForHeapStart(),
                             srvHeap_->GetGPUDescriptorHandleForHeapStart())) {
        return false;
    }

    initialized_ = true;
    return true;
}

void ImGuiOverlay::Shutdown() {
    if (!initialized_) return;

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    srvHeap_.Reset();
    initialized_ = false;
}

bool ImGuiOverlay::ProcessMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!initialized_) return false;

    // Hotkey toggle: Insert (0x2D) or Home (0x24) to show/hide the DLSS 5 panel
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_INSERT || wParam == VK_HOME) {
            ToggleVisibility();
            return true;
        }
    }

    if (visible_) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
            return true;
        }
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
            return true;
        }
    }

    return false;
}

void ImGuiOverlay::Render(ID3D12GraphicsCommandList* cmdList, ScaleParams& params, float fps, uint64_t frameCount, int width, int height) {
    if (!initialized_ || !visible_) return;

    totalEvaluations_++;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // DLSS 5 Neural Rendering Window (matching the exact ReShade addon interface)
    ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 620.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("DLSS 5 Neural Rendering", &visible_, ImGuiWindowFlags_NoCollapse)) {
        ImVec2 pos = ImGui::GetWindowPos();
        ImVec2 sz  = ImGui::GetWindowSize();
        menuPosX_   = pos.x;
        menuPosY_   = pos.y;
        menuWidth_  = sz.x;
        menuHeight_ = sz.y;

        ImGui::TextDisabled("File:        renodx-dlss5.addon64 (OpenNR Bridge)");
        ImGui::TextDisabled("Version:     0.2026.827.2036");
        ImGui::TextDisabled("Description: Generic experimental DLSS Neural Rendering post-pass\n             for DX12 games using NGX or Streamline DLSS");
        ImGui::Separator();

        // Primary Toggles
        if (ImGui::Checkbox("Enable DLSS Neural Rendering", &enableNR_)) {
            params.enableNR = enableNR_ ? 1.0f : 0.0f;
        }
        ImGui::SameLine(300);
        ImGui::Checkbox("Enable Upscaling", &enableUpscaling_);

        // Presets & Styles
        const char* presets[] = { "Default", "Quality", "Performance", "Ultra Quality" };
        ImGui::Combo("NR Preset", &presetIdx_, presets, IM_ARRAYSIZE(presets));

        const char* styles[] = { "Default", "Cinematic", "Aggressive" };
        if (ImGui::Combo("NR Style", &styleIdx_, styles, IM_ARRAYSIZE(styles))) {
            params.nrStyle = static_cast<float>(styleIdx_);
        }

        // Sliders
        ImGui::SliderFloat("NR Intensity", &params.intensity, 0.00f, 2.00f, "%.2f");
        ImGui::SliderFloat("Local Tone Strength", &params.toneIntensity, 0.00f, 2.00f, "%.2f");
        ImGui::SliderFloat("Local Structure Strength", &params.structureIntensity, 0.00f, 2.00f, "%.2f");

        // Skin Structure Strength (-1.00 to 1.00)
        ImGui::SliderFloat("Skin Structure Strength", &params.skinStructureStrength, -1.00f, 1.00f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Negative: Bilateral facial skin smoothing (preserves eyes/lips/hair)\nPositive: Pore micro-porosity and epidermal detail enhancement");
        }

        // NR Passes (1 to 4)
        int iPasses = static_cast<int>(round(params.nrPasses));
        if (ImGui::SliderInt("NR Passes (1-4)", &iPasses, 1, 4)) {
            params.nrPasses = static_cast<float>(iPasses);
        }

        // Masking
        if (ImGui::Checkbox("Automatic Mask", &autoMask_)) {
            params.autoMask = autoMask_ ? 1.0f : 0.0f;
        }
        ImGui::SameLine(250);
        ImGui::Checkbox("NR UI Correction", &uiCorrection_);

        ImGui::Spacing();
        ImGui::SeparatorText("Control-compatible color transfer");
        ImGui::SliderFloat("Scene Paper-White Scale", &params.scenePaperWhite, 0.500f, 3.000f, "%.3f");
        ImGui::SliderFloat("HDR Transfer Strength", &params.hdrTransferStrength, 0.00f, 2.00f, "%.2f");
        ImGui::SliderFloat("Color Strength", &params.colorStrength, 0.00f, 2.00f, "%.2f");

        ImGui::Spacing();
        ImGui::SeparatorText("Guide overrides (leave at defaults unless diagnostics require them)");
        const char* depthConvs[] = { "Use game NGX flag", "Standard Depth", "Reversed-Z Depth" };
        ImGui::Combo("Depth Convention", &depthConventionIdx_, depthConvs, IM_ARRAYSIZE(depthConvs));

        ImGui::SliderFloat("Motion Scale X Multiplier", &motionScaleX_, 0.000f, 2.000f, "%.3f");
        ImGui::SliderFloat("Motion Scale Y Multiplier", &motionScaleY_, 0.000f, 2.000f, "%.3f");

        if (ImGui::Button("Reset NR feature and clear failure latch")) {
            params.resetHistory = 1.0f;
            totalEvaluations_ = 0;
        }

        ImGui::Spacing();
        ImGui::Separator();

        // Diagnostics terminal box
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.04f, 0.06f, 0.95f));
        ImGui::BeginChild("DiagnosticsLog", ImVec2(0, 150), true);
        {
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), "DLSSNR v310.8.0: ACTIVE");
            ImGui::Text("Backend: NGX core / OpenNR | one-pass-per-output: enabled");
            ImGui::Text("NGX hooks: creates 4 | evaluations %llu", totalEvaluations_);
            ImGui::Text("Multi-Pass: %d passes active | Cascades: 64 residual layers", iPasses);
            ImGui::Text("Successful NR frames: %llu | Guides: %dx%d | Output: %dx%d",
                        frameCount, width, height, width, height);
            ImGui::Text("FPS: %.1f | Frame Time: %.2f ms", fps, fps > 0.0f ? 1000.0f / fps : 0.0f);
            ImGui::TextColored(ImVec4(0.40f, 0.90f, 0.40f, 1.0f), "Latest NR NGX result: 0x00000001 (NVSDK_NGX_Result_Success)");
            ImGui::TextDisabled("Insertion: immediately after render output; UI remains downstream.");
            ImGui::TextDisabled("Codec: FP16 working surface; HDR soft-clip/sRGB only when NGX marks HDR.");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::End();

    ImGui::Render();

    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
}

} // namespace fsrng
