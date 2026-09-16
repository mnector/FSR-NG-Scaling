#include <windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <iomanip>
#include <algorithm>
#include <vector>

#include "capture/capture_manager.h"
#include "depth/depth_worker.h"
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

    std::cout << "=========================================================" << std::endl;
    std::cout << "  FSR-NG-Scaling: Lossless Neural Scaler for Windows 11   " << std::endl;
    std::cout << "  DirectX 12 / DLSS Native Desktop Scaling Engine        " << std::endl;
    std::cout << "=========================================================\n" << std::endl;

    // 1. Load Configuration
    ConfigReader config("config/settings.ini");
    float splitScreen = config.GetFloat("debug_split_screen", 0.0f);
    int toggleKey     = config.GetInt("toggle_key", 83); // 'S'
    int reloadKey     = config.GetInt("reload_key", 82); // 'R'
    int fpsLimit      = config.GetInt("fps_limit", 30);
    bool depthEnabled = config.GetBool("enabled", false);
    std::string depthModelPath = config.GetString("model_path", "models/depth/depth_anything_v2_vits.onnx");
    std::string depthProvider = config.GetString("provider", "CUDA");

    std::cout << "[Config] Split Screen: " << (splitScreen > 0.5f ? "ON" : "OFF") << std::endl;
    std::cout << "[Config] Depth Estimation: " << (depthEnabled ? "ON" : "OFF") << std::endl;

    // 2. Initialize Neural Upscaler (isolated D3D12 device & compute queue)
    NeuralUpscaler upscaler;
    std::cout << "\n[Engine] Initializing DirectX 12 Compute Pipeline..." << std::endl;
    if (!upscaler.Initialize()) {
        std::cerr << "[Engine] ERROR: " << upscaler.error() << std::endl;
        // Bypassed
    }
    std::cout << "[Engine] D3D12 Compute Pipeline ready." << std::endl;

    // 2b. Initialize Depth Estimation (optional)
    DepthWorker depthWorker;
    std::atomic<int> depthVisMode{ 0 };
    Microsoft::WRL::ComPtr<ID3D12Resource> visUploadBuffer;

    ID3D12Device* device = upscaler.device();
    ID3D12CommandQueue* directQueue  = upscaler.directQueue();

    // 3. Command Allocator and List
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> directAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> directCmd;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(directAlloc.GetAddressOf()));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, directAlloc.Get(), nullptr, IID_PPV_ARGS(directCmd.GetAddressOf()));
    directCmd->Close();

    uint32_t capWidth = GetSystemMetrics(SM_CXSCREEN);
    uint32_t capHeight = GetSystemMetrics(SM_CYSCREEN);

    // 4. Initialize Display Overlay Window
    OverlayWindow overlay;
    std::cout << "[Display] Creating borderless topmost overlay..." << std::endl;
    if (!overlay.Create("FSR-NG-Overlay", capWidth, capHeight)) {
        std::cerr << "[Display] ERROR: Failed to create overlay window." << std::endl;
        // Bypassed
    }

    auto calculateScales = [&](int& dlssInputW, int& dlssInputH) {
        float dpiScaleFactor = 1.0f;
        if (config.GetBool("auto_dpi_scale", true)) {
            UINT dpi = GetDpiForSystem();
            if (dpi > 0) dpiScaleFactor = 96.0f / static_cast<float>(dpi);
        }

        std::string dlssMode = config.GetString("dlss_mode", "auto");
        std::transform(dlssMode.begin(), dlssMode.end(), dlssMode.begin(), ::tolower);
        
        float dlssScale = 1.0f;
        if (dlssMode == "quality") dlssScale = 0.666f;
        else if (dlssMode == "balanced") dlssScale = 0.580f;
        else if (dlssMode == "performance") dlssScale = 0.500f;
        else if (dlssMode == "ultra_performance") dlssScale = 0.333f;
        else if (dlssMode == "auto") {
            dlssScale = (capWidth >= 3840) ? 0.5f : 0.666f;
        }

        float finalInputScale = dpiScaleFactor * dlssScale;
        dlssInputW = static_cast<int>(capWidth * finalInputScale);
        dlssInputH = static_cast<int>(capHeight * finalInputScale);
        
        std::cout << "[Scaling] DLSS Mode: " << dlssMode << " | DPI Scale: " << dpiScaleFactor << " | DLSS Input: " << dlssInputW << "x" << dlssInputH << std::endl;
    };

    int dlssInputW, dlssInputH;
    calculateScales(dlssInputW, dlssInputH);

    // 5. Initialize SwapChain Presenter (DXGI Flip Discard)
    
    SwapchainPresenter presenter;
    
    if (!presenter.Initialize(overlay.hwnd(), device, directQueue, capWidth, capHeight)) {
        std::cerr << "[Display] ERROR: " << presenter.error() << std::endl;
        // Bypassed
    }

    // Allocate neural upscaler resources
    
    upscaler.Resize(dlssInputW, dlssInputH, capWidth, capHeight, presenter.format(), 1.0f);
    
    upscaler.params.splitScreen = splitScreen;
    upscaler.params.resetHistory = 1.0f;

    // Start with overlay in full click-through mode
    
    overlay.SetClickThrough(true);

    // 6. Register Global Hotkeys
    
    HotkeyManager hotkeys(overlay.hwnd());
    overlay.SetHotKeyCallback([&](WPARAM w, LPARAM l) { hotkeys.OnHotKey(w, l); });
    std::atomic<bool> scalingActive{ false };
    std::atomic<bool> menuModeActive{ false };

    
    hotkeys.Register('M', HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        menuModeActive = !menuModeActive.load();
        overlay.SetClickThrough(!menuModeActive.load());
        
        if (menuModeActive.load()) {
            std::cout << "\n[Hotkey] MENU MODE: Mouse unlocked for OptiScaler GUI. (Game clicks BLOCKED)" << std::endl;
            if (!scalingActive.load()) {
                scalingActive = true;
                overlay.Show(true);
            }
        } else {
            std::cout << "\n[Hotkey] GAME MODE: Mouse click-through ENABLED. (GUI clicks ignored)" << std::endl;
        }
    });

    
    hotkeys.Register(toggleKey, HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        scalingActive = !scalingActive.load();
        overlay.Show(scalingActive.load());
        upscaler.ResetHistory();
        std::cout << "\n[Hotkey] Scaling toggled: " << (scalingActive.load() ? "ENABLED (Visible)" : "DISABLED (Hidden)") << std::endl;
    });

    hotkeys.Register('D', HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        int mode = depthVisMode.load();
        mode = (mode + 1) % 3;
        depthVisMode.store(mode);
        std::cout << "\n[Hotkey] Depth Visualization toggled: " << (mode == 0 ? "OFF" : (mode == 1 ? "SIMPLE" : "FULL")) << std::endl;
    });

    hotkeys.Register(reloadKey, HotkeyManager::MOD_CTRL_KEY | HotkeyManager::MOD_ALT_KEY, [&]() {
        config.Reload();
        upscaler.params.splitScreen = config.GetFloat("debug_split_screen", 0.0f);
        fpsLimit = config.GetInt("fps_limit", 30);
        
        int newInW, newInH;
        calculateScales(newInW, newInH);
        
        if (newInW != upscaler.inWidth() || newInH != upscaler.inHeight()) {
            std::cout << "[Hotkey] Requires restart to change capture resolution. (DLSS Input resizing at runtime is not supported)." << std::endl;
        }
        
        std::cout << "\n[Hotkey] Settings reloaded live." << std::endl;
    });

    
    overlay.Show(false);
    std::cout << "\n=========================================================\n"
              << "  FSR-NG Active! (STARTED HIDDEN)\n"
              << "  Controls:\n"
              << "  * [Ctrl + Alt + S] : Toggle Scaling Overlay On/Off\n"
              << "  * [Ctrl + Alt + M] : Toggle Menu Mode (Mouse) / Game Mode\n"
              << "  * [Ctrl + C]       : Exit application\n"
              << "=========================================================\n" << std::endl;

    // 7. Initialize Capture Engine via DXGI Output Duplication
    
    
    CaptureManager capture;
    std::cout << "[Capture] Initializing DXGI Desktop Duplication..." << std::endl; 
    
    if (!capture.Initialize(device, directQueue, dlssInputW, dlssInputH)) {
        std::cerr << "[Capture] ERROR: " << capture.error() << std::endl;
        // Bypassed
    }

    if (!capture.Start(nullptr)) { // Full primary desktop capture
        std::cerr << "[Capture] ERROR starting capture: " << capture.error() << std::endl;
        // Bypassed
    }

    std::cout << "[Capture] Capture active: " << capture.width() << "x" << capture.height() << " (DXGI VRAM Direct)" << std::endl;

    if (depthEnabled) {
        std::cout << "\n[Depth] Initializing DepthWorker (Async ONNX Pipeline)..." << std::endl;
        if (!depthWorker.Initialize(device, directQueue, dlssInputW, dlssInputH, std::wstring(depthModelPath.begin(), depthModelPath.end()), std::wstring(depthProvider.begin(), depthProvider.end()))) {
            std::cerr << "[Depth] ERROR: Failed to initialize DepthWorker." << std::endl;
            depthEnabled = false;
        } else {
            std::cout << "[Depth] " << depthWorker.GetVersion() << " ready." << std::endl;
        }
    }

    if (depthEnabled) {
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufDesc = {};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uint32_t rowPitch = (518 * 4 + 255) & ~255;
        bufDesc.Width = rowPitch * 518;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&visUploadBuffer));
    }

    // 8. Main Render Loop
    auto lastFpsTime = std::chrono::steady_clock::now();
    uint64_t frameCounter = 0;
    float currentFps = 0.0f;

    while (g_running.load()) {
        if (!overlay.ProcessMessages()) break;

        if (!scalingActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        auto frameStart = std::chrono::steady_clock::now();

        bool newFrame = false;
        ID3D12Resource* inputFrame = capture.AcquireLatestFrame(&newFrame);
        if (!inputFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        IDXGIKeyedMutex* keyedMutex = capture.GetD3D12KeyedMutex();
        if (keyedMutex) keyedMutex->AcquireSync(0, 50);

        directAlloc->Reset();
        directCmd->Reset(directAlloc.Get(), nullptr);

        // 3. Process Frame (depth estimation - ONNX via Async Worker)
        if (depthEnabled) {
            depthWorker.Update(directCmd.Get(), inputFrame);
            const auto depthMap = depthWorker.GetLatestDepthMap();
            upscaler.Process(directCmd.Get(), inputFrame, 0, 0, depthMap.data());
        } else {
            upscaler.Process(directCmd.Get(), inputFrame, 0, 0);
        }

        // 4. Depth Visualization
        int visMode = depthVisMode.load();
        if (depthEnabled && visMode != 0 && visUploadBuffer) {
            const auto visPixels = depthWorker.GetLatestVisPixels();
            void* pData = nullptr;
            if (SUCCEEDED(visUploadBuffer->Map(0, nullptr, &pData))) {
                uint32_t rowPitch = (518 * 4 + 255) & ~255;
                for (uint32_t y = 0; y < 518; ++y) {
                    memcpy(static_cast<uint8_t*>(pData) + y * rowPitch, visPixels.data() + y * 518, 518 * 4);
                }
                visUploadBuffer->Unmap(0, nullptr);
                
                D3D12_RESOURCE_DESC outDesc = upscaler.output()->GetDesc();
                
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = upscaler.output();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = visUploadBuffer.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Footprint.Format = outDesc.Format;
                src.PlacedFootprint.Footprint.Width = 518;
                src.PlacedFootprint.Footprint.Height = 518;
                src.PlacedFootprint.Footprint.Depth = 1;
                src.PlacedFootprint.Footprint.RowPitch = rowPitch;
                
                uint32_t destX = (visMode == 1) ? 0 : (outDesc.Width / 2 - 518 / 2);
                uint32_t destY = (visMode == 1) ? 0 : (outDesc.Height / 2 - 518 / 2);
                
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = upscaler.output();
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                directCmd->ResourceBarrier(1, &barrier);

                directCmd->CopyTextureRegion(&dst, destX, destY, 0, &src, nullptr);
                
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                directCmd->ResourceBarrier(1, &barrier);
            }
        }

        directCmd->Close();
        ID3D12CommandList* lists[] = { directCmd.Get() };
        directQueue->ExecuteCommandLists(1, lists);

        if (depthEnabled) {
            depthWorker.PostSubmit(directQueue);
        }

        if (keyedMutex) keyedMutex->ReleaseSync(0);

        if (upscaler.output()) {
            presenter.Present(upscaler.output(), nullptr, false);
        }

        presenter.WaitForGpu();

        frameCounter++;
        // Continuous Z-order heartbeat: reaffirm HWND_TOPMOST so no background or activated windows stick out
        if (frameCounter % 15 == 0) {
            overlay.BringToTop();
        }

        auto now = std::chrono::steady_clock::now();
        std::chrono::duration<float> elapsed = now - lastFpsTime;
        if (elapsed.count() >= 1.0f) {
            currentFps = frameCounter / elapsed.count();
            frameCounter = 0;
            lastFpsTime = now;
        }

        if (fpsLimit > 0) {
            float targetFrameTimeMs = 1000.0f / fpsLimit;
            auto frameEnd = std::chrono::steady_clock::now();
            std::chrono::duration<float, std::milli> frameElapsed = frameEnd - frameStart;
            if (frameElapsed.count() < targetFrameTimeMs) {
                int sleepTime = static_cast<int>(targetFrameTimeMs - frameElapsed.count());
                if (sleepTime > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
                }
            }
        }
    }

    std::cout << "\n[FSR-NG] Cleaning up and shutting down gracefully..." << std::endl;
    
    overlay.Show(false);
    capture.Stop();

    std::cout << "[FSR-NG] Terminated successfully." << std::endl;
    return 0;
}
