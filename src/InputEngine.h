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

    // Initialize raw input with given window handle
    bool Initialize(HWND hwnd);

    // Start/stop the high-priority input thread
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

    // Process WM_INPUT message (call from window procedure)
    void ProcessRawInput(LPARAM lParam);

private:
    void InputThreadFunc(std::stop_token stopToken);
    bool RegisterRawInput(HWND hwnd);
    void UnregisterRawInput();

    HWND hwnd_ = nullptr;
    std::jthread inputThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};

    RingBuffer<MouseEvent, RING_BUFFER_SIZE> eventBuffer_;

    // QPC frequency for timestamp conversion
    int64_t qpcFrequency_ = 0;
};

} // namespace RLA
