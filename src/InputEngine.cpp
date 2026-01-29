#include "InputEngine.h"

#include <stdexcept>

namespace RLA {

// Static pointer for WndProc to access the instance
static InputEngine* g_inputEngineInstance = nullptr;

InputEngine::InputEngine() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

InputEngine::~InputEngine() {
    Stop();

    // Signal thread to stop
    shouldStop_ = true;

    // Post quit message to the input thread's message loop
    if (inputHwnd_) {
        PostMessage(inputHwnd_, WM_QUIT, 0, 0);
    }

    // Wait for thread to finish
    if (inputThread_.joinable()) {
        inputThread_.join();
    }

    g_inputEngineInstance = nullptr;
}

bool InputEngine::Initialize() {
    if (initialized_) return true;

    g_inputEngineInstance = this;

    // Start the input thread - it will create its own window and message loop
    inputThread_ = std::thread(&InputEngine::InputThreadFunc, this);

    // Wait for the thread to be ready (window created and raw input registered)
    while (!threadReady_.load() && !shouldStop_.load()) {
        Sleep(1);
    }

    if (shouldStop_) {
        return false;
    }

    initialized_ = true;
    return true;
}

LRESULT CALLBACK InputEngine::InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INPUT:
            if (g_inputEngineInstance) {
                g_inputEngineInstance->ProcessRawInput(lParam);
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

void InputEngine::InputThreadFunc() {
    // Set thread priority for time-critical input handling
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Register window class for the hidden input window
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = InputWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"RLA_InputWindowClass";

    if (!RegisterClassExW(&wc)) {
        // Class might already be registered, that's okay
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            shouldStop_ = true;
            threadReady_ = true;
            return;
        }
    }

    // Create a message-only window (HWND_MESSAGE parent means it's invisible and doesn't appear in taskbar)
    inputHwnd_ = CreateWindowExW(
        0,
        L"RLA_InputWindowClass",
        L"RLA Input",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,  // Message-only window
        nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (!inputHwnd_) {
        shouldStop_ = true;
        threadReady_ = true;
        return;
    }

    // Register for raw input on this window
    if (!RegisterRawInput(inputHwnd_)) {
        DestroyWindow(inputHwnd_);
        inputHwnd_ = nullptr;
        shouldStop_ = true;
        threadReady_ = true;
        return;
    }

    // Signal that we're ready
    threadReady_ = true;

    // Run the message loop for this thread
    // This loop is NOT blocked by VSync - it runs as fast as messages arrive
    MSG msg{};
    while (!shouldStop_.load()) {
        // Use GetMessage for efficient waiting (doesn't spin CPU)
        // But check for quit periodically
        BOOL result = GetMessage(&msg, nullptr, 0, 0);

        if (result == 0) {
            // WM_QUIT received
            break;
        }
        else if (result == -1) {
            // Error
            break;
        }
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // Cleanup
    UnregisterRawInput();
    if (inputHwnd_) {
        DestroyWindow(inputHwnd_);
        inputHwnd_ = nullptr;
    }
}

bool InputEngine::RegisterRawInput(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = RIDEV_INPUTSINK; // Receive input even when not focused
    rid.hwndTarget = hwnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        return false;
    }

    return true;
}

void InputEngine::UnregisterRawInput() {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;

    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

bool InputEngine::Start() {
    if (!initialized_) return false;
    if (running_) return true;

    running_ = true;
    return true;
}

void InputEngine::Stop() {
    running_ = false;
}

void InputEngine::ProcessRawInput(LPARAM lParam) {
    if (!running_) return;

    // Get timestamp IMMEDIATELY - this is the key improvement!
    // Since we're on a dedicated thread not blocked by VSync,
    // this timestamp is as close to the actual event time as possible.
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);

    // Use stack-allocated buffer - RAWINPUT for mouse is always the same size
    // This avoids heap allocation overhead which was limiting throughput to ~2600 Hz
    alignas(8) BYTE buffer[sizeof(RAWINPUT)];
    UINT size = sizeof(buffer);

    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
        return;
    }

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer);

    if (raw->header.dwType != RIM_TYPEMOUSE) {
        return;
    }

    MouseEvent event{};
    event.deviceHandle = raw->header.hDevice;
    event.timestamp = timestamp.QuadPart;
    event.deltaX = raw->data.mouse.lLastX;
    event.deltaY = raw->data.mouse.lLastY;

    // Only push if there's actual movement
    if (event.deltaX != 0 || event.deltaY != 0) {
        eventBuffer_.push(event);
    }
}

size_t InputEngine::ProcessEvents(const EventCallback& callback) {
    size_t processed = 0;

    while (auto event = eventBuffer_.pop()) {
        callback(*event);
        ++processed;
    }

    return processed;
}

float InputEngine::GetBufferUtilization() const {
    return static_cast<float>(eventBuffer_.size()) / static_cast<float>(eventBuffer_.capacity());
}

void InputEngine::UpdateEventRates(size_t eventsA, size_t eventsB) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (lastRateUpdateTime_ == 0) {
        lastRateUpdateTime_ = now.QuadPart;
        lastEventCountA_ = eventsA;
        lastEventCountB_ = eventsB;
        return;
    }

    double elapsedSeconds = static_cast<double>(now.QuadPart - lastRateUpdateTime_) / qpcFrequency_;

    // Update rate every 100ms for smoother display
    if (elapsedSeconds >= 0.1) {
        size_t deltaA = eventsA - lastEventCountA_;
        size_t deltaB = eventsB - lastEventCountB_;

        eventRateA_ = deltaA / elapsedSeconds;
        eventRateB_ = deltaB / elapsedSeconds;

        lastRateUpdateTime_ = now.QuadPart;
        lastEventCountA_ = eventsA;
        lastEventCountB_ = eventsB;
    }
}

} // namespace RLA
