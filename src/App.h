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

    // Process raw input message
    void OnRawInput(LPARAM lParam);

    // Check if app should quit
    bool ShouldQuit() const { return shouldQuit_; }

    // Get current state
    AppState GetState() const { return state_; }

private:
    // UI rendering methods
    void RenderMainWindow();
    void RenderDevicePanel();
    void RenderControlPanel();
    void RenderPlotPanel();
    void RenderResultPanel();
    void RenderStatusBar();

    // State transitions
    void TransitionTo(AppState newState);

    // Recording control
    void StartRecording();
    void StopRecording();
    void RunAnalysis();

    // File operations
    void SaveSession();
    void LoadSession();
    void ExportCsv();

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
    bool syncPlotView_ = false;
    float plotTimeOffset_ = 0.0f;

    // Live plotting data (updated during recording)
    std::vector<double> liveTimesA_;
    std::vector<double> liveVelocitiesA_;
    std::vector<double> liveTimesB_;
    std::vector<double> liveVelocitiesB_;

    // Status message
    std::string statusMessage_;

    // QPC frequency
    int64_t qpcFrequency_ = 0;

    // Helper to calculate velocity
    static double CalculateVelocity(int32_t dx, int32_t dy);
};

} // namespace RLA
