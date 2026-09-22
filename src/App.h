#pragma once

#include "Types.h"
#include "InputEngine.h"
#include "DeviceManager.h"
#include "Analyzer.h"
#include "Renderer.h"
#include "DataStore.h"
#include "Autosave.h"
#include "MouseLibrary.h"

#include <memory>
#include <string>
#include <future>

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
    void ReleaseRecordingCursor();
    void OnCaptureEscape();
    void OnCaptureFocusLost();

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
    void RenderCaptureTest();
    int captureTestChoice_ = 2;
    void RenderPlotPanel();
    void RenderScaleControls();
    void ApplyAutoScaleB();
    void RenderLatencyControls();
    void StartLatencyAnalysis();
    void PollLatencyAnalysis();
    void ResetLatencyAnalysis();
    void RenderAutosaveControls();
    void RenderMouseLibrary();
    bool SetupCombo(const char* label, std::string& selection);
    void RenderDeviceBinding(const char* label, const MouseDevice* device);
    void LibraryChanged();
    void PollLibrarySave();
    void SaveComparison();
    void CaptureMouseIdentity();
    void DetectSessionRates();
    void OpenRankedRun(const std::string& key);
    void RenderTimingPanel();
    void RenderStatusBar();

    // State transitions
    void TransitionTo(AppState newState);

    // Recording control
    void StartRecording();
    void StopRecording();
    void UpdateRecordingCursor();

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
    std::unique_ptr<Autosave> autosave_;
    std::string autosaveError_;
    MouseLibrary mouseLibrary_;
    std::filesystem::path libraryPath_;
    bool libraryReadOnly_ = false, libraryDirty_ = false, rankingDirty_ = true;
    bool librarySaveFailed_ = false;
    size_t savingRuns_ = 0;
    bool autoRankPending_ = false;
    std::future<std::string> librarySaveTask_;
    std::deque<RecordingSession> runsToKeep_;
    std::string libraryMessage_, comparisonMessage_;
    std::vector<MouseRank> ranking_;
    int rankMethod_ = 2, resultMethod_ = 2;
    std::string editMouse_, editSetup_, setupParent_;
    char mouseName_[161]{}, setupLabel_[161]{};
    int connectionIndex_ = 1, setupRate_ = 0;
    bool showLibrary_ = false;
    int libraryPage_ = 0; // 0: device setup, 1: rankings
    bool showAllLatencyMarkers_ = false;
    bool highlightLatencyMatch_ = false;
    bool fitMovementView_ = false;
    HANDLE libraryLock_ = nullptr;

    // State
    AppState state_ = AppState::Idle;
    bool shouldQuit_ = false;
    HWND hwnd_ = nullptr;
    bool keepCursorInWindow_ = true;
    bool cursorConfined_ = false;
    RECT cursorClip_{};
    bool rawCaptureMode_ = true;
    bool stoppingRecording_ = false;

    // Recording data
    RecordingSession currentSession_;
    AnalysisResult analysisResult_;
    LatencyFit latencyFit_;
    std::future<LatencyFit> latencyTask_;
    uint64_t sessionRevision_ = 0, latencyRevision_ = 0;
    int latencyMode_ = 2;
    int selectedLatencyMatch_ = -1;
    bool focusLatencyMatch_ = false;

    // UI state
    bool showPlotWindow_ = true;

    // Live plotting data (updated during recording)
    std::vector<double> liveTimesA_;
    std::vector<double> liveVelocitiesA_;
    std::vector<double> liveTimesB_;
    std::vector<double> liveVelocitiesB_;

    // Smoothing options
    bool enableSmoothing_ = true;
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
    bool showTimingRaw_ = true;
    float timingRawOpacity_ = 0.18f;
    float timingGapMs_ = 20.0f;
    bool showTimingGaps_ = false;
    bool timingFitPending_ = true;
    double timingEndMs_ = 1.0;
    bool showInstantHz_ = false;
    bool followTiming_ = true;
    bool timingDirty_ = true;
    double lastTimingUpdate_ = -1.0;

    // Time binning options (aggregates events into time windows)
    bool enableTimeBinning_ = true;
    float timeBinMs_ = 2.0f;  // Bin size in milliseconds
    bool timeWeightedBins_ = true;

    // Gap interpolation options (fills gaps with zero-velocity points)
    bool enableGapInterpolation_ = true;  // Enabled by default
    float gapThresholdMs_ = 5.0f;  // Gaps larger than this are filled with zeros
    float gapSampleIntervalMs_ = 1.0f;  // Interval between interpolated zero points

    struct MovementKey {
        uint64_t revision;
        size_t events;
        bool binning, weighted, fillGaps, smoothing;
        float binMs, gapMs, gapStep, smoothMs, scale;
        int smoothMode, samples;
        bool operator==(const MovementKey&) const = default;
    };
    struct MovementCache {
        MovementKey key{};
        bool valid=false;
        size_t builds=0;
        std::chrono::steady_clock::time_point updatedAt{};
        std::vector<double> times, values;
    };
    MovementCache movementA_, movementB_;
    const MovementCache& PrepareMovementPlot(bool mouseB);

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
