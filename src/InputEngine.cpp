#include "InputEngine.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <cwchar>

namespace RLA {
namespace { constexpr UINT ConfigureForegroundTest=WM_APP+81; }

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
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* engine = reinterpret_cast<InputEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case ConfigureForegroundTest:
            return engine && engine->SwitchForegroundTest(wParam!=0,reinterpret_cast<HWND>(lParam));
        case WM_TIMER:
            if (engine && hwnd==engine->foregroundHwnd_) { InvalidateRect(hwnd,nullptr,FALSE); return 0; }
            break;
        case WM_PAINT:
            if (engine && hwnd==engine->foregroundHwnd_) { engine->PaintForegroundTest(hwnd); return 0; }
            break;
        case WM_LBUTTONDOWN:
            if (engine && hwnd==engine->foregroundHwnd_) { engine->stopClick_=true; return 0; }
            break;
        case WM_KEYDOWN:
            if (engine && hwnd==engine->foregroundHwnd_ && wParam==VK_ESCAPE) { engine->stopClick_=true; return 0; }
            break;
        case WM_CLOSE:
            if (engine && hwnd==engine->foregroundHwnd_) { engine->stopClick_=true; return 0; }
            break;
        case WM_ACTIVATE:
            if (engine && hwnd==engine->foregroundHwnd_ && engine->foregroundTestActive_ && LOWORD(wParam)==WA_INACTIVE)
                engine->stopClick_=true;
            break;
        case WM_INPUT:
            // GetMessage removed this one report. Read it before draining the
            // remaining queue, then let Windows clean up its message handle.
            if (engine) { engine->ReadRawInput(reinterpret_cast<HRAWINPUT>(lParam)); engine->DrainRawInput(); }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_DESTROY:
            if (engine && hwnd==engine->inputHwnd_) PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd,msg,wParam,lParam);
}

