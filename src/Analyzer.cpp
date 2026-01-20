#include "Analyzer.h"

#include <cmath>
#include <algorithm>
#include <limits>

namespace RLA {

Analyzer::Analyzer() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

double Analyzer::TicksToMicroseconds(int64_t ticks) const {
    return (static_cast<double>(ticks) * 1000000.0) / static_cast<double>(qpcFrequency_);
}

double Analyzer::TicksToMilliseconds(int64_t ticks) const {
    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(qpcFrequency_);
}

double Analyzer::CalculateVelocity(int32_t dx, int32_t dy) {
    return std::sqrt(static_cast<double>(dx * dx + dy * dy));
}

std::vector<VelocityPoint> Analyzer::BuildVelocityProfile(
    const std::vector<MouseEvent>& events,
    int64_t startTimestamp) {

    std::vector<VelocityPoint> profile;
    profile.reserve(events.size());

    double prevVelocity = 0.0;

    for (const auto& event : events) {
        VelocityPoint point;
        point.timeMs = TicksToMilliseconds(event.timestamp - startTimestamp);
        point.velocity = CalculateVelocity(event.deltaX, event.deltaY);
        point.acceleration = point.velocity - prevVelocity;
        prevVelocity = point.velocity;
        profile.push_back(point);
    }

    return profile;
}

std::vector<VelocityPoint> Analyzer::SmoothVelocityProfile(const std::vector<VelocityPoint>& profile) {
    if (profile.size() < static_cast<size_t>(movingAverageWindow_)) {
        return profile;
    }

    std::vector<VelocityPoint> smoothed;
    smoothed.reserve(profile.size());

    int halfWindow = movingAverageWindow_ / 2;

    for (size_t i = 0; i < profile.size(); ++i) {
        VelocityPoint point = profile[i];

        // Calculate moving average for velocity
        double sum = 0.0;
        int count = 0;

        for (int j = -halfWindow; j <= halfWindow; ++j) {
            int idx = static_cast<int>(i) + j;
            if (idx >= 0 && idx < static_cast<int>(profile.size())) {
                sum += profile[idx].velocity;
                ++count;
            }
        }

        if (count > 0) {
            point.velocity = sum / count;
        }

        smoothed.push_back(point);
    }

    // Recalculate acceleration after smoothing
    for (size_t i = 1; i < smoothed.size(); ++i) {
        smoothed[i].acceleration = smoothed[i].velocity - smoothed[i - 1].velocity;
    }

    return smoothed;
}

int Analyzer::FindImpactPoint(const std::vector<VelocityPoint>& profile) {
    if (profile.size() < 3) {
        return -1;
    }

    int bestIndex = -1;
    double maxDeceleration = 0.0;

    // Look for peak negative acceleration (deceleration = impact)
    for (size_t i = 1; i < profile.size(); ++i) {
        // Impact is characterized by sudden deceleration (negative acceleration)
        double deceleration = -profile[i].acceleration;

        if (deceleration > accelerationThreshold_ && deceleration > maxDeceleration) {
            maxDeceleration = deceleration;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

int Analyzer::FindCorrelatedImpact(
    const std::vector<VelocityPoint>& profile,
    double targetTimeMs,
    double windowMs) {

    if (profile.empty()) {
        return -1;
    }

    int bestIndex = -1;
    double maxDeceleration = 0.0;

    for (size_t i = 1; i < profile.size(); ++i) {
        double timeDiff = std::abs(profile[i].timeMs - targetTimeMs);

        if (timeDiff <= windowMs) {
            double deceleration = -profile[i].acceleration;

            // Find strongest deceleration within window
            if (deceleration > accelerationThreshold_ * 0.5 && deceleration > maxDeceleration) {
                maxDeceleration = deceleration;
                bestIndex = static_cast<int>(i);
            }
        }
    }

    return bestIndex;
}

AnalysisResult Analyzer::Analyze(const RecordingSession& session) {
    AnalysisResult result;
    result.valid = false;

    if (session.eventsA.empty() || session.eventsB.empty()) {
        result.errorMessage = "Insufficient data: one or both mice have no recorded events";
        return result;
    }

    // Update QPC frequency from session
    if (session.qpcFrequency > 0) {
        qpcFrequency_ = static_cast<int64_t>(session.qpcFrequency);
    }

    // Build velocity profiles
    result.mouseAData = BuildVelocityProfile(session.eventsA, session.startTimestamp);
    result.mouseBData = BuildVelocityProfile(session.eventsB, session.startTimestamp);

    // Apply smoothing
    auto smoothedA = SmoothVelocityProfile(result.mouseAData);
    auto smoothedB = SmoothVelocityProfile(result.mouseBData);

    // Find impact point in Mouse A (reference)
    int impactIndexA = FindImpactPoint(smoothedA);

    if (impactIndexA < 0) {
        result.errorMessage = "No impact detected in reference mouse (Mouse A). Try a stronger collision.";
        return result;
    }

    double impactTimeA = smoothedA[impactIndexA].timeMs;

    // Find correlated impact in Mouse B
    int impactIndexB = FindCorrelatedImpact(smoothedB, impactTimeA, correlationWindowMs_);

    if (impactIndexB < 0) {
        result.errorMessage = "No correlated impact found in test mouse (Mouse B). Ensure both mice collide simultaneously.";
        return result;
    }

    double impactTimeB = smoothedB[impactIndexB].timeMs;

    // Calculate latency difference in microseconds
    result.latencyDiffMicroseconds = (impactTimeB - impactTimeA) * 1000.0;

    // Store original timestamps (approximate - based on profile index)
    if (impactIndexA < static_cast<int>(session.eventsA.size())) {
        result.impactTimestampA = session.eventsA[impactIndexA].timestamp;
    }
    if (impactIndexB < static_cast<int>(session.eventsB.size())) {
        result.impactTimestampB = session.eventsB[impactIndexB].timestamp;
    }

    result.valid = true;
    return result;
}

} // namespace RLA
