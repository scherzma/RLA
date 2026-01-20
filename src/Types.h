#pragma once

#include <cstdint>
#include <vector>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace RLA {

// Minimal footprint event for 8kHz capture
struct MouseEvent {
    HANDLE deviceHandle;    // 8 bytes
    int64_t timestamp;      // 8 bytes (QPC ticks)
    int32_t deltaX;         // 4 bytes
    int32_t deltaY;         // 4 bytes
};  // Total: 24 bytes

// Velocity data point for analysis
struct VelocityPoint {
    double timeMs;          // Relative to recording start
    double velocity;        // sqrt(dx^2 + dy^2)
    double acceleration;    // Change in velocity
};

// Device information
struct MouseDevice {
    HANDLE handle = nullptr;
    std::wstring name;
    std::wstring path;
    bool assigned = false;
};

// Analysis result from bump detection
struct AnalysisResult {
    bool valid = false;
    double latencyDiffMicroseconds = 0.0;
    int64_t impactTimestampA = 0;
    int64_t impactTimestampB = 0;
    std::vector<VelocityPoint> mouseAData;
    std::vector<VelocityPoint> mouseBData;
    std::string errorMessage;
};

// Application states
enum class AppState {
    Idle,
    AssigningMouseA,
    AssigningMouseB,
    Ready,
    Recording,
    Analyzing,
    ShowingResults
};

// Recording session info
struct RecordingSession {
    int64_t startTimestamp = 0;
    int64_t endTimestamp = 0;
    std::vector<MouseEvent> eventsA;
    std::vector<MouseEvent> eventsB;
    double qpcFrequency = 0.0;
};

} // namespace RLA
