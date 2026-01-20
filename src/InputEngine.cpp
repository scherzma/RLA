#include "InputEngine.h"

#include <stdexcept>

namespace RLA {

InputEngine::InputEngine() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

InputEngine::~InputEngine() {
    Stop();
    if (initialized_) {
        UnregisterRawInput();
    }
}

bool InputEngine::Initialize(HWND hwnd) {
    if (initialized_) return true;

    hwnd_ = hwnd;

    if (!RegisterRawInput(hwnd)) {
        return false;
    }

    initialized_ = true;
    return true;
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

    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));

    if (size == 0) return;

    // Use stack allocation for small inputs, heap for larger
    std::vector<BYTE> buffer(size);
    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());

    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, raw, &size, sizeof(RAWINPUTHEADER)) != size) {
        return;
    }

    if (raw->header.dwType != RIM_TYPEMOUSE) {
        return;
    }

    // Get high-precision timestamp
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);

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

// Note: The high-priority thread approach is replaced with direct WM_INPUT processing
// which is more reliable on Windows. The thread function below is kept for reference
// but the actual input processing happens in ProcessRawInput() called from WndProc.
void InputEngine::InputThreadFunc(std::stop_token stopToken) {
    // Set thread priority for time-critical input handling
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (!stopToken.stop_requested() && running_) {
        // In the WM_INPUT model, we don't need a busy loop here
        // The main message pump handles input delivery
        Sleep(1);
    }
}

} // namespace RLA
