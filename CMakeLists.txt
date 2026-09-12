cmake_minimum_required(VERSION 3.20)
project(FSR_NG_Scaling LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags for high-performance MSVC build
if (MSVC)
    add_compile_options(
        /W4
        /O2
        /permissive-
        /EHsc
        /utf-8
        /Zc:__cplusplus
    )
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/capture/capture_manager.cpp
    src/neural_engine/d3d12_compute_engine.cpp
    src/neural_engine/neural_upscaler.cpp
    src/display/overlay_window.cpp
    src/display/swapchain_presenter.cpp
    src/utils/config_reader.cpp
    src/utils/hotkey_manager.cpp
)

set(HEADERS
    src/capture/capture_manager.h
    src/capture/frame_pool.h
    src/neural_engine/d3d12_compute_engine.h
    src/neural_engine/neural_upscaler.h
    src/display/overlay_window.h
    src/display/swapchain_presenter.h
    src/utils/config_reader.h
    src/utils/hotkey_manager.h
)

add_executable(FSR-NG-Scaling ${SOURCES} ${HEADERS})

target_include_directories(FSR-NG-Scaling PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Strictly link the required Windows/DirectX libraries
target_link_libraries(FSR-NG-Scaling PRIVATE
    d3d12.lib
    dxgi.lib
    d3dcompiler.lib
    dwmapi.lib
    gdiplus.lib
)

# Post-build: copy shader and config to output directory
add_custom_command(TARGET FSR-NG-Scaling POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:FSR-NG-Scaling>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/config/settings.ini $<TARGET_FILE_DIR:FSR-NG-Scaling>/config/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/OptiScaler.dll $<TARGET_FILE_DIR:FSR-NG-Scaling>/dxgi.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/OptiScaler.dll $<TARGET_FILE_DIR:FSR-NG-Scaling>/nvngx.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/OptiScaler.ini $<TARGET_FILE_DIR:FSR-NG-Scaling>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/dlssnr_amd_pass1.dll $<TARGET_FILE_DIR:FSR-NG-Scaling>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/dlssnr_amd_pass2.dll $<TARGET_FILE_DIR:FSR-NG-Scaling>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/dlssnr_amd_pass3.dll $<TARGET_FILE_DIR:FSR-NG-Scaling>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/dlssnr_on_amd_weights.bin $<TARGET_FILE_DIR:FSR-NG-Scaling>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/EnvyDynamicPacing.ps1 $<TARGET_FILE_DIR:FSR-NG-Scaling>/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/backend/Launch-Envy.bat $<TARGET_FILE_DIR:FSR-NG-Scaling>/
)
