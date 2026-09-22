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
    size_t ProcessEvents(const EventCallback& callback);

    // Get buffer statistics
    size_t GetBufferSize() const { return eventBuffer_.size(); }
    size_t GetBufferCapacity() const { return eventBuffer_.capacity(); }
    float GetBufferUtilization() const;

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

    // Process raw input message (called on input thread)
    void ProcessRawInput(LPARAM lParam);

    // Register/unregister raw input for the hidden window
    bool RegisterRawInput(HWND hwnd);
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
