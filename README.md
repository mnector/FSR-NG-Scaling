# FSR-NG-Scaling (Lossless Neural Scaler)

Aplicación nativa para Windows 11 (C++20 / DirectX 12) diseñada para escalar y mejorar en tiempo real fotogramas de cualquier ventana o videojuego utilizando shaders HLSL de inferencia neural en GPUs AMD Radeon (RDNA / RDNA AI) y proyectar el resultado a través de un overlay sin bordes de latencia ultra-baja.

---

## 1. Principios de Arquitectura

1. **Cero Inyección de Procesos:**
   No utiliza `CreateRemoteThread`, `WriteProcessMemory`, ni wrappers de DLLs. El juego o aplicación permanece intacto.
2. **Captura Directa por GPU y Detección Dinámica de Ventana:**
   Extrae los fotogramas directamente de la VRAM mediante `IDXGIOutputDuplication`. Identifica automáticamente la ventana en primer plano (`GetForegroundWindow`) y recorta el área cliente con precisión sub-píxel eliminando barras de título y marcos de ventana.
3. **Pase Neural Aislado en DirectX 12 & Mapeo SafeTensors Profundo:**
   Ejecuta el Compute Shader en una cola de cómputo D3D12 independiente (`D3D12_COMMAND_QUEUE_TYPE_COMPUTE`). Decodifica y mapea los 153 tensores del modelo OpenNR (atención W-MSA y cascada residual multi-etapa) convirtiendo pesos FP16 a FP32.
4. **Acumulación Temporal e Histéresis ($S_{t-1}$):**
   Incorpora búfer de historia de alta resolución con supresión de ghosting basada en clamping AABB local de color ($3 \times 3$), eliminando el parpadeo y jitter sub-píxel en escenas de alta velocidad.
5. **Presentación de Ultra-Baja Latencia:**
   Overlay de pantalla completa sin bordes (`WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT`) con SwapChain DXGI en modelo Flip (`DXGI_SWAP_EFFECT_FLIP_DISCARD`) y soporte de tearing (`DXGI_PRESENT_ALLOW_TEARING`).
6. **Ajustes en Caliente y Atajos Globales:**
   Recarga instantánea de parámetros (`config/settings.ini`) y alternancia de modos en vivo sin reiniciar.

---

## 2. Weight Extraction and Packaging Pipeline

This repository **does not distribute proprietary model weights**. To prepare a model for inference, use the included inspection and extraction script:

```cmd
# Place your source binary (e.g. nvngx_dlssnr.dll) in the repository root, then run:
python inspect_and_extract.py
# (or use uv run inspect_and_extract.py)
```

The script will:
1. Parse the PE sections (`.rdata`, `.rsrc`, `.data`) to locate tensor weight payloads.
2. Identify neural layer structures (QKV projections, Swin attention blocks, convolutions).
3. Generate standard SafeTensors model files (`models/fsr_ng_model.safetensors` and `weights/model.safetensors`) aligned to 64-byte boundaries.
4. Calculate the cryptographic SHA-256 hash and update `models/model_manifest.json` and `weights/model_manifest.json`.

---

## 3. Estructura del Proyecto

```text
FSR-NG-Scaling/
├── CMakeLists.txt              # Configuración de CMake (C++20, MSVC, DX12, DXGI)
├── README.md                   # Documentación y guía de uso
├── build.bat                   # Script automatizado de compilación Release
├── inspect_and_extract.py      # Extractor e inspector de tensores DLSS-NR a SafeTensors
├── config/
│   └── settings.ini            # Parámetros en vivo (intensidad, tono, estabilidad, hotkeys)
├── models/
│   └── .gitkeep                # Directorio para modelos SafeTensors (fsr_ng_model.safetensors)
├── shaders/
│   └── neural_scale_cs.hlsl    # Compute Shader con W-MSA, W-Crop, AABB temporal y deep cascade
└── src/
    ├── main.cpp                # Bucle de eventos, coordinación, crop dinámico y telemetría FPS
    ├── capture/
    │   ├── capture_manager.h/.cpp # Captura GPU DXGI y cálculo de área cliente de ventana
    │   └── frame_pool.h           # Cola de fotogramas sincronizada
    ├── neural_engine/
    │   ├── safetensors_loader.h/.cpp # Parser C++20 seguro SafeTensors con SHA-256 y F16->F32
    │   ├── neural_upscaler.h/.cpp    # Coordinador de inferencia, empaquetado profundo y buffers UAV
    │   └── d3d12_compute_engine.h/.cpp # Dispositivo D3D12, RootSig (t0..t2, u0), Compute PSO
    ├── display/
    │   ├── overlay_window.h/.cpp     # Ventana fullscreen transparente sin bordes
    │   └── swapchain_presenter.h/.cpp# SwapChain DXGI Flip Discard
    └── utils/
        ├── config_reader.h/.cpp      # Lector / auto-recarga de settings.ini
        └── hotkey_manager.h/.cpp     # Registro y despacho de Hotkeys globales
```

---

## 4. Requisitos y Compilación

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

El ejecutable resultante se genera en `build\Release\FSR-NG-Scaling.exe` junto con las carpetas `shaders/`, `config/` y `models/`.

---

## 5. Controles y Configuración

* **`Ctrl + Alt + S`**: Alternar la escala en vivo (Activar/Desactivar Overlay).
* **`Ctrl + Alt + W`**: Alternar entre modo Ventana Activa Dinámica y modo Monitor Completo.
* **`Ctrl + Alt + R`**: Recargar `config/settings.ini` en caliente sin reiniciar la aplicación.
* **`Ctrl + C`**: Cierre limpio y liberación de recursos GPU.
