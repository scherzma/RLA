#include "Analyzer.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <map>

namespace RLA {

ScaleFit Analyzer::FitScaleB(const RecordingSession& session) {
    ScaleFit result;
    result.message = "Not enough matching movement. Record both mice moving together for longer.";
    if (!std::isfinite(session.qpcFrequency) || session.qpcFrequency <= 0 ||
        session.eventsA.empty() || session.eventsB.empty()) return result;

    auto medianInterval = [&](const std::vector<MouseEvent>& events) {
        std::vector<double> intervals;
        intervals.reserve(events.size());
        for (size_t i = 1; i < events.size(); ++i) {
            if (events[i].timestamp > events[i - 1].timestamp)
                intervals.push_back((events[i].timestamp - events[i - 1].timestamp) * 1000.0 / session.qpcFrequency);
        }
        if (intervals.empty()) return 0.0;
        auto middle = intervals.begin() + intervals.size() / 2;
        std::nth_element(intervals.begin(), middle, intervals.end());
        return *middle;
    };
    // At least one typical report from the slower mouse per bin. Round to a
    // whole millisecond so small timestamp jitter does not shift every bin.
    result.binMs = (std::max)(2.0, std::round((std::max)(
        medianInterval(session.eventsA), medianInterval(session.eventsB))));
    if (result.binMs > 50.0) return result;
    const int windowBins = (std::max)(8, static_cast<int>(std::ceil(50.0 / result.binMs)));
    result.windowMs = windowBins * result.binMs;
    const int maxLagBins = static_cast<int>(10.0 / result.binMs);
    // Sparse common bins keep memory proportional to movement, not idle time.
    struct Delta { double x = 0, y = 0; };
    using Bins = std::map<int64_t, Delta>;
    auto binEvents = [&](const std::vector<MouseEvent>& events) {
        Bins bins;
        for (const auto& event : events) {
            const double index = std::floor((event.timestamp - session.startTimestamp) *
                1000.0 / session.qpcFrequency / result.binMs);
            if (!std::isfinite(index) || index < 0 || index >= std::ldexp(1.0, 63) - 1024) continue;
            auto& bin = bins[static_cast<int64_t>(index)];
            bin.x += event.deltaX;
            bin.y += event.deltaY;
        }
        return bins;
    };
    const auto a = binEvents(session.eventsA), b = binEvents(session.eventsB);
    auto at = [](const Bins& bins, int64_t index) {
        const auto it = bins.find(index);
        return it == bins.end() ? Delta{} : it->second;
    };
    std::vector<double> slopes;
    int64_t lastWindow = -1;
    for (const auto& [index, delta] : a) {
        const int64_t window = index / windowBins;
        if (window == lastWindow) continue;
        lastWindow = window;
        ++result.testedWindows;
        const int64_t first = window * windowBins;
        double bestError = 0.04, bestScale = 0;
        // Allow up to 10 ms of delay for matching only; plots retain original time.
        for (int lag = -maxLagBins; lag <= maxLagBins; ++lag) {
            double aa = 0, bb = 0, ab = 0, ax = 0, ay = 0, bx = 0, by = 0;
            int active = 0, sameDirection = 0;
            for (int j = 0; j < windowBins; ++j) {
                const auto va = at(a, first + j), vb = at(b, first + j + lag);
                const double a2 = va.x * va.x + va.y * va.y;
                const double b2 = vb.x * vb.x + vb.y * vb.y;
                const double dot = va.x * vb.x + va.y * vb.y;
                aa += a2; bb += b2; ab += dot;
                ax += va.x; ay += va.y; bx += vb.x; by += vb.y;
                if (a2 >= 4 && b2 >= 4) {
                    ++active;
                    if (dot > 0.9 * std::sqrt(a2 * b2)) ++sameDirection;
                }
            }
            if (active < windowBins * 0.6 || sameDirection < active * 0.9 || aa <= 0 || bb <= 0) continue;
            // Centered vector correlation rejects unrelated and constant movement.
            const double varA = aa - (ax * ax + ay * ay) / windowBins;
            const double varB = bb - (bx * bx + by * by) / windowBins;
            if (varA < aa * 0.02 || varB < bb * 0.02) continue;
            const double correlation = (ab - (ax * bx + ay * by) / windowBins) / std::sqrt(varA * varB);
            if (correlation < 0.9) continue;
            const double scale = ab / bb;
            const double error = (std::max)(0.0, (aa - ab * ab / bb) / aa);
            if (scale >= 0.01 && scale <= 100 && error < bestError) {
                bestError = error;
                bestScale = scale;
            }
        }
        if (bestScale > 0) slopes.push_back(std::log(bestScale));
    }
    if (slopes.size() < 3) return result;
    std::sort(slopes.begin(), slopes.end());
    // Use the largest group of ratios within 15%. A collision cannot dominate
    // the fit through its amplitude. Equally plausible groups cause refusal.
    size_t bestBegin = 0, bestEnd = 0, left = 0;
    for (size_t right = 0; right < slopes.size(); ++right) {
        while (slopes[right] - slopes[left] > std::log(1.15)) ++left;
        if (right + 1 - left > bestEnd - bestBegin) {
            bestBegin = left;
            bestEnd = right + 1;
        }
    }
    result.matchedWindows = bestEnd - bestBegin;
    if (result.matchedWindows < 3 || result.matchedWindows * 2 <= slopes.size()) {
        result.message = "Matching sections give conflicting scales. Use a recording with more shared movement.";
        return result;
    }
    const size_t middle = bestBegin + result.matchedWindows / 2;
    result.scale = std::exp(result.matchedWindows % 2 ? slopes[middle] :
        (slopes[middle - 1] + slopes[middle]) * 0.5);
    result.valid = true;
    result.message = "Scale fitted from matching movement with common time bins.";
    return result;
}

EventTiming Analyzer::BuildEventTiming(const std::vector<MouseEvent>& events,
    int64_t startTimestamp, double frequency, double endMs, double windowMs) {
    EventTiming result;
    if (!std::isfinite(frequency) || frequency <= 0 || !std::isfinite(endMs) ||
        endMs <= 0 || !std::isfinite(windowMs) || windowMs <= 0) return result;
    const double missing = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> positive;
    positive.reserve(events.size());
    result.timesMs.reserve(events.size());
    result.intervalsMs.reserve(events.size());
    result.instantHz.reserve(events.size());
    result.meanIntervalsMs.reserve(events.size());
    for (size_t i = 1; i < events.size(); ++i) {
        const double interval = (events[i].timestamp - events[i - 1].timestamp) * 1000.0 / frequency;
        result.timesMs.push_back((events[i].timestamp - startTimestamp) * 1000.0 / frequency);
        result.intervalsMs.push_back(interval >= 0 ? interval : missing);
        result.instantHz.push_back(interval > 0 ? 1000.0 / interval : missing);
        if (interval > 0) positive.push_back(interval);
        else ++result.nonPositiveIntervals;
    }
    size_t meanStart = 0, meanCount = 0;
    double meanSum = 0;
    for (size_t i = 0; i < result.timesMs.size(); ++i) {
        if (result.intervalsMs[i] > 0) { meanSum += result.intervalsMs[i]; ++meanCount; }
        while (meanStart < i && result.timesMs[meanStart] <= result.timesMs[i] - windowMs) {
            if (result.intervalsMs[meanStart] > 0) { meanSum -= result.intervalsMs[meanStart]; --meanCount; }
            ++meanStart;
        }
        result.meanIntervalsMs.push_back(meanCount ? meanSum / meanCount : missing);
    }
    if (!positive.empty()) {
        const size_t n = positive.size();
        result.maxMs = *std::max_element(positive.begin(), positive.end());
        auto middle = positive.begin() + n / 2;
        std::nth_element(positive.begin(), middle, positive.end());
        result.medianMs = n % 2 ? *middle : (*std::max_element(positive.begin(), middle) + *middle) * 0.5;
        auto percentile = positive.begin() + static_cast<size_t>(std::ceil(n * 0.95)) - 1;
        std::nth_element(positive.begin(), percentile, positive.end());
        result.p95Ms = *percentile;
    }
    // Fixed elapsed-time windows include idle sections as zero Hz. Two cursors
    // count (t-window, t], including events with identical timestamps.
    if (!std::is_sorted(events.begin(), events.end(), [](const MouseEvent& x, const MouseEvent& y) {
        return x.timestamp < y.timestamp;
    })) return result;
    const double step = (std::max)(windowMs / 4.0, endMs / 200000.0);
    size_t left = 0, right = 0;
    auto time = [&](size_t i) { return (events[i].timestamp - startTimestamp) * 1000.0 / frequency; };
    for (double t = (std::min)(windowMs, endMs); ; t = (std::min)(t + step, endMs)) {
        while (right < events.size() && time(right) <= t) ++right;
        while (left < right && time(left) <= t - windowMs) ++left;
        result.rateTimesMs.push_back(t);
        result.ratesHz.push_back((right - left) * 1000.0 / (std::min)(t, windowMs));
        if (t >= endMs) break;
    }
    return result;
}

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
    return std::hypot(static_cast<double>(dx), static_cast<double>(dy));
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
