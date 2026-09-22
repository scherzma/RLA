#pragma once

#include "Types.h"
#include "InputEngine.h"
#include "DeviceManager.h"
#include "Analyzer.h"
#include "Renderer.h"
#include "DataStore.h"

#include <memory>
#include <string>

namespace RLA {

class App {
public:
    App();
    ~App();

    // Initialize the application with window handle
    bool Initialize(HWND hwnd, int width, int height);

    // Main update loop - call each frame
    void Update();

    // Render the UI - call each frame
    void Render();

    // Handle window resize
    void OnResize(int width, int height);

    // Check if app should quit
    bool ShouldQuit() const { return shouldQuit_; }

    // Get current state
    AppState GetState() const { return state_; }

private:
    friend struct AppRegressionAccess;

    // UI rendering methods
    void RenderMainWindow();
    void RenderDevicePanel();
    void RenderControlPanel();
    void RenderPlotPanel();
    void RenderScaleControls();
    void ApplyAutoScaleB();
    void RenderTimingPanel();
    void RenderStatusBar();

    // State transitions
    void TransitionTo(AppState newState);

    // Recording control
    void StartRecording();
    void StopRecording();

    // File operations
    void SaveSession();
    void LoadSession();
    void ExportCsv();
    void RebuildPlotData();

    // Helper to get device display name
    std::string GetMouseDisplayName(const MouseDevice* device) const;

    // Components
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<InputEngine> inputEngine_;
    std::unique_ptr<DeviceManager> deviceManager_;
    std::unique_ptr<Analyzer> analyzer_;
    std::unique_ptr<DataStore> dataStore_;

    // State
    AppState state_ = AppState::Idle;
    bool shouldQuit_ = false;

    // Recording data
    RecordingSession currentSession_;
    AnalysisResult analysisResult_;

    // UI state
    bool showPlotWindow_ = true;

    // Live plotting data (updated during recording)
    std::vector<double> liveTimesA_;
    std::vector<double> liveVelocitiesA_;
    std::vector<double> liveTimesB_;
    std::vector<double> liveVelocitiesB_;

    // Smoothing options
    bool enableSmoothing_ = false;
    int smoothingMode_ = 0;  // 0 = by samples, 1 = by time
    int smoothingSamples_ = 3;  // Number of samples for moving average
    float smoothingTimeMs_ = 1.0f;  // Time window in ms

    // Y-axis scaling for Mouse B
    bool enableYScaleB_ = false;
    float yScaleB_ = 1.0f;  // Multiplier for Mouse B velocity values
    std::string autoScaleMessage_;

    EventTiming timingA_, timingB_;
    float rateWindowMs_ = 100.0f;
    int timingGraph_ = 0; // Both, intervals only, rate only.
    bool showTimingSettings_ = false;
    bool showTimingSummary_ = false;
    float timingGapMs_ = 20.0f;
    bool showTimingGaps_ = false;
    bool timingFitPending_ = true;
    double timingEndMs_ = 1.0;
    bool showInstantHz_ = false;
    bool followTiming_ = true;
    bool timingDirty_ = true;
    double lastTimingUpdate_ = -1.0;

    // Time binning options (aggregates events into time windows)
    bool enableTimeBinning_ = false;
    float timeBinMs_ = 1.0f;  // Bin size in milliseconds
    bool timeWeightedBins_ = true;

    // Gap interpolation options (fills gaps with zero-velocity points)
    bool enableGapInterpolation_ = true;  // Enabled by default
    float gapThresholdMs_ = 5.0f;  // Gaps larger than this are filled with zeros
    float gapSampleIntervalMs_ = 1.0f;  // Interval between interpolated zero points

    // Data point markers option
    bool showDataPointMarkers_ = false;  // Disabled by default
    float markerSize_ = 3.0f;  // Size of the marker dots

    // Plot mode: 0 = Movement magnitude, 1 = Raw X/Y Deltas, 2 = Event timing
    int plotMode_ = 0;

    // Time binning helper functions
    void ApplyTimeBinning(const std::vector<MouseEvent>& events, int64_t startTimestamp,
                          std::vector<double>& outTimes, std::vector<double>& outVelocities, double binMs);

    // Smoothing helper functions
    void ApplyMovingAverageSmoothing(const std::vector<double>& times, const std::vector<double>& values,
                                     std::vector<double>& outTimes, std::vector<double>& outValues, int windowSize);
    void ApplyTimeWindowSmoothing(const std::vector<double>& times, const std::vector<double>& values,
                                  std::vector<double>& outTimes, std::vector<double>& outValues, double windowMs);

    // Gap interpolation helper function (fills time gaps with zero-velocity points)
    void InterpolateGaps(const std::vector<double>& times, const std::vector<double>& values,
                         std::vector<double>& outTimes, std::vector<double>& outValues,
                         double gapThresholdMs, double sampleIntervalMs);

    // Status message
    std::string statusMessage_;

    // QPC frequency
    int64_t qpcFrequency_ = 0;

    // Helper to calculate velocity
    static double CalculateVelocity(int32_t dx, int32_t dy);
};

} // namespace RLA