void InputEngine::InputThreadFunc() {
    // Set thread priority for time-critical input handling
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

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
        this
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

    // Message-driven wake-ups have no timer delay and no stale queue flag loop.
    // WM_INPUT reads the removed report with GetRawInputData, then uses
    // GetRawInputBuffer for reports that arrived while it was being processed.
    MSG msg{};
    while (!shouldStop_.load()) {
        const BOOL received = GetMessageW(&msg, nullptr, 0, 0);
        if (received <= 0) {
            if (received == -1) { lastError_ = GetLastError(); ++readErrors_; }
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Cleanup
    foregroundTestActive_=false;
    if (foregroundHwnd_) { DestroyWindow(foregroundHwnd_); foregroundHwnd_=nullptr; }
    UnregisterRawInput();
    if (inputHwnd_) {
        DestroyWindow(inputHwnd_);
        inputHwnd_ = nullptr;
    }
}

bool InputEngine::RegisterRawInput(HWND hwnd, bool noLegacy) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = RIDEV_INPUTSINK | (noLegacy ? RIDEV_NOLEGACY : 0);
    rid.hwndTarget = hwnd;

    if (!rawDeviceRegistrar_(&rid, 1, sizeof(rid))) {
        return false;
    }

    return true;
}

bool InputEngine::SetRawCapture(bool enabled) {
    if (rawCapture_.load() == enabled) return true;
    if (!initialized_ || shouldStop_) return false;
    const HWND target=ForegroundTestWindow();
    if (!RegisterRawInput(target ? target : inputHwnd_, enabled)) {
        lastError_ = GetLastError();
        ++readErrors_;
        return false;
    }
    stopClick_ = false;
    rawCapture_ = enabled;
    return true;
}

bool InputEngine::BeginForegroundTest(int64_t startTimestamp) {
    if (!initialized_ || shouldStop_) return false;
    foregroundTestStart_=startTimestamp;
    DWORD_PTR result=0;
    return SendMessageTimeoutW(inputHwnd_,ConfigureForegroundTest,TRUE,0,SMTO_ABORTIFHUNG,2000,&result) && result!=0;
}

bool InputEngine::EndForegroundTest(HWND returnWindow) {
    if (!initialized_ || shouldStop_) return !foregroundTestActive_;
    DWORD_PTR result=0;
    return SendMessageTimeoutW(inputHwnd_,ConfigureForegroundTest,FALSE,reinterpret_cast<LPARAM>(returnWindow),SMTO_ABORTIFHUNG,2000,&result) && result!=0;
}

bool InputEngine::SwitchForegroundTest(bool enabled, HWND returnWindow) {
    // Both capture windows belong to this thread. Parser state and the SPSC
    // ring buffer therefore retain one producer in both test modes.
    if (enabled) {
        if (!foregroundHwnd_) {
            foregroundHwnd_=CreateWindowExW(0,L"RLA_InputWindowClass",L"RLA - Foreground capture test",
                WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, CW_USEDEFAULT,CW_USEDEFAULT,720,300,
                nullptr,nullptr,GetModuleHandleW(nullptr),this);
        }
        const HWND target=foregroundHwnd_;
        if (!target || !RegisterRawInput(target,rawCapture_)) return false;
        foregroundTestActive_=true;
        windowShower_(target,SW_SHOW);
        foregroundSetter_(target);
        if (foregroundGetter_()!=target) {
            RegisterRawInput(inputHwnd_,rawCapture_);
            foregroundTestActive_=false;
            windowShower_(target,SW_HIDE);
            return false;
        }
        if (!SetTimer(target,1,100,nullptr)) {
            RegisterRawInput(inputHwnd_,rawCapture_);
            foregroundTestActive_=false;
            windowShower_(target,SW_HIDE);
            return false;
        }
        InvalidateRect(target,nullptr,TRUE);
        return true;
    }
    if (!RegisterRawInput(inputHwnd_,rawCapture_)) return false;
    const HWND target=foregroundHwnd_;
    const bool restore=target && foregroundGetter_()==target;
    foregroundTestActive_=false;
    if (target) { KillTimer(target,1); windowShower_(target,SW_HIDE); }
    if (restore && returnWindow) foregroundSetter_(returnWindow);
    return true;
}

void InputEngine::PaintForegroundTest(HWND hwnd) {
    PAINTSTRUCT paint{}; const HDC dc=BeginPaint(hwnd,&paint);
    RECT rect{}; GetClientRect(hwnd,&rect); FillRect(dc,&rect,GetSysColorBrush(COLOR_WINDOW));
    const auto oldFont=SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT)); SetBkMode(dc,TRANSPARENT);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double elapsed=(now.QuadPart-foregroundTestStart_)*1000.0/qpcFrequency_;
    const wchar_t* stage=elapsed<3000 ? L"GET READY" : elapsed<8000 ? L"MOVE A ONLY" : elapsed<13000 ? L"MOVE BOTH MICE" : L"MOVE A ONLY AGAIN";
    const double boundary=elapsed<3000 ? 3000 : elapsed<8000 ? 8000 : elapsed<13000 ? 13000 : 18000;
    wchar_t text[512]{};
    swprintf_s(text,L"Foreground capture test - graphs off\n\n%s\n%.1f seconds remaining in this stage\n\nKeep this window in front. Click or press Esc to stop.\nThe test stops and saves automatically after 18 seconds.",stage,(std::max)(0.0,(boundary-elapsed)/1000.0));
    InflateRect(&rect,-20,-20); DrawTextW(dc,text,-1,&rect,DT_CENTER|DT_WORDBREAK);
    SelectObject(dc,oldFont); EndPaint(hwnd,&paint);
}

void InputEngine::UnregisterRawInput() {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;

    rawDeviceRegistrar_(&rid, 1, sizeof(rid));
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

bool InputEngine::ReadRawInput(HRAWINPUT input) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    const UINT read = rawDataReader_(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));
    if (read == UINT_MAX) { lastError_ = GetLastError(); ++readErrors_; return false; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (read > sizeof(raw) || !ProcessRawBatch(reinterpret_cast<const BYTE*>(&raw), read, 1, now.QuadPart)) {
        lastError_ = ERROR_INVALID_DATA; ++readErrors_; return false;
    }
    return true;
}

bool InputEngine::DrainRawInput(size_t* recordsRead) {
    if (recordsRead) *recordsRead = 0;
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
        if (recordsRead) *recordsRead += count;
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
            // Ordinary button messages are disabled in raw capture mode.
            // Retain a stop gesture even on reports with no movement.
            if (rawCapture_ && (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)) stopClick_ = true;
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
        droppedEvents_.load(std::memory_order_relaxed), lastError_.load(std::memory_order_relaxed), rawCapture_.load()};
}

size_t InputEngine::ProcessEvents(const EventCallback& callback, size_t maxEvents) {
    size_t processed = 0;
    // Drain a bounded snapshot. A producer that keeps adding events must not
    // keep the UI inside this function indefinitely.
    const size_t available = (std::min)(maxEvents, eventBuffer_.size());
    while (processed < available) {
        const auto event = eventBuffer_.pop();
        if (!event) break;
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
