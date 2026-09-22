#include "InputEngine.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>

namespace RLA {

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
}

bool InputEngine::Initialize() {
    if (initialized_) return true;

#ifndef _WIN64
    // WOW64 returns a different buffered RAWINPUT layout. Use the x64 build
    // rather than silently interpreting its headers as native 32-bit headers.
    BOOL wow64 = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &wow64) || wow64) return false;
#endif

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

    // Leave WM_INPUT in the queue for GetRawInputBuffer. GetMessage or an
    // unfiltered PeekMessage would remove packets before the buffered read.
    // RIDEV_DEVNOTIFY is deliberately not used: its messages share this queue.
    MSG msg{};
    while (!shouldStop_.load()) {
        const bool readOk = DrainRawInput();
        while (PeekMessage(&msg, nullptr, 0, WM_INPUT - 1, PM_REMOVE) ||
               PeekMessage(&msg, nullptr, WM_INPUT + 1, UINT_MAX, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                shouldStop_ = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (shouldStop_) break;
        // Retry errors without spinning a time-critical thread on a stuck queue.
        if (!readOk) Sleep(1);
        const DWORD wait = MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE,
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) {
            lastError_ = GetLastError();
            ++readErrors_;
            break;
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

bool InputEngine::DrainRawInput() {
    // Bound each drain so shutdown messages are serviced under continuous input.
    for (int batch = 0; batch < 32 && !shouldStop_; ++batch) {
        UINT size = static_cast<UINT>(rawBuffer_.size() * sizeof(uint64_t));
        const UINT count = rawBufferReader_(reinterpret_cast<RAWINPUT*>(rawBuffer_.data()),
            &size, sizeof(RAWINPUTHEADER));
        if (count == UINT_MAX) {
            const DWORD error = GetLastError();
            if (error == ERROR_INSUFFICIENT_BUFFER && size > rawBuffer_.size() * sizeof(uint64_t) &&
                size <= 1024 * 1024) {
                rawBuffer_.resize((size + 7) / 8);
                continue;
            }
            lastError_ = error;
            ++readErrors_;
            return false;
        }
        if (count == 0) return true;
        LARGE_INTEGER timestamp;
        QueryPerformanceCounter(&timestamp);
        // RAWINPUT has no device timestamp. Preserve the shared read time;
        // assigning a fresh timestamp per packet would measure parser speed.
        if (!ProcessRawBatch(reinterpret_cast<const BYTE*>(rawBuffer_.data()),
            rawBuffer_.size() * sizeof(uint64_t), count, timestamp.QuadPart)) {
            lastError_ = ERROR_INVALID_DATA;
            ++readErrors_;
            return false;
        }
    }
    return true;
}

bool InputEngine::ProcessRawBatch(const BYTE* bytes, size_t size, UINT count, int64_t timestamp) {
    maxBatch_.store((std::max)(maxBatch_.load(std::memory_order_relaxed), uint64_t(count)), std::memory_order_relaxed);
    size_t offset = 0;
    for (UINT i = 0; i < count; ++i) {
        if (offset > size || size - offset < sizeof(RAWINPUTHEADER)) return false;
        RAWINPUTHEADER header;
        std::memcpy(&header, bytes + offset, sizeof(header));
        if (header.dwSize < sizeof(header) || header.dwSize > size - offset) return false;
        if (header.dwType == RIM_TYPEMOUSE) {
            if (header.dwSize < offsetof(RAWINPUT, data) + sizeof(RAWMOUSE)) return false;
            RAWMOUSE mouse;
            std::memcpy(&mouse, bytes + offset + offsetof(RAWINPUT, data), sizeof(mouse));
            packets_.fetch_add(1, std::memory_order_relaxed);
            if (count > 1) groupedPackets_.fetch_add(1, std::memory_order_relaxed);
            if (running_ && (mouse.lLastX != 0 || mouse.lLastY != 0)) {
                const MouseEvent event{header.hDevice, timestamp, mouse.lLastX, mouse.lLastY};
                if (!eventBuffer_.push(event)) droppedEvents_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // NEXTRAWINPUTBLOCK alignment, with checked bounds before each read.
        offset += (static_cast<size_t>(header.dwSize) + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
    }
    return true;
}

CaptureDiagnostics InputEngine::GetCaptureDiagnostics() const {
    return {true, packets_.load(std::memory_order_relaxed), groupedPackets_.load(std::memory_order_relaxed),
        maxBatch_.load(std::memory_order_relaxed), readErrors_.load(std::memory_order_relaxed),
        droppedEvents_.load(std::memory_order_relaxed), lastError_.load(std::memory_order_relaxed)};
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

void InputEngine::ResetEventRates() {
    eventRateA_ = eventRateB_ = 0.0;
    lastRateUpdateTime_ = 0;
    lastEventCountA_ = lastEventCountB_ = 0;
}

void InputEngine::UpdateEventRates(size_t eventsA, size_t eventsB) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (lastRateUpdateTime_ == 0 || eventsA < lastEventCountA_ || eventsB < lastEventCountB_) {
        eventRateA_ = eventRateB_ = 0.0;
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
