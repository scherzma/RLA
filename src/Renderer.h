#pragma once

#include "Types.h"

#include <d3d11.h>
#include <string>
#include <memory>

struct ImGuiContext;

namespace RLA {

class Renderer {
public:
    Renderer();
    ~Renderer();

    // Initialize DirectX 11 and ImGui
    bool Initialize(HWND hwnd, int width, int height);

    // Cleanup resources
    void Shutdown();

    // Begin/End frame
    void BeginFrame();
    void EndFrame();

    // Handle window resize
    void OnResize(int width, int height);

    // Check if initialized
    bool IsInitialized() const { return initialized_; }

    // Get D3D device (for advanced use)
    ID3D11Device* GetDevice() const { return device_; }

private:
    friend struct RendererRegressionAccess;
    bool CreateDeviceAndSwapChain(HWND hwnd, int width, int height);
    bool CreateRenderTarget();
    void CleanupRenderTarget();
    bool InitializeImGui(HWND hwnd);

    HWND hwnd_ = nullptr;
    bool initialized_ = false;

    // DirectX 11 resources
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* deviceContext_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* renderTargetView_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int pendingWidth_ = 0, pendingHeight_ = 0;
    HRESULT lastPresent_ = S_OK;
};

} // namespace RLA
