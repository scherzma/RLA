#pragma once

#include "Types.h"
#include "RingBuffer.h"

#include <thread>
#include <atomic>
#include <functional>

namespace RLA {

// Ring buffer size: ~4 million events (power of 2), roughly 96MB
constexpr size_t RING_BUFFER_SIZE = 1 << 22; // 4,194,304 events

class InputEngine {
public:
    using EventCallback = std::function<void(const MouseEvent&)>;

    InputEngine();
    ~InputEngine();

    // Initialize the input engine (creates dedicated input thread)
    bool Initialize();

    // Start/stop recording events
    bool Start();
    void Stop();

    // Check if running
    bool IsRunning() const { return running_.load(); }

    // Process pending events - call from main thread
    // Returns number of events processed
    size_t ProcessEvents(const EventCallback& callback, size_t maxEvents = 8192);

    // Get buffer statistics
    size_t GetBufferSize() const { return eventBuffer_.size(); }
    size_t GetBufferCapacity() const { return eventBuffer_.capacity(); }
    float GetBufferUtilization() const;
    CaptureDiagnostics GetCaptureDiagnostics() const;
    bool SetRawCapture(bool enabled);
    bool IsRawCapture() const { return rawCapture_.load(); }
    bool TakeStopClick() { return stopClick_.exchange(false); }

    // Get event rate statistics (events per second)
    double GetEventRateA() const { return eventRateA_; }
    double GetEventRateB() const { return eventRateB_; }
    void UpdateEventRates(size_t eventsA, size_t eventsB);
    void ResetEventRates();

private:
    friend struct InputEngineRegressionAccess;

    // Input thread function - runs dedicated message loop
    void InputThreadFunc();

    // Window procedure for the hidden input window
    static LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // Returns false on an API/packet error. Called only on the input thread.
    bool DrainRawInput(size_t* recordsRead = nullptr);
    bool ProcessRawBatch(const BYTE* bytes, size_t size, UINT count, int64_t timestamp);

    // Register/unregister raw input for the hidden window
    bool RegisterRawInput(HWND hwnd, bool noLegacy = false);
    void UnregisterRawInput();

    // Hidden window for raw input (owned by input thread)
    HWND inputHwnd_ = nullptr;

    // Input thread
    std::thread inputThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> threadReady_{false};
    std::atomic<bool> shouldStop_{false};

    RingBuffer<MouseEvent, RING_BUFFER_SIZE> eventBuffer_;

    // Reused, 8-byte aligned storage. No allocation in the normal input path.
    std::vector<uint64_t> rawBuffer_ = std::vector<uint64_t>(8192);
    decltype(&GetRawInputBuffer) rawBufferReader_ = &GetRawInputBuffer;
    decltype(&RegisterRawInputDevices) rawDeviceRegistrar_ = &RegisterRawInputDevices;
    std::atomic<bool> rawCapture_{false}, stopClick_{false};
    std::atomic<uint64_t> packets_{0}, groupedPackets_{0}, maxBatch_{0};
    std::atomic<uint64_t> readErrors_{0}, droppedEvents_{0};
    std::atomic<uint32_t> lastError_{0};

    // QPC frequency for timestamp conversion
    int64_t qpcFrequency_ = 0;

    // Event rate tracking
    double eventRateA_ = 0.0;
    double eventRateB_ = 0.0;
    int64_t lastRateUpdateTime_ = 0;
    size_t lastEventCountA_ = 0;
    size_t lastEventCountB_ = 0;
};

} // namespace RLA
