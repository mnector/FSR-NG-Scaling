#include "capture_manager.h"
#include <iostream>
#include <string>

namespace fsrng {

CaptureManager::CaptureManager() {}

CaptureManager::~CaptureManager() { 
    Stop(); 
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

bool CaptureManager::Initialize(ID3D12Device* d3d12Device, ID3D12CommandQueue* commandQueue, uint32_t captureWidth, uint32_t captureHeight) {
    d3d12Device_ = d3d12Device;
    commandQueue_ = commandQueue;
    width_ = captureWidth;
    height_ = captureHeight;

    if (FAILED(d3d12Device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAlloc_)))) return false;
    if (FAILED(d3d12Device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAlloc_.Get(), nullptr, IID_PPV_ARGS(&commandList_)))) return false;
    commandList_->Close();

    if (FAILED(d3d12Device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
    fenceEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Create D3D12 Texture
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = width_;
    texDesc.Height = height_;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(d3d12Device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&texture_)))) return false;

    // Create Upload Buffer
    UINT64 uploadBufferSize = 0;
    d3d12Device_->GetCopyableFootprints(&texDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);

    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadBufferSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(d3d12Device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer_)))) return false;

    // Create GDI DIB Section
    screenDC_ = GetDC(NULL);
    memDC_ = CreateCompatibleDC(screenDC_);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width_;
    bmi.bmiHeader.biHeight = -static_cast<int>(height_); // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    bitmap_ = CreateDIBSection(memDC_, &bmi, DIB_RGB_COLORS, &bitmapData_, NULL, 0);
    if (!bitmap_) return false;

    SelectObject(memDC_, bitmap_);

    return true;
}

bool CaptureManager::Start(HWND targetWindow) {
    return true;
}

void CaptureManager::Stop() {
    WaitForGPU();
    if (memDC_) { DeleteDC(memDC_); memDC_ = nullptr; }
    if (screenDC_) { ReleaseDC(NULL, screenDC_); screenDC_ = nullptr; }
    if (bitmap_) { DeleteObject(bitmap_); bitmap_ = nullptr; }
}

void CaptureManager::WaitForGPU() {
    if (commandQueue_ && fence_ && fenceEvent_) {
        fenceValue_++;
        commandQueue_->Signal(fence_.Get(), fenceValue_);
        if (fence_->GetCompletedValue() < fenceValue_) {
            fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }
}

ID3D12Resource* CaptureManager::AcquireLatestFrame(bool* newFrame) {
    if (newFrame) *newFrame = true;

    // Capture screen via GDI
    SetStretchBltMode(memDC_, HALFTONE);
    StretchBlt(memDC_, 0, 0, width_, height_, screenDC_, 0, 0, GetDeviceCaps(screenDC_, DESKTOPHORZRES), GetDeviceCaps(screenDC_, DESKTOPVERTRES), SRCCOPY);

    // Map upload buffer
    void* mappedData = nullptr;
    if (FAILED(uploadBuffer_->Map(0, nullptr, &mappedData))) return nullptr;

    // Copy row by row to handle row pitch
    D3D12_RESOURCE_DESC texDesc = texture_->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows;
    UINT64 rowSize, totalBytes;
    d3d12Device_->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

    const BYTE* src = static_cast<const BYTE*>(bitmapData_);
    BYTE* dst = static_cast<BYTE*>(mappedData);

    for (UINT y = 0; y < height_; ++y) {
        memcpy(dst + y * footprint.Footprint.RowPitch, src + y * width_ * 4, width_ * 4);
    }
    uploadBuffer_->Unmap(0, nullptr);

    // Record GPU commands
    commandAlloc_->Reset();
    commandList_->Reset(commandAlloc_.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture_.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList_->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = texture_.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer_.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    commandList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    commandList_->ResourceBarrier(1, &barrier);

    commandList_->Close();

    // Execute
    ID3D12CommandList* lists[] = { commandList_.Get() };
    commandQueue_->ExecuteCommandLists(1, lists);
    
    // We must wait for the upload to complete before modifying the upload buffer again on the next frame
    WaitForGPU();

    return texture_.Get();
}

WindowClientInfo CaptureManager::GetForegroundClientArea(HWND excludeHwnd) {
    WindowClientInfo info{};
    HWND fg = GetForegroundWindow();
    if (!fg || fg == excludeHwnd || fg == GetDesktopWindow() || fg == GetShellWindow()) return info;

    RECT clientRect;
    if (GetClientRect(fg, &clientRect)) {
        POINT pt = { 0, 0 };
        ClientToScreen(fg, &pt);
        info.valid = true;
        info.x = pt.x;
        info.y = pt.y;
        info.width = clientRect.right - clientRect.left;
        info.height = clientRect.bottom - clientRect.top;
        
        char title[256];
        if (GetWindowTextA(fg, title, sizeof(title))) {
            info.title = title;
        }
        info.hwnd = fg;
    }
    return info;
}

}
