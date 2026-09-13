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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
    float splitScreen           = config.GetFloat("debug_split_screen", 0.0f);
    int   scaleFactor           = config.GetInt("scale_factor", 2);
    int   toggleKey             = config.GetInt("toggle_key", 83); // 'S'
    int   reloadKey             = config.GetInt("reload_key", 82); // 'R'
    int   toggleModeKey         = config.GetInt("toggle_mode_key", 87); // 'W'
    std::string captureMode     = config.GetString("capture_mode", "desktop");
    std::string modelPath       = config.GetString("model_path", "models/fsr_ng_model.safetensors");
    std::string expectedSha     = config.GetString("expected_sha256", "");

    std::cout << "[Config] Scale Factor: " << scaleFactor << "x\n";
    std::cout << "[Config] NR Intensity: " << intensity << "\n";
    std::cout << "[Config] Initial Capture Mode: " << captureMode << "\n";
    std::cout << "[Config] Split Screen: " << (splitScreen > 0.5f ? "ON" : "OFF") << "\n";

    // 2. Initialize Neural Upscaler (isolated D3D12 device & compute queue)
    NeuralUpscaler upscaler;
    std::cout << "\n[Engine] Initializing DirectX 12 Compute Pipeline...\n";
    if (!upscaler.Initialize()) {
        std::cerr << "[Engine] ERROR: " << upscaler.error() << std::endl;
        MessageBoxA(nullptr, "Fatal Error. Please run from terminal to see the logs.", "FSR-NG Error", MB_ICONERROR); return 1;
    }
    std::cout << "[Engine] D3D12 Compute Pipeline ready." << std::endl;

    ID3D12Device* device = upscaler.engine().device();
    ID3D12CommandQueue* computeQueue = upscaler.engine().queue();
    ID3D12CommandQueue* directQueue  = upscaler.engine().directQueue();

    // 3. Command Allocator and List
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> directAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> directCmd;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(directAlloc.GetAddressOf()));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, directAlloc.Get(), nullptr, IID_PPV_ARGS(directCmd.GetAddressOf()));
    directCmd->Close();

    uint32_t capWidth = GetSystemMetrics(SM_CXSCREEN);
    uint32_t capHeight = GetSystemMetrics(SM_CYSCREEN);

    // 5. Initialize Display Overlay Window
    uint32_t targetWidth  = capWidth;
    uint32_t targetHeight = capHeight;
    if (scaleFactor > 1) {
        targetWidth  = capWidth * scaleFactor;
        targetHeight = capHeight * scaleFactor;
    }

    // EmojiWidget disabled per user request

    OverlayWindow overlay;
    std::cout << "[Display] Creating borderless topmost overlay...\n";
    if (!overlay.Create("FSR-NG-Overlay", capWidth, capHeight)) {
        std::cerr << "[Display] ERROR: Failed to create overlay window.\n";
        MessageBoxA(nullptr, "Fatal Error. Please run from terminal to see the logs.", "FSR-NG Error", MB_ICONERROR); return 1;
    }

    float initialScale = config.GetFloat("desktop_scale", 1.0f);
    float initialOutScale = config.GetFloat("output_scale", 1.0f);
    int initialOutW = static_cast<int>(capWidth * initialOutScale);
    int initialOutH = static_cast<int>(capHeight * initialOutScale);

    // 6. Initialize SwapChain Presenter (DXGI Flip Discard)
    SwapchainPresenter presenter;
    if (!presenter.Initialize(overlay.hwnd(), device, directQueue, initialOutW, initialOutH)) {
        std::cerr << "[Display] ERROR: " << presenter.error() << std::endl;
        MessageBoxA(nullptr, "Fatal Error. Please run from terminal to see the logs.", "FSR-NG Error", MB_ICONERROR); return 1;
    }

    // Allocate neural upscaler resources
    upscaler.Resize(capWidth, capHeight, initialOutW, initialOutH, presenter.format(), initialScale);
    upscaler.params.intensity = intensity;
    upscaler.params.splitScreen = splitScreen;
    upscaler.params.resetHistory = 1.0f;

    // Start with overlay in full click-through mode
    overlay.SetClickThrough(true);

    // 7. Register Global Hotkeys
    HotkeyManager hotkeys(overlay.hwnd());
    overlay.SetHotKeyCallback([&](WPARAM w, LPARAM l) {
        hotkeys.OnHotKey(w, l);
    });
    std::atomic<bool> scalingActive{ false }; // Disabled by default so mouse works
    std::atomic<bool> windowModeActive{ captureMode == "window" }; // Re-enabled window mode
    std::atomic<bool> menuModeActive{ false }; // False = Game Mode (Click-through)
    HWND lastForegroundHwnd = nullptr;
    std::string currentTargetTitle = "Desktop";

    // Initialize as Click-Through (Game Mode)
    overlay.SetClickThrough(true);

    // Ctrl+Alt+M: Toggle Menu Mode (Interact with OptiScaler GUI)
    hotkeys.Register('M', HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        menuModeActive = !menuModeActive.load();
        overlay.SetClickThrough(!menuModeActive.load());
        
        if (menuModeActive.load()) {
            std::cout << "\n[Hotkey] MENU MODE: Mouse unlocked for OptiScaler GUI. (Game clicks BLOCKED)" << std::endl;
            // Also force overlay to be visible so they can see the menu
            if (!scalingActive.load()) {
                scalingActive = true;
                overlay.Show(true);
            }
        } else {
            std::cout << "\n[Hotkey] GAME MODE: Mouse click-through ENABLED. (GUI will be invisible)" << std::endl;
        }
    });

    // Ctrl+Alt+S: Toggle live scaling
    hotkeys.Register(toggleKey, HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        scalingActive = !scalingActive.load();
        overlay.Show(scalingActive.load());
        upscaler.ResetHistory();
        std::cout << "\n[Hotkey] Scaling toggled: " << (scalingActive.load() ? "ENABLED (Visible)" : "DISABLED (Hidden)") << std::endl;
    });

    // Ctrl+Alt+W: Toggle capture mode (Window / Desktop)
    hotkeys.Register(toggleModeKey, HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        windowModeActive = !windowModeActive.load();
        upscaler.ResetHistory();
        std::cout << "\n[Hotkey] Capture mode toggled to: " << (windowModeActive.load() ? "WINDOW" : "DESKTOP") << std::endl;
    });

    // Ctrl+Alt+R: Live reload settings.ini
    hotkeys.Register(reloadKey, HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        config.Reload();
        upscaler.params.intensity             = config.GetFloat("intensity", 1.00f);
        upscaler.params.splitScreen           = config.GetFloat("debug_split_screen", 0.0f);
        std::string newMode = config.GetString("capture_mode", "desktop");
        windowModeActive = (newMode == "window");
        float desktopScale = config.GetFloat("desktop_scale", 1.0f);
        float rOutScale = config.GetFloat("output_scale", 1.0f);
        upscaler.Resize(upscaler.inWidth(), upscaler.inHeight(), static_cast<int>(GetSystemMetrics(SM_CXSCREEN) * rOutScale), static_cast<int>(GetSystemMetrics(SM_CYSCREEN) * rOutScale), presenter.format(), desktopScale);
        upscaler.ResetHistory();
        std::cout << "\n[Hotkey] Settings reloaded live from settings.ini:"
                  << " Intensity=" << upscaler.params.intensity
                  << " Mode=" << (windowModeActive.load() ? "Window" : "Desktop")
                  << " SplitScreen=" << upscaler.params.splitScreen
                  << " DesktopScale=" << desktopScale << std::endl;
    });

    overlay.Show(false);
    std::cout << "\n=========================================================\n"
              << "  FSR-NG Active! (STARTED HIDDEN)\n"
              << "  Controls:\n"
              << "  * [Ctrl + Alt + S] : Toggle Scaling Overlay On/Off\n"
              << "  * [Ctrl + Alt + M] : Toggle Menu Mode (Mouse) / Game Mode\n"
              << "  * [Ctrl + C]       : Exit application\n"
              << "=========================================================\n\n";


    // 4. Initialize Capture Engine via DXGI Output Duplication
    CaptureManager capture;
    std::cout << "[Capture] Initializing DXGI Desktop Duplication...\n";
    if (!capture.Initialize(device)) {
        std::cerr << "[Capture] ERROR: " << capture.error() << std::endl;
        MessageBoxA(nullptr, "Fatal Error. Please run from terminal to see the logs.", "FSR-NG Error", MB_ICONERROR); return 1;
    }

    if (!capture.Start(nullptr)) { // Full primary desktop capture
        std::cerr << "[Capture] ERROR starting capture: " << capture.error() << std::endl;
        MessageBoxA(nullptr, "Fatal Error. Please run from terminal to see the logs.", "FSR-NG Error", MB_ICONERROR); return 1;
    }

    capWidth  = capture.width();
    capHeight = capture.height();
    std::cout << "[Capture] Capture active: " << capWidth << "x" << capHeight << " (DXGI VRAM Direct)\n";


    // 8. Main Render Loop
    auto lastFpsTime = std::chrono::steady_clock::now();
    uint64_t frameCounter = 0;
    uint64_t totalFramesRendered = 0;
    float currentFps = 0.0f;

    while (g_running.load()) {
        //emoji.Update();
        if (!overlay.ProcessMessages()) {
            break; // WM_QUIT
        }

        if (!scalingActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // A. Handle Dynamic Foreground Window Crop
        int currentInputW = capWidth;
        int currentInputH = capHeight;
        int currentX = 0;
        int currentY = 0;

        if (windowModeActive.load()) {
            WindowClientInfo wInfo = capture.GetForegroundClientArea(overlay.hwnd());
            if (wInfo.valid) {
                currentInputW = std::clamp<int>(static_cast<int>(wInfo.width), 16, capWidth);
                currentInputH = std::clamp<int>(static_cast<int>(wInfo.height), 16, capHeight);
                currentX = wInfo.x;
                currentY = wInfo.y;

                if (wInfo.hwnd != lastForegroundHwnd) {
                    lastForegroundHwnd = wInfo.hwnd;
                    currentTargetTitle = wInfo.title.empty() ? "Target Window" : wInfo.title;
                    upscaler.ResetHistory();
                    std::cout << "\n[Target Window] Active: \"" << currentTargetTitle
                              << "\" (" << currentInputW << "x" << currentInputH << ")" << std::endl;
                }
            }
        } else {
            lastForegroundHwnd = nullptr;
        }

        float outputScale = config.GetFloat("output_scale", 1.0f);
    int currentOutputW = static_cast<int>(GetSystemMetrics(SM_CXSCREEN) * outputScale);
        int currentOutputH = static_cast<int>(GetSystemMetrics(SM_CYSCREEN) * outputScale);
    if (currentOutputW < 16) currentOutputW = 16;
    if (currentOutputH < 16) currentOutputH = 16;
        int overlayX = 0;
        int overlayY = 0;

        if (!windowModeActive.load()) {
            currentInputW = capWidth;
            currentInputH = capHeight;
            currentX = 0;
            currentY = 0;
        }

        if (currentInputW != upscaler.inWidth() || currentInputH != upscaler.inHeight() || currentOutputW != upscaler.outWidth() || currentOutputW != presenter.width() || currentOutputH != presenter.height()) {
            float desktopScale = config.GetFloat("desktop_scale", 1.0f);
            upscaler.Resize(currentInputW, currentInputH, currentOutputW, currentOutputH, presenter.format(), desktopScale);
            overlay.SetPositionAndSize(overlayX, overlayY, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
            presenter.Resize(currentOutputW, currentOutputH);
        } else if (windowModeActive.load()) {
            overlay.SetPositionAndSize(overlayX, overlayY, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        }

        // B. Acquire frame from GPU VRAM
        bool newFrame = false;
        ID3D12Resource* inputFrame = capture.AcquireLatestFrame(&newFrame);
        if (!inputFrame || !newFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // C. Execute Neural Upscaler Compute Pass
        directAlloc->Reset();
        directCmd->Reset(directAlloc.Get(), nullptr);

        if (capture.GetD3D12KeyedMutex()) capture.GetD3D12KeyedMutex()->AcquireSync(0, INFINITE);
        upscaler.Process(directCmd.Get(), inputFrame, currentX, currentY);
        if (capture.GetD3D12KeyedMutex()) capture.GetD3D12KeyedMutex()->ReleaseSync(0);

        directCmd->Close();
        ID3D12CommandList* lists[] = { directCmd.Get() };
        directQueue->ExecuteCommandLists(1, lists);

        totalFramesRendered++;

        // D. Present via Flip Discard SwapChain
        if (upscaler.output()) {
            presenter.Present(upscaler.output(), nullptr, true);
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
                      << " | Menu: OptiScaler Native"
                      << std::flush;
        }

        // FPS Limiter
        static auto lastRenderTime = std::chrono::steady_clock::now();
        auto currentRenderTime = std::chrono::steady_clock::now();
        std::chrono::duration<double, std::milli> frameDuration = currentRenderTime - lastRenderTime;
        
        float fpsLimit = config.GetFloat("fps_limit", 30.0f);
        if (fpsLimit > 0.0f) {
            double targetDuration = 1000.0 / fpsLimit;
            if (frameDuration.count() < targetDuration) {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(targetDuration - frameDuration.count())));
            }
        }
        lastRenderTime = std::chrono::steady_clock::now();
    }

    std::cout << "\n[FSR-NG] Cleaning up and shutting down gracefully...\n";
    overlay.Show(false);
    capture.Stop();

    std::cout << "[FSR-NG] Terminated successfully.\n";
    return 0;
}
