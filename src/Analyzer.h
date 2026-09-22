#pragma once

#include "Types.h"

#include <vector>

namespace RLA {

struct ScaleFit {
    bool valid = false;
    double scale = 1.0;
    double binMs = 2.0;
    double windowMs = 50.0;
    size_t matchedWindows = 0;
    size_t testedWindows = 0;
    std::string message;
};

struct EventTiming {
    std::vector<double> timesMs, intervalsMs, instantHz;
    std::vector<double> meanIntervalsMs;
    std::vector<double> rateTimesMs, ratesHz;
    size_t nonPositiveIntervals = 0;
    double medianMs = 0.0, p95Ms = 0.0, maxMs = 0.0;
};

class Analyzer {
public:
    Analyzer();
    ~Analyzer() = default;

    // Set QPC frequency for timestamp conversion
    void SetQPCFrequency(int64_t frequency) { qpcFrequency_ = frequency; }

    // Analyze recorded data and detect bump/collision
    AnalysisResult Analyze(const RecordingSession& session);

    // Fits B to A from shared movement only. Does not change event timestamps.
    static ScaleFit FitScaleB(const RecordingSession& session);
    static EventTiming BuildEventTiming(const std::vector<MouseEvent>& events,
        int64_t startTimestamp, double frequency, double endMs, double windowMs);

    // Configuration
    void SetAccelerationThreshold(double threshold) { accelerationThreshold_ = threshold; }
    double GetAccelerationThreshold() const { return accelerationThreshold_; }

    void SetCorrelationWindow(double windowMs) { correlationWindowMs_ = windowMs; }
    double GetCorrelationWindow() const { return correlationWindowMs_; }

    void SetMovingAverageWindow(int window) { movingAverageWindow_ = window; }
    int GetMovingAverageWindow() const { return movingAverageWindow_; }

private:
    // Convert QPC ticks to microseconds
    double TicksToMicroseconds(int64_t ticks) const;

    // Convert QPC ticks to milliseconds
    double TicksToMilliseconds(int64_t ticks) const;

    // Calculate velocity from delta values
    static double CalculateVelocity(int32_t dx, int32_t dy);

    // Build velocity profile from events
    std::vector<VelocityPoint> BuildVelocityProfile(
        const std::vector<MouseEvent>& events,
        int64_t startTimestamp);

    // Apply moving average smoothing
    std::vector<VelocityPoint> SmoothVelocityProfile(const std::vector<VelocityPoint>& profile);

    // Find peak deceleration (impact point)
    // Returns index of peak, or -1 if not found
    int FindImpactPoint(const std::vector<VelocityPoint>& profile);

    // Find corresponding impact in other mouse within correlation window
    int FindCorrelatedImpact(
        const std::vector<VelocityPoint>& profile,
        double targetTimeMs,
        double windowMs);

    int64_t qpcFrequency_ = 10000000; // Default 10MHz
    double accelerationThreshold_ = 500.0; // Minimum deceleration for impact detection
    double correlationWindowMs_ = 50.0; // Window to search for correlated impact
    int movingAverageWindow_ = 5; // Window size for smoothing
};

} // namespace RLA
