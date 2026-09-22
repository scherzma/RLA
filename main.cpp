// Relative Latency Analyzer (RLA)
// Measures relative input latency between two gaming mice using collision detection

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>
#include <memory>
#include <string>

#include <imgui.h>
#include "src/App.h"
#include "resource.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Global app instance
static std::unique_ptr<RLA::App> g_app;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Let ImGui handle input first
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
        // Note: WM_INPUT is now handled by InputEngine's dedicated thread
        // for accurate timestamps independent of VSync

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED && g_app) {
                int width = LOWORD(lParam);
                int height = HIWORD(lParam);
                g_app->OnResize(width, height);
            }
            return 0;

        case WM_SYSCOMMAND:
            // Disable ALT key menu activation
            if ((wParam & 0xfff0) == SC_KEYMENU) {
                return 0;
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Register window class
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_RLA),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_RLA),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"RLA_WindowClass";

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create window
    int width = 1280;
    int height = 720;

    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0,
        L"RLA_WindowClass",
        L"Relative Latency Analyzer (RLA)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create and initialize application
    g_app = std::make_unique<RLA::App>();

    if (!g_app->Initialize(hwnd, width, height)) {
        std::wstring errorMsg = L"Failed to initialize application.\n\nLast error code: " +
                                std::to_wstring(GetLastError());
        MessageBoxW(nullptr, errorMsg.c_str(), L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Show window
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Main message loop
    MSG msg{};
    while (msg.message != WM_QUIT) {
        // Process all pending messages
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT) {
            break;
        }

        // Update and render
        g_app->Update();
        g_app->Render();

        // Check if app wants to quit
        if (g_app->ShouldQuit()) {
            PostQuitMessage(0);
        }
    }

    // Cleanup
    g_app.reset();

    UnregisterClassW(L"RLA_WindowClass", hInstance);

    return static_cast<int>(msg.wParam);
}
