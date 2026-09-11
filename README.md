# FSR-NG-Scaling (Lossless Neural Scaler)

Aplicación nativa para Windows 11 (C++20 / DirectX 12) diseñada para escalar y mejorar en tiempo real fotogramas de cualquier ventana o videojuego utilizando shaders HLSL de inferencia neural en GPUs AMD Radeon (RDNA / RDNA AI) y proyectar el resultado a través de un overlay sin bordes de latencia ultra-baja.

---

## 1. Principios de Arquitectura

1. **Cero Inyección de Procesos:**
   No utiliza `CreateRemoteThread`, `WriteProcessMemory`, ni wrappers de DLLs. El juego o aplicación permanece intacto.
2. **Captura Directa por GPU (DXGI Desktop Duplication):**
   Extrae los fotogramas directamente de la VRAM mediante `IDXGIOutputDuplication` con coste despreciable de CPU.
3. **Pase Neural Aislado en DirectX 12:**
   Ejecuta el Compute Shader en una cola de cómputo D3D12 independiente (`D3D12_COMMAND_QUEUE_TYPE_COMPUTE`), garantizando cero colisiones con el pipeline del juego.
4. **Presentación de Ultra-Baja Latencia:**
   Overlay de pantalla completa sin bordes (`WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT`) con SwapChain DXGI en modelo Flip (`DXGI_SWAP_EFFECT_FLIP_DISCARD`) y soporte de tearing (`DXGI_PRESENT_ALLOW_TEARING`).
5. **Ajustes en Caliente y Parser SafeTensors:**
   Carga pesos de modelos generativos con verificación de integridad SHA-256 e interactúa en vivo a través de atajos de teclado y `config/settings.ini`.

---

## 2. Estructura del Proyecto

```text
FSR-NG-Scaling/
├── CMakeLists.txt              # Configuración de CMake (C++20, MSVC, DX12, DXGI)
├── README.md                   # Documentación y guía de uso
├── build.bat                   # Script automatizado de compilación Release
├── config/
│   └── settings.ini            # Parámetros en vivo (intensidad, tono, hotkeys)
├── shaders/
│   └── neural_scale_cs.hlsl    # Compute Shader con sliders de estructura y split-screen
└── src/
    ├── main.cpp                # Bucle de eventos, coordinación y telemetría FPS
    ├── capture/
    │   ├── capture_manager.h/.cpp # Captura GPU mediante DXGI Output Duplication
    │   └── frame_pool.h           # Cola de fotogramas sincronizada
    ├── neural_engine/
    │   ├── safetensors_loader.h/.cpp # Parser C++20 seguro SafeTensors con SHA-256
    │   ├── neural_upscaler.h/.cpp    # Coordinador de inferencia y buffers UAV
    │   └── d3d12_compute_engine.h/.cpp # Dispositivo D3D12, RootSig, Compute PSO
    ├── display/
    │   ├── overlay_window.h/.cpp     # Ventana fullscreen transparente sin bordes
    │   └── swapchain_presenter.h/.cpp# SwapChain DXGI Flip Discard
    └── utils/
        ├── config_reader.h/.cpp      # Lector / auto-recarga de settings.ini
        └── hotkey_manager.h/.cpp     # Registro y despacho de Hotkeys globales
```

---

## 3. Requisitos y Compilación

* **Sistema Operativo:** Windows 11 (x64)
* **Compilador:** Microsoft Visual Studio 2022 / 2026 (MSVC C++20)
* **SDK:** Windows 10/11 SDK (10.0.19041+)
* **CMake:** Versión 3.20 o superior

### Compilación rápida:

```cmd
build.bat
```

O manualmente mediante CMake:

```cmd
cmake -B build -A x64
cmake --build build --config Release
```

El ejecutable resultante se genera en `build\Release\FSR-NG-Scaling.exe` junto con las carpetas `shaders/` y `config/`.

---

## 4. Controles y Configuración

* **`Ctrl + Alt + S`**: Alternar la escala en vivo (Activar/Desactivar Overlay).
* **`Ctrl + Alt + R`**: Recargar `config/settings.ini` en caliente sin reiniciar la aplicación.
* **`Ctrl + C`**: Cierre limpio y liberación de recursos GPU.
