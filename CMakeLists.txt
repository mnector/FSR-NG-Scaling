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
    src/neural_engine/ngx_interop.cpp
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
    src/neural_engine/ngx_interop.h
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
)

# Post-build: copy shader and config to output directory
add_custom_command(TARGET FSR-NG-Scaling POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:FSR-NG-Scaling>/shaders
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:FSR-NG-Scaling>/config
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:FSR-NG-Scaling>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/shaders/neural_scale_cs.hlsl $<TARGET_FILE_DIR:FSR-NG-Scaling>/shaders/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/config/settings.ini $<TARGET_FILE_DIR:FSR-NG-Scaling>/config/
)
