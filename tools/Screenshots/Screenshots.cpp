// Renders the README screenshots from the user's real library, read-only.
// Usage: RLAScreenshots.exe <output-folder> [recording-key]
// Writes library.bmp, movement.bmp, and rankings.bmp (1500 x 1000). Nothing is
// saved to the library; no mouse input is captured; no device is opened.
#include "src/App.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <ShlObj.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace RLA {
struct RendererRegressionAccess {
    static void Snapshot(Renderer& r, const std::filesystem::path& path) {
        ImGui::Render();
        r.deviceContext_->OMSetRenderTargets(1, &r.renderTargetView_, nullptr);
        const float clear[4] = {0.06f, 0.06f, 0.06f, 1};
        r.deviceContext_->ClearRenderTargetView(r.renderTargetView_, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ID3D11Texture2D* buffer = nullptr;
        r.swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer));
        D3D11_TEXTURE2D_DESC desc{};
        buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        ID3D11Texture2D* stage = nullptr;
        r.device_->CreateTexture2D(&desc, nullptr, &stage);
        r.deviceContext_->CopyResource(stage, buffer);
        D3D11_MAPPED_SUBRESOURCE map{};
        r.deviceContext_->Map(stage, 0, D3D11_MAP_READ, 0, &map);
        BITMAPFILEHEADER head{};
        head.bfType = 0x4d42; head.bfOffBits = sizeof(head) + sizeof(BITMAPINFOHEADER);
        head.bfSize = head.bfOffBits + desc.Width * desc.Height * 4;
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info); info.biWidth = desc.Width; info.biHeight = -static_cast<int>(desc.Height);
        info.biPlanes = 1; info.biBitCount = 32;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<char*>(&head), sizeof(head));
        out.write(reinterpret_cast<char*>(&info), sizeof(info));
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = static_cast<unsigned char*>(map.pData) + y * map.RowPitch;
            for (UINT x = 0; x < desc.Width; ++x) {
                const char pixel[4] = {static_cast<char>(row[x * 4 + 2]), static_cast<char>(row[x * 4 + 1]),
                                       static_cast<char>(row[x * 4]), static_cast<char>(255)};
                out.write(pixel, 4);
            }
        }
        r.deviceContext_->Unmap(stage, 0);
        stage->Release(); buffer->Release();
        if (!out) throw std::runtime_error("Could not write " + path.string());
    }
};

struct AppRegressionAccess {
    static void Run(const std::filesystem::path& output, std::string key) {
        constexpr int width = 1500, height = 1000;
        wchar_t localData[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localData)))
            throw std::runtime_error("Local application data folder not found.");
        const auto root = std::filesystem::path(localData) / L"RLA";
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"RLA_Screenshots";
        RegisterClassW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName, L"RLA screenshots", WS_POPUP, 0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) throw std::runtime_error("Could not create the screenshot window.");
        {
            App app;
            app.deviceManager_ = std::make_unique<DeviceManager>();
            app.inputEngine_ = std::make_unique<InputEngine>(); // Not started: no capture.
            app.dataStore_ = std::make_unique<DataStore>();
            app.renderer_ = std::make_unique<Renderer>();
            if (!app.renderer_->Initialize(window, width, height)) throw std::runtime_error("Renderer failed.");
            ImGui::GetIO().IniFilename = nullptr;
            std::string error;
            if (!app.mouseLibrary_.Load(root / L"mice.json", error)) throw std::runtime_error("Library: " + error);
            app.mouseLibrary_.autoRank = false; // libraryPath_ stays empty: nothing is saved.
            if (key.empty()) {
                const SavedComparison* latest = nullptr;
                for (const auto& run : app.mouseLibrary_.Comparisons())
                    if (run.enabled && run.method == LatencyMethod::Pipeline && (!latest || run.savedAt >= latest->savedAt)) latest = &run;
                if (!latest) throw std::runtime_error("No saved pipeline result.");
                key = latest->recordingKey;
            }
            const auto session = app.dataStore_->LoadFromJson(root / L"Runs" / (key + ".json"));
            if (!session) throw std::runtime_error("Recording: " + app.dataStore_->GetLastError());
            app.currentSession_ = *session;
            app.RebuildPlotData();
            app.state_ = AppState::Ready;
            app.ApplyAutoScaleB();
            app.StartLatencyAnalysis();
            app.latencyTask_.wait();
            app.PollLatencyAnalysis();
            app.rankingDirty_ = true;
            struct Shot { const char* name; bool library; int page; };
            for (const Shot shot : {Shot{"library", true, 0}, Shot{"movement", false, 0}, Shot{"rankings", true, 1}}) {
                app.showLibrary_ = shot.library; app.libraryPage_ = shot.page;
                app.fitMovementView_ = !shot.library;
                for (int frame = 0; frame < 3; ++frame) {
                    app.renderer_->BeginFrame();
                    ImGui::GetIO().DisplaySize = ImVec2(width, height);
                    app.RenderMainWindow();
                    if (frame < 2) { ImGui::Render(); continue; }
                    RendererRegressionAccess::Snapshot(*app.renderer_, output / (std::string(shot.name) + ".bmp"));
                }
            }
            std::cout << "Recording " << key << "\n"
                      << "Pipeline: " << (app.latencyFit_.valid ? std::to_string(app.latencyFit_.differenceMs) : app.latencyFit_.message) << "\n"
                      << "Data age: " << (app.dataAgeFit_.valid ? std::to_string(app.dataAgeFit_.differenceMs) : app.dataAgeFit_.message) << "\n";
            app.libraryDirty_ = false;
            app.runsToKeep_.clear();
        }
        DestroyWindow(window);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }
};
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) { std::cerr << "Usage: RLAScreenshots <output-folder> [recording-key]\n"; return 2; }
        const std::filesystem::path output(argv[1]);
        std::filesystem::create_directories(output);
        RLA::AppRegressionAccess::Run(output, argc > 2 ? argv[2] : "");
        std::cout << "Screenshots written to " << std::filesystem::absolute(output).string() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
