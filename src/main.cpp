#include <windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <iomanip>
#include <algorithm>

#include "capture/capture_manager.h"
#include "neural_engine/neural_upscaler.h"
#include "neural_engine/ngx_interop.h"
#include "display/overlay_window.h"
#include "display/swapchain_presenter.h"
#include "display/imgui_overlay.h"
#include "utils/config_reader.h"
#include "utils/hotkey_manager.h"

using namespace fsrng;

static std::atomic<bool> g_running{ true };

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        std::cout << "\n[FSR-NG] Shutdown signal received. Closing..." << std::endl;
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    std::cout << "=========================================================\n";
    std::cout << "  FSR-NG-Scaling: Lossless Neural Scaler for Windows 11   \n";
    std::cout << "  AMD RDNA AI / DirectX 12 Isolated Compute Engine        \n";
    std::cout << "=========================================================\n\n";

    // 1. Load Configuration
    ConfigReader config("config/settings.ini");
    float intensity             = config.GetFloat("intensity", 1.00f);
    float structureIntensity    = config.GetFloat("structure_intensity", 1.00f);
    float toneIntensity         = config.GetFloat("tone_intensity", 1.00f);
    float splitScreen           = config.GetFloat("debug_split_screen", 0.0f);
    float temporalStability     = config.GetFloat("temporal_stability", 0.0f); // Default 0 to eliminate ghosting
    float detailBoost           = config.GetFloat("detail_boost", 1.35f);
    float catmullRom            = config.GetFloat("catmull_rom", 1.0f);
    float skinStructureStrength = config.GetFloat("skin_structure_strength", -1.00f);
    float nrPasses              = config.GetFloat("nr_passes", 1.0f);
    float scenePaperWhite       = config.GetFloat("scene_paper_white", 1.000f);
    float hdrTransferStrength   = config.GetFloat("hdr_transfer_strength", 1.00f);
    float colorStrength         = config.GetFloat("color_strength", 1.00f);
    float enableNR              = config.GetFloat("enable_nr", 1.0f);
    float autoMask              = config.GetFloat("auto_mask", 1.0f);
    int   scaleFactor           = config.GetInt("scale_factor", 2);
    int   toggleKey             = config.GetInt("toggle_key", 83); // 'S'
    int   reloadKey             = config.GetInt("reload_key", 82); // 'R'
    int   toggleModeKey         = config.GetInt("toggle_mode_key", 87); // 'W'
    std::string captureMode     = config.GetString("capture_mode", "window");
    std::string modelPath       = config.GetString("model_path", "models/fsr_ng_model.safetensors");
    std::string expectedSha     = config.GetString("expected_sha256", "");

    std::cout << "[Config] Scale Factor: " << scaleFactor << "x\n";
    std::cout << "[Config] NR Intensity: " << intensity << "\n";
    std::cout << "[Config] Local Structure: " << structureIntensity << "\n";
    std::cout << "[Config] Local Tone: " << toneIntensity << "\n";
    std::cout << "[Config] Skin Structure Strength: " << skinStructureStrength << "\n";
    std::cout << "[Config] NR Passes: " << nrPasses << "\n";
    std::cout << "[Config] Temporal Stability (Zero-Ghosting): " << temporalStability << "\n";
    std::cout << "[Config] Detail Boost (DLSS 5 OpenNR): " << detailBoost << "\n";
    std::cout << "[Config] Catmull-Rom 9-Tap Filter: " << (catmullRom > 0.5f ? "ON" : "OFF") << "\n";
    std::cout << "[Config] Initial Capture Mode: " << captureMode << "\n";
    std::cout << "[Config] Split Screen: " << (splitScreen > 0.5f ? "ON" : "OFF") << "\n";

    // 2. Initialize Neural Upscaler (isolated D3D12 device & compute queue)
    NeuralUpscaler upscaler;
    std::cout << "\n[Engine] Initializing DirectX 12 Compute Pipeline...\n";
    if (!upscaler.Initialize("shaders/neural_scale_cs.hlsl", modelPath, expectedSha)) {
        std::cerr << "[Engine] ERROR: " << upscaler.error() << std::endl;
        return 1;
    }
    std::cout << "[Engine] D3D12 Compute Pipeline ready." << std::endl;

    ID3D12Device* device = upscaler.engine().device();
    ID3D12CommandQueue* computeQueue = upscaler.engine().queue();
    ID3D12CommandQueue* directQueue  = upscaler.engine().directQueue();

    // 2.1 Probe NVIDIA NGX DLSS-NR dynamic library & fallback gracefully to OpenNR SafeTensors
    NgxInterop ngx;
    ngx.ProbeAndInitialize(device);
    std::cout << "[Engine] " << ngx.statusMessage() << std::endl;

    // 3. Command Allocator and List for Compute Dispatches
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> computeAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> computeCmd;
    HRESULT hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        IID_PPV_ARGS(computeAlloc.GetAddressOf())
    );
    if (FAILED(hr)) {
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(computeAlloc.GetAddressOf()));
    }

    hr = device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        computeAlloc.Get(),
        nullptr,
        IID_PPV_ARGS(computeCmd.GetAddressOf())
    );
    if (FAILED(hr)) {
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, computeAlloc.Get(), nullptr, IID_PPV_ARGS(computeCmd.GetAddressOf()));
    }
    computeCmd->Close();

    // 4. Initialize Capture Engine via DXGI Output Duplication
    CaptureManager capture;
    std::cout << "[Capture] Initializing DXGI Desktop Duplication...\n";
    if (!capture.Initialize(device)) {
        std::cerr << "[Capture] ERROR: " << capture.error() << std::endl;
        return 1;
    }

    if (!capture.Start(nullptr)) { // Full primary desktop capture
        std::cerr << "[Capture] ERROR starting capture: " << capture.error() << std::endl;
        return 1;
    }

    uint32_t capWidth  = capture.width();
    uint32_t capHeight = capture.height();
    std::cout << "[Capture] Capture active: " << capWidth << "x" << capHeight << " (DXGI VRAM Direct)\n";

    // 5. Initialize Display Overlay Window
    uint32_t targetWidth  = capWidth;
    uint32_t targetHeight = capHeight;
    if (scaleFactor > 1) {
        targetWidth  = capWidth * scaleFactor;
        targetHeight = capHeight * scaleFactor;
    }

    OverlayWindow overlay;
    std::cout << "[Display] Creating borderless topmost overlay...\n";
    if (!overlay.Create("FSR-NG-Overlay", capWidth, capHeight)) {
        std::cerr << "[Display] ERROR: Failed to create overlay window.\n";
        return 1;
    }

    // 6. Initialize SwapChain Presenter (DXGI Flip Discard)
    SwapchainPresenter presenter;
    if (!presenter.Initialize(overlay.hwnd(), device, directQueue, capWidth, capHeight)) {
        std::cerr << "[Display] ERROR: " << presenter.error() << std::endl;
        return 1;
    }

    // Allocate neural upscaler resources
    upscaler.Resize(capWidth, capHeight, capWidth, capHeight);
    upscaler.params.intensity = intensity;
    upscaler.params.structureIntensity = structureIntensity;
    upscaler.params.toneIntensity = toneIntensity;
    upscaler.params.splitScreen = splitScreen;
    upscaler.params.temporalStability = temporalStability;
    upscaler.params.detailBoost = detailBoost;
    upscaler.params.catmullRom = catmullRom;
    upscaler.params.skinStructureStrength = skinStructureStrength;
    upscaler.params.nrPasses = nrPasses;
    upscaler.params.scenePaperWhite = scenePaperWhite;
    upscaler.params.hdrTransferStrength = hdrTransferStrength;
    upscaler.params.colorStrength = colorStrength;
    upscaler.params.enableNR = enableNR;
    upscaler.params.autoMask = autoMask;
    upscaler.params.resetHistory = 1.0f;

    // 6.1 Initialize Dear ImGui overlay (DLSS 5 Neural Rendering menu)
    ImGuiOverlay imgui;
    if (!imgui.Initialize(overlay.hwnd(), device, directQueue, DXGI_FORMAT_B8G8R8A8_UNORM)) {
        std::cerr << "[ImGui] Warning: Failed to initialize Dear ImGui overlay\n";
    }
    overlay.SetHitTestCallback([&](int x, int y) -> bool {
        return imgui.IsPointInsideMenu(x, y);
    });

    overlay.SetMsgCallback([&](HWND h, UINT m, WPARAM w, LPARAM l) -> bool {
        return imgui.ProcessMessage(h, m, w, l);
    });

    // 7. Register Global Hotkeys
    HotkeyManager hotkeys(overlay.hwnd());
    overlay.SetHotKeyCallback([&](WPARAM w, LPARAM l) {
        hotkeys.OnHotKey(w, l);
    });
    std::atomic<bool> scalingActive{ true };
    std::atomic<bool> windowModeActive{ captureMode == "window" };
    HWND lastForegroundHwnd = nullptr;
    std::string currentTargetTitle = "Desktop";

    // Insert or Home: Toggle DLSS 5 Neural Rendering GUI menu
    auto toggleGuiMenu = [&]() {
        imgui.ToggleVisibility();
        std::cout << "\n[Hotkey] DLSS 5 GUI Menu: "
                  << (imgui.isVisible() ? "OPEN (Interactive)" : "CLOSED (Hidden)") << std::endl;
    };
    hotkeys.Register(VK_INSERT, 0, toggleGuiMenu);
    hotkeys.Register(VK_HOME, 0, toggleGuiMenu);

    // Ctrl+Alt+S: Toggle live scaling
    hotkeys.Register(toggleKey, HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        scalingActive = !scalingActive.load();
        overlay.Show(scalingActive.load());
        upscaler.ResetHistory();
        std::cout << "\n[Hotkey] Scaling toggled: " << (scalingActive.load() ? "ENABLED (Visible)" : "DISABLED (Hidden)") << std::endl;
    });

    // Ctrl+Alt+W: Toggle between Dynamic Foreground Window and Full Desktop
    hotkeys.Register(toggleModeKey, HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        windowModeActive = !windowModeActive.load();
        upscaler.ResetHistory();
        std::cout << "\n[Hotkey] Capture mode toggled: "
                  << (windowModeActive.load() ? "DYNAMIC ACTIVE WINDOW (Borderless Upscale)" : "FULL MONITOR DESKTOP")
                  << std::endl;
    });

    // Ctrl+Alt+R: Live reload settings.ini
    hotkeys.Register(reloadKey, HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        config.Reload();
        upscaler.params.intensity             = config.GetFloat("intensity", 1.00f);
        upscaler.params.structureIntensity    = config.GetFloat("structure_intensity", 1.00f);
        upscaler.params.toneIntensity         = config.GetFloat("tone_intensity", 1.00f);
        upscaler.params.splitScreen           = config.GetFloat("debug_split_screen", 0.0f);
        upscaler.params.temporalStability     = config.GetFloat("temporal_stability", 0.0f);
        upscaler.params.detailBoost           = config.GetFloat("detail_boost", 1.35f);
        upscaler.params.catmullRom            = config.GetFloat("catmull_rom", 1.0f);
        upscaler.params.skinStructureStrength = config.GetFloat("skin_structure_strength", -1.00f);
        upscaler.params.nrPasses              = config.GetFloat("nr_passes", 1.0f);
        upscaler.params.scenePaperWhite       = config.GetFloat("scene_paper_white", 1.000f);
        upscaler.params.hdrTransferStrength   = config.GetFloat("hdr_transfer_strength", 1.00f);
        upscaler.params.colorStrength         = config.GetFloat("color_strength", 1.00f);
        upscaler.params.enableNR              = config.GetFloat("enable_nr", 1.0f);
        upscaler.params.autoMask              = config.GetFloat("auto_mask", 1.0f);
        std::string newMode = config.GetString("capture_mode", "window");
        windowModeActive = (newMode == "window");
        upscaler.ResetHistory();
        std::cout << "\n[Hotkey] Settings reloaded live from settings.ini:"
                  << " Intensity=" << upscaler.params.intensity
                  << " Structure=" << upscaler.params.structureIntensity
                  << " Tone=" << upscaler.params.toneIntensity
                  << " SkinStructure=" << upscaler.params.skinStructureStrength
                  << " NRPasses=" << upscaler.params.nrPasses
                  << " Temporal=" << upscaler.params.temporalStability
                  << " DetailBoost=" << upscaler.params.detailBoost
                  << " CatmullRom=" << (upscaler.params.catmullRom > 0.5f ? "ON" : "OFF")
                  << " Mode=" << (windowModeActive.load() ? "Window" : "Desktop")
                  << " SplitScreen=" << upscaler.params.splitScreen << std::endl;
    });

    overlay.Show(true);
    std::cout << "\n=========================================================\n";
    std::cout << "  FSR-NG Active! Controls:\n";
    std::cout << "  * [Insert] / [Home]: Toggle DLSS 5 Neural Rendering GUI menu\n";
    std::cout << "  * [Ctrl + Alt + S] : Toggle Scaling Overlay On/Off\n";
    std::cout << "  * [Ctrl + Alt + W] : Toggle Window vs Desktop mode\n";
    std::cout << "  * [Ctrl + Alt + R] : Reload settings.ini live\n";
    std::cout << "  * [Ctrl + C]       : Exit application\n";
    std::cout << "=========================================================\n\n";


    // 8. Main Render Loop
    auto lastFpsTime = std::chrono::steady_clock::now();
    uint64_t frameCounter = 0;
    uint64_t totalFramesRendered = 0;
    float currentFps = 0.0f;

    while (g_running.load()) {
        if (!overlay.ProcessMessages()) {
            break; // WM_QUIT
        }

        if (!scalingActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // A. Handle Dynamic Foreground Window Crop
        if (windowModeActive.load()) {
            WindowClientInfo wInfo = capture.GetForegroundClientArea(overlay.hwnd());
            if (wInfo.valid && capWidth > 0 && capHeight > 0) {
                float cropX = std::clamp(static_cast<float>(wInfo.x) / static_cast<float>(capWidth), 0.0f, 1.0f);
                float cropY = std::clamp(static_cast<float>(wInfo.y) / static_cast<float>(capHeight), 0.0f, 1.0f);
                float cropW = std::clamp(static_cast<float>(wInfo.width) / static_cast<float>(capWidth), 0.01f, 1.0f);
                float cropH = std::clamp(static_cast<float>(wInfo.height) / static_cast<float>(capHeight), 0.01f, 1.0f);

                upscaler.params.captureCrop = float4{ cropX, cropY, cropW, cropH };
                upscaler.params.modeWindow = 1.0f;

                if (wInfo.hwnd != lastForegroundHwnd) {
                    lastForegroundHwnd = wInfo.hwnd;
                    currentTargetTitle = wInfo.title.empty() ? "Target Window" : wInfo.title;
                    upscaler.ResetHistory();
                    std::cout << "\n[Target Window] Active: \"" << currentTargetTitle
                              << "\" (" << wInfo.width << "x" << wInfo.height << ")" << std::endl;
                }
            } else {
                upscaler.params.modeWindow = 0.0f;
                upscaler.params.captureCrop = float4{ 0.0f, 0.0f, 1.0f, 1.0f };
            }
        } else {
            upscaler.params.modeWindow = 0.0f;
            upscaler.params.captureCrop = float4{ 0.0f, 0.0f, 1.0f, 1.0f };
        }

        // B. Acquire frame from GPU VRAM
        ID3D12Resource* inputFrame = capture.AcquireLatestFrame();
        if (!inputFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // C. Execute Neural Upscaler Compute Pass
        computeAlloc->Reset();
        computeCmd->Reset(computeAlloc.Get(), nullptr);

        upscaler.Process(computeCmd.Get(), inputFrame);

        computeCmd->Close();
        ID3D12CommandList* lists[] = { computeCmd.Get() };
        computeQueue->ExecuteCommandLists(1, lists);

        totalFramesRendered++;

        // D. Present via Flip Discard SwapChain with live Dear ImGui DLSS 5 Neural Rendering overlay
        if (upscaler.output()) {
            presenter.Present(upscaler.output(), [&](ID3D12GraphicsCommandList* cl) {
                imgui.Render(cl, upscaler.params, currentFps, totalFramesRendered, upscaler.outWidth(), upscaler.outHeight());
            }, true);
        }

        frameCounter++;
        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed = now - lastFpsTime;
        if (elapsed.count() >= 1.0f) {
            currentFps = frameCounter / elapsed.count();
            frameCounter = 0;
            lastFpsTime = now;

            std::cout << "\r[Running] FPS: " << std::fixed << std::setprecision(1) << currentFps
                      << " | Mode: " << (upscaler.params.modeWindow > 0.5f ? "WINDOW" : "DESKTOP")
                      << " | Out: " << upscaler.outWidth() << "x" << upscaler.outHeight()
                      << " | Skin: " << upscaler.params.skinStructureStrength
                      << " | Passes: " << (int)round(upscaler.params.nrPasses)
                      << " | NR: " << (upscaler.params.enableNR > 0.5f ? "ON" : "OFF")
                      << " | Temporal: " << upscaler.params.temporalStability
                      << " | Menu: " << (imgui.isVisible() ? "VISIBLE" : "HIDDEN")
                      << std::flush;
        }
    }

    std::cout << "\n[FSR-NG] Cleaning up and shutting down gracefully...\n";
    imgui.Shutdown();
    overlay.Show(false);
    capture.Stop();

    std::cout << "[FSR-NG] Terminated successfully.\n";
    return 0;
}
