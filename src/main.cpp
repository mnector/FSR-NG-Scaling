#include <windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <iomanip>
#include <algorithm>

#include "capture/capture_manager.h"
#include "neural_engine/neural_upscaler.h"
#include "display/overlay_window.h"
#include "display/swapchain_presenter.h"
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
    float intensity          = config.GetFloat("intensity", 0.75f);
    float structureIntensity = config.GetFloat("structure_intensity", 0.40f);
    float toneIntensity      = config.GetFloat("tone_intensity", 0.10f);
    float splitScreen        = config.GetFloat("debug_split_screen", 0.0f);
    float temporalStability  = config.GetFloat("temporal_stability", 0.85f);
    int   scaleFactor        = config.GetInt("scale_factor", 2);
    int   toggleKey          = config.GetInt("toggle_key", 83); // 'S'
    int   reloadKey          = config.GetInt("reload_key", 82); // 'R'
    int   toggleModeKey      = config.GetInt("toggle_mode_key", 87); // 'W'
    std::string captureMode  = config.GetString("capture_mode", "window");
    std::string modelPath    = config.GetString("model_path", "models/fsr_ng_model.safetensors");
    std::string expectedSha  = config.GetString("expected_sha256", "");

    std::cout << "[Config] Scale Factor: " << scaleFactor << "x\n";
    std::cout << "[Config] Neural Intensity: " << intensity << "\n";
    std::cout << "[Config] Structure Sharpening: " << structureIntensity << "\n";
    std::cout << "[Config] Tone Intensity: " << toneIntensity << "\n";
    std::cout << "[Config] Temporal Stability: " << temporalStability << "\n";
    std::cout << "[Config] Initial Capture Mode: " << captureMode << "\n";
    std::cout << "[Config] Split Screen: " << (splitScreen > 0.5f ? "ON" : "OFF") << "\n";

    // 2. Initialize Neural Upscaler (isolated D3D12 device & compute queue)
    NeuralUpscaler upscaler;
    std::cout << "\n[Engine] Initializing DirectX 12 Compute Pipeline...\n";
    if (!upscaler.Initialize("shaders/neural_scale_cs.hlsl", modelPath, expectedSha)) {
        std::cerr << "[Engine] ERROR: " << upscaler.error() << std::endl;
        return 1;
    }
    std::cout << "[Engine] D3D12 Compute Pipeline ready.\n";

    ID3D12Device* device = upscaler.engine().device();
    ID3D12CommandQueue* computeQueue = upscaler.engine().queue();
    ID3D12CommandQueue* directQueue  = upscaler.engine().directQueue();

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
    upscaler.params.resetHistory = 1.0f;

    // 7. Register Global Hotkeys
    HotkeyManager hotkeys(overlay.hwnd());
    overlay.SetHotKeyCallback([&](WPARAM w, LPARAM l) {
        hotkeys.OnHotKey(w, l);
    });
    std::atomic<bool> scalingActive{ true };
    std::atomic<bool> windowModeActive{ captureMode == "window" };
    HWND lastForegroundHwnd = nullptr;
    std::string currentTargetTitle = "Desktop";

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
        upscaler.params.intensity          = config.GetFloat("intensity", 0.75f);
        upscaler.params.structureIntensity = config.GetFloat("structure_intensity", 0.40f);
        upscaler.params.toneIntensity      = config.GetFloat("tone_intensity", 0.10f);
        upscaler.params.splitScreen        = config.GetFloat("debug_split_screen", 0.0f);
        upscaler.params.temporalStability  = config.GetFloat("temporal_stability", 0.85f);
        std::string newMode = config.GetString("capture_mode", "window");
        windowModeActive = (newMode == "window");
        upscaler.ResetHistory();
        std::cout << "\n[Hotkey] Settings reloaded live from settings.ini:"
                  << " Intensity=" << upscaler.params.intensity
                  << " Structure=" << upscaler.params.structureIntensity
                  << " Tone=" << upscaler.params.toneIntensity
                  << " Temporal=" << upscaler.params.temporalStability
                  << " Mode=" << (windowModeActive.load() ? "Window" : "Desktop")
                  << " SplitScreen=" << upscaler.params.splitScreen << std::endl;
    });

    overlay.Show(true);
    std::cout << "\n=========================================================\n";
    std::cout << "  FSR-NG Active! Controls:\n";
    std::cout << "  * [Ctrl + Alt + S] : Toggle Scaling Overlay On/Off\n";
    std::cout << "  * [Ctrl + Alt + W] : Toggle Window vs Desktop mode\n";
    std::cout << "  * [Ctrl + Alt + R] : Reload settings.ini live\n";
    std::cout << "  * [Ctrl + C]       : Exit application\n";
    std::cout << "=========================================================\n\n";

    // 8. Main Render Loop
    auto lastFpsTime = std::chrono::steady_clock::now();
    uint64_t frameCounter = 0;
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

        // D. Present via Flip Discard SwapChain (VSync synchronized)
        if (upscaler.output()) {
            presenter.Present(upscaler.output(), true);
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
                      << " | Temporal: " << upscaler.params.temporalStability
                      << " | Intensity: " << upscaler.params.intensity
                      << " | Split: " << (upscaler.params.splitScreen > 0.5f ? "ON" : "OFF")
                      << std::flush;
        }
    }

    std::cout << "\n[FSR-NG] Cleaning up and shutting down gracefully...\n";
    overlay.Show(false);
    capture.Stop();

    std::cout << "[FSR-NG] Terminated successfully.\n";
    return 0;
}
