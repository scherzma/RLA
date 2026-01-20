#include "Renderer.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <implot.h>

namespace RLA {

Renderer::Renderer() = default;

Renderer::~Renderer() {
    Shutdown();
}

bool Renderer::Initialize(HWND hwnd, int width, int height) {
    if (initialized_) return true;

    hwnd_ = hwnd;
    width_ = width;
    height_ = height;

    if (!CreateDeviceAndSwapChain(hwnd, width, height)) {
        return false;
    }

    if (!CreateRenderTarget()) {
        return false;
    }

    if (!InitializeImGui(hwnd)) {
        return false;
    }

    initialized_ = true;
    return true;
}

bool Renderer::CreateDeviceAndSwapChain(HWND hwnd, int width, int height) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    // Try with debug layer first in debug builds
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags = D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,                    // Adapter
        D3D_DRIVER_TYPE_HARDWARE,   // Driver type
        nullptr,                    // Software rasterizer
        createDeviceFlags,          // Flags
        featureLevels,              // Feature levels
        ARRAYSIZE(featureLevels),   // Num feature levels
        D3D11_SDK_VERSION,          // SDK version
        &sd,                        // Swap chain desc
        &swapChain_,                // Swap chain
        &device_,                   // Device
        &featureLevel,              // Actual feature level
        &deviceContext_             // Device context
    );

    // If debug layer failed, try without it
    if (FAILED(hr) && createDeviceFlags != 0) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,  // No debug flag
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &sd,
            &swapChain_,
            &device_,
            &featureLevel,
            &deviceContext_
        );
    }

    return SUCCEEDED(hr);
}

bool Renderer::CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));

    if (FAILED(hr)) {
        return false;
    }

    hr = device_->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView_);
    backBuffer->Release();

    return SUCCEEDED(hr);
}

void Renderer::CleanupRenderTarget() {
    if (renderTargetView_) {
        renderTargetView_->Release();
        renderTargetView_ = nullptr;
    }
}

bool Renderer::InitializeImGui(HWND hwnd) {
    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Setup style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    if (!ImGui_ImplWin32_Init(hwnd)) {
        return false;
    }

    if (!ImGui_ImplDX11_Init(device_, deviceContext_)) {
        return false;
    }

    return true;
}

void Renderer::Shutdown() {
    if (!initialized_) return;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    CleanupRenderTarget();

    if (swapChain_) {
        swapChain_->Release();
        swapChain_ = nullptr;
    }

    if (deviceContext_) {
        deviceContext_->Release();
        deviceContext_ = nullptr;
    }

    if (device_) {
        device_->Release();
        device_ = nullptr;
    }

    initialized_ = false;
}

void Renderer::BeginFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Renderer::EndFrame() {
    ImGui::Render();

    const float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    deviceContext_->OMSetRenderTargets(1, &renderTargetView_, nullptr);
    deviceContext_->ClearRenderTargetView(renderTargetView_, clearColor);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    swapChain_->Present(1, 0); // VSync enabled
}

void Renderer::OnResize(int width, int height) {
    if (!initialized_ || width == 0 || height == 0) return;

    width_ = width;
    height_ = height;

    CleanupRenderTarget();

    swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);

    CreateRenderTarget();
}

} // namespace RLA
