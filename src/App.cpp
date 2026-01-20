#include "App.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <format>

// Windows file dialogs
#include <commdlg.h>
#include <ShlObj.h>

namespace RLA {

App::App() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

App::~App() = default;

bool App::Initialize(HWND hwnd, int width, int height) {
    // Create components
    renderer_ = std::make_unique<Renderer>();
    inputEngine_ = std::make_unique<InputEngine>();
    deviceManager_ = std::make_unique<DeviceManager>();
    analyzer_ = std::make_unique<Analyzer>();
    dataStore_ = std::make_unique<DataStore>();

    // Initialize renderer
    if (!renderer_->Initialize(hwnd, width, height)) {
        statusMessage_ = "Failed to initialize renderer";
        return false;
    }

    // Initialize input engine
    if (!inputEngine_->Initialize(hwnd)) {
        statusMessage_ = "Failed to initialize input engine";
        return false;
    }

    inputEngine_->Start();
    statusMessage_ = "Ready - Assign mice to begin";

    return true;
}

void App::Update() {
    // Process input events
    inputEngine_->ProcessEvents([this](const MouseEvent& event) {
        // Handle device assignment
        if (deviceManager_->IsAssigning()) {
            if (deviceManager_->ProcessEvent(event)) {
                // Assignment completed
                if (deviceManager_->IsMouseAAssigned() && !deviceManager_->IsMouseBAssigned() &&
                    state_ == AppState::AssigningMouseA) {
                    statusMessage_ = "Mouse A assigned! Now assign Mouse B";
                    TransitionTo(AppState::AssigningMouseB);
                    deviceManager_->StartAssigningMouseB();
                }
                else if (deviceManager_->IsMouseAAssigned() && deviceManager_->IsMouseBAssigned()) {
                    statusMessage_ = "Both mice assigned - Ready to record";
                    TransitionTo(AppState::Ready);
                }
            }
        }
        // Recording
        else if (state_ == AppState::Recording) {
            double timeMs = (event.timestamp - currentSession_.startTimestamp) * 1000.0 / qpcFrequency_;
            double velocity = CalculateVelocity(event.deltaX, event.deltaY);

            if (deviceManager_->IsMouseA(event.deviceHandle)) {
                currentSession_.eventsA.push_back(event);
                liveTimesA_.push_back(timeMs);
                liveVelocitiesA_.push_back(velocity);
            }
            else if (deviceManager_->IsMouseB(event.deviceHandle)) {
                currentSession_.eventsB.push_back(event);
                liveTimesB_.push_back(timeMs);
                liveVelocitiesB_.push_back(velocity);
            }
        }
    });
}

void App::Render() {
    renderer_->BeginFrame();

    RenderMainWindow();

    renderer_->EndFrame();
}

void App::RenderMainWindow() {
    // Setup main window to fill viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;

    ImGui::Begin("RLA Main", nullptr, windowFlags);

    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Session...", "Ctrl+S")) {
                SaveSession();
            }
            if (ImGui::MenuItem("Load Session...", "Ctrl+O")) {
                LoadSession();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export CSV...")) {
                ExportCsv();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                shouldQuit_ = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show Plot", nullptr, &showPlotWindow_);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // Main layout
    RenderDevicePanel();
    ImGui::Separator();
    RenderControlPanel();
    ImGui::Separator();

    if (showPlotWindow_) {
        RenderPlotPanel();
    }

    if (analysisResult_.valid || !analysisResult_.errorMessage.empty()) {
        RenderResultPanel();
    }

    RenderStatusBar();

    ImGui::End();
}

void App::RenderDevicePanel() {
    ImGui::Text("Device Assignment");

    // Mouse A
    ImGui::BeginGroup();
    {
        auto* deviceA = deviceManager_->GetMouseADevice();
        std::string nameA = deviceA ? GetMouseDisplayName(deviceA) : "Not Assigned";

        ImGui::Text("Reference Mouse (A):");
        ImGui::SameLine();
        ImGui::TextColored(deviceA ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.8f, 0.2f, 0.2f, 1.0f),
                          "%s", nameA.c_str());
        ImGui::SameLine();

        bool isAssigning = state_ == AppState::AssigningMouseA;
        if (isAssigning) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "(Wiggle mouse...)");
            if (ImGui::Button("Cancel##A")) {
                deviceManager_->CancelAssignment();
                TransitionTo(AppState::Idle);
                statusMessage_ = "Assignment cancelled";
            }
        }
        else {
            if (ImGui::Button("Assign##A")) {
                deviceManager_->StartAssigningMouseA();
                TransitionTo(AppState::AssigningMouseA);
                statusMessage_ = "Wiggle the REFERENCE mouse to assign it";
            }
        }
    }
    ImGui::EndGroup();

    // Mouse B
    ImGui::BeginGroup();
    {
        auto* deviceB = deviceManager_->GetMouseBDevice();
        std::string nameB = deviceB ? GetMouseDisplayName(deviceB) : "Not Assigned";

        ImGui::Text("Test Mouse (B):     ");
        ImGui::SameLine();
        ImGui::TextColored(deviceB ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.8f, 0.2f, 0.2f, 1.0f),
                          "%s", nameB.c_str());
        ImGui::SameLine();

        bool isAssigning = state_ == AppState::AssigningMouseB;
        if (isAssigning) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "(Wiggle mouse...)");
            if (ImGui::Button("Cancel##B")) {
                deviceManager_->CancelAssignment();
                TransitionTo(AppState::Idle);
                statusMessage_ = "Assignment cancelled";
            }
        }
        else {
            bool canAssign = deviceManager_->IsMouseAAssigned();
            if (!canAssign) ImGui::BeginDisabled();
            if (ImGui::Button("Assign##B")) {
                deviceManager_->StartAssigningMouseB();
                TransitionTo(AppState::AssigningMouseB);
                statusMessage_ = "Wiggle the TEST mouse to assign it";
            }
            if (!canAssign) ImGui::EndDisabled();
        }
    }
    ImGui::EndGroup();

    // Reset button
    if (deviceManager_->IsMouseAAssigned() || deviceManager_->IsMouseBAssigned()) {
        ImGui::SameLine();
        if (ImGui::Button("Reset All")) {
            deviceManager_->ResetAssignments();
            TransitionTo(AppState::Idle);
            statusMessage_ = "Assignments reset";
        }
    }
}

void App::RenderControlPanel() {
    ImGui::Text("Recording Controls");

    bool canRecord = deviceManager_->IsMouseAAssigned() && deviceManager_->IsMouseBAssigned();
    bool isRecording = state_ == AppState::Recording;

    if (!canRecord && !isRecording) {
        ImGui::BeginDisabled();
    }

    if (!isRecording) {
        if (ImGui::Button("Start Recording")) {
            StartRecording();
        }
    }
    else {
        if (ImGui::Button("Stop Recording")) {
            StopRecording();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "RECORDING");
        ImGui::SameLine();
        ImGui::Text("Events: A=%zu B=%zu",
                   currentSession_.eventsA.size(),
                   currentSession_.eventsB.size());

        // Show live polling rates
        inputEngine_->UpdateEventRates(currentSession_.eventsA.size(), currentSession_.eventsB.size());
        double rateA = inputEngine_->GetEventRateA();
        double rateB = inputEngine_->GetEventRateB();

        ImGui::SameLine();
        ImGui::Text("| Rate: A=%.0f Hz  B=%.0f Hz", rateA, rateB);
    }

    if (!canRecord && !isRecording) {
        ImGui::EndDisabled();
    }

    // Analyze button
    ImGui::SameLine();
    bool canAnalyze = !currentSession_.eventsA.empty() && !currentSession_.eventsB.empty() &&
                      state_ != AppState::Recording;

    if (!canAnalyze) ImGui::BeginDisabled();
    if (ImGui::Button("Analyze")) {
        RunAnalysis();
    }
    if (!canAnalyze) ImGui::EndDisabled();

    // Buffer utilization
    ImGui::SameLine();
    float utilization = inputEngine_->GetBufferUtilization() * 100.0f;
    ImGui::Text("Buffer: %.1f%%", utilization);
}

void App::RenderPlotPanel() {
    ImGui::Text("Velocity Plot");

    // Plot controls
    ImGui::Checkbox("Sync View (shift B by latency)", &syncPlotView_);

    if (syncPlotView_ && analysisResult_.valid) {
        plotTimeOffset_ = static_cast<float>(-analysisResult_.latencyDiffMicroseconds / 1000.0);
    }
    else {
        plotTimeOffset_ = 0.0f;
    }

    // Calculate plot size - use available space minus room for result panel and status bar
    float reservedHeight = 80.0f; // Space for result panel and status bar
    ImVec2 availableSize = ImGui::GetContentRegionAvail();
    float plotHeight = (std::max)(200.0f, availableSize.y - reservedHeight);
    ImVec2 plotSize(-1, plotHeight);

    if (ImPlot::BeginPlot("Velocity vs Time", plotSize)) {
        ImPlot::SetupAxes("Time (ms)", "Velocity (counts)");

        // During recording, auto-fit X axis to show all data
        if (state_ == AppState::Recording) {
            // Find max time from live data
            double maxTime = 100.0; // minimum range
            if (!liveTimesA_.empty()) {
                maxTime = (std::max)(maxTime, liveTimesA_.back());
            }
            if (!liveTimesB_.empty()) {
                maxTime = (std::max)(maxTime, liveTimesB_.back());
            }
            // Add 10% padding
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, maxTime * 1.1, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImGuiCond_Once);
        }
        else {
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 1000, ImGuiCond_Once);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImGuiCond_Once);
        }

        // During recording, show live data
        if (state_ == AppState::Recording) {
            // Plot live Mouse A data
            if (!liveTimesA_.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), 2.0f);
                ImPlot::PlotLine("Mouse A (Reference)", liveTimesA_.data(), liveVelocitiesA_.data(),
                                static_cast<int>(liveTimesA_.size()));
            }

            // Plot live Mouse B data
            if (!liveTimesB_.empty()) {
                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), 2.0f);
                ImPlot::PlotLine("Mouse B (Test)", liveTimesB_.data(), liveVelocitiesB_.data(),
                                static_cast<int>(liveTimesB_.size()));
            }
        }
        else {
            // Plot analysis result data
            // Plot Mouse A data
            if (!analysisResult_.mouseAData.empty()) {
                std::vector<double> timesA, velsA;
                timesA.reserve(analysisResult_.mouseAData.size());
                velsA.reserve(analysisResult_.mouseAData.size());

                for (const auto& p : analysisResult_.mouseAData) {
                    timesA.push_back(p.timeMs);
                    velsA.push_back(p.velocity);
                }

                ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), 2.0f);
                ImPlot::PlotLine("Mouse A (Reference)", timesA.data(), velsA.data(),
                                static_cast<int>(timesA.size()));
            }

            // Plot Mouse B data
            if (!analysisResult_.mouseBData.empty()) {
                std::vector<double> timesB, velsB;
                timesB.reserve(analysisResult_.mouseBData.size());
                velsB.reserve(analysisResult_.mouseBData.size());

                for (const auto& p : analysisResult_.mouseBData) {
                    timesB.push_back(p.timeMs + plotTimeOffset_);
                    velsB.push_back(p.velocity);
                }

                ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), 2.0f);
                ImPlot::PlotLine("Mouse B (Test)", timesB.data(), velsB.data(),
                                static_cast<int>(timesB.size()));
            }

            // Draw impact markers
            if (analysisResult_.valid) {
                double impactTimeA = 0.0;

                // Find impact times from timestamps
                for (const auto& p : analysisResult_.mouseAData) {
                    // Approximate - just use the time from data
                    impactTimeA = p.timeMs;
                    break;
                }

                ImPlot::SetNextLineStyle(ImVec4(0.2f, 1.0f, 0.2f, 0.8f), 1.0f);
                double impactX[2] = { impactTimeA, impactTimeA };
                double impactY[2] = { 0, 1000 };
                ImPlot::PlotLine("##ImpactA", impactX, impactY, 2);
            }
        }

        ImPlot::EndPlot();
    }
}

double App::CalculateVelocity(int32_t dx, int32_t dy) {
    return std::sqrt(static_cast<double>(dx * dx + dy * dy));
}

void App::RenderResultPanel() {
    ImGui::Separator();
    ImGui::Text("Analysis Result");

    if (analysisResult_.valid) {
        double latencyUs = analysisResult_.latencyDiffMicroseconds;
        const char* comparison = latencyUs < 0 ? "FASTER" : "SLOWER";
        ImVec4 color = latencyUs < 0 ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.8f, 0.2f, 0.2f, 1.0f);

        ImGui::TextColored(color, "Mouse B is %.2f us %s than Mouse A",
                          std::abs(latencyUs), comparison);

        ImGui::Text("(%.3f ms)", std::abs(latencyUs) / 1000.0);
    }
    else if (!analysisResult_.errorMessage.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Analysis Error:");
        ImGui::TextWrapped("%s", analysisResult_.errorMessage.c_str());
    }
}

void App::RenderStatusBar() {
    ImGui::Separator();

    // State indicator
    const char* stateStr = "Unknown";
    ImVec4 stateColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);

    switch (state_) {
        case AppState::Idle:
            stateStr = "IDLE";
            stateColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            break;
        case AppState::AssigningMouseA:
        case AppState::AssigningMouseB:
            stateStr = "ASSIGNING";
            stateColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            break;
        case AppState::Ready:
            stateStr = "READY";
            stateColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            break;
        case AppState::Recording:
            stateStr = "RECORDING";
            stateColor = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
            break;
        case AppState::Analyzing:
            stateStr = "ANALYZING";
            stateColor = ImVec4(0.2f, 0.6f, 1.0f, 1.0f);
            break;
        case AppState::ShowingResults:
            stateStr = "RESULTS";
            stateColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            break;
    }

    ImGui::TextColored(stateColor, "[%s]", stateStr);
    ImGui::SameLine();
    ImGui::Text("%s", statusMessage_.c_str());
}

void App::TransitionTo(AppState newState) {
    state_ = newState;
}

void App::StartRecording() {
    currentSession_ = RecordingSession{};

    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);
    currentSession_.startTimestamp = timestamp.QuadPart;
    currentSession_.qpcFrequency = static_cast<double>(qpcFrequency_);

    analysisResult_ = AnalysisResult{}; // Clear previous results

    // Clear live plotting data
    liveTimesA_.clear();
    liveVelocitiesA_.clear();
    liveTimesB_.clear();
    liveVelocitiesB_.clear();

    TransitionTo(AppState::Recording);
    statusMessage_ = "Recording... Perform the bump test, then click Stop";
}

void App::StopRecording() {
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);
    currentSession_.endTimestamp = timestamp.QuadPart;

    statusMessage_ = std::format("Recording stopped. A={} events, B={} events. Analyzing...",
                                 currentSession_.eventsA.size(),
                                 currentSession_.eventsB.size());

    // Auto-analyze after stopping
    RunAnalysis();
}

void App::RunAnalysis() {
    TransitionTo(AppState::Analyzing);
    statusMessage_ = "Analyzing...";

    analysisResult_ = analyzer_->Analyze(currentSession_);

    if (analysisResult_.valid) {
        double latencyUs = analysisResult_.latencyDiffMicroseconds;
        const char* comparison = latencyUs < 0 ? "faster" : "slower";
        statusMessage_ = std::format("Analysis complete: Mouse B is {:.2f} us {} than Mouse A",
                                    std::abs(latencyUs), comparison);
        TransitionTo(AppState::ShowingResults);
    }
    else {
        statusMessage_ = "Analysis failed: " + analysisResult_.errorMessage;
        TransitionTo(AppState::Ready);
    }
}

void App::OnResize(int width, int height) {
    if (renderer_) {
        renderer_->OnResize(width, height);
    }
}

void App::OnRawInput(LPARAM lParam) {
    if (inputEngine_) {
        inputEngine_->ProcessRawInput(lParam);
    }
}

std::string App::GetMouseDisplayName(const MouseDevice* device) const {
    if (!device) return "Unknown";

    // Convert wide string to narrow
    std::string name;
    for (wchar_t c : device->name) {
        if (c < 128) {
            name += static_cast<char>(c);
        }
    }

    return name.empty() ? "Mouse Device" : name;
}

void App::SaveSession() {
    if (currentSession_.eventsA.empty() && currentSession_.eventsB.empty()) {
        statusMessage_ = "No data to save";
        return;
    }

    OPENFILENAMEA ofn{};
    char filename[MAX_PATH] = "session.json";

    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "json";

    if (GetSaveFileNameA(&ofn)) {
        auto* deviceA = deviceManager_->GetMouseADevice();
        auto* deviceB = deviceManager_->GetMouseBDevice();

        std::wstring nameA = deviceA ? deviceA->name : L"Unknown";
        std::wstring nameB = deviceB ? deviceB->name : L"Unknown";

        if (dataStore_->SaveToJson(filename, currentSession_, analysisResult_, nameA, nameB)) {
            statusMessage_ = "Session saved successfully";
        }
        else {
            statusMessage_ = "Save failed: " + dataStore_->GetLastError();
        }
    }
}

void App::LoadSession() {
    OPENFILENAMEA ofn{};
    char filename[MAX_PATH] = "";

    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameA(&ofn)) {
        auto session = dataStore_->LoadFromJson(filename);
        if (session) {
            currentSession_ = *session;
            analysisResult_ = AnalysisResult{}; // Clear analysis
            TransitionTo(AppState::Ready);
            statusMessage_ = std::format("Session loaded. A={} events, B={} events",
                                        currentSession_.eventsA.size(),
                                        currentSession_.eventsB.size());
        }
        else {
            statusMessage_ = "Load failed: " + dataStore_->GetLastError();
        }
    }
}

void App::ExportCsv() {
    if (currentSession_.eventsA.empty() && currentSession_.eventsB.empty()) {
        statusMessage_ = "No data to export";
        return;
    }

    OPENFILENAMEA ofn{};
    char filename[MAX_PATH] = "session.csv";

    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "csv";

    if (GetSaveFileNameA(&ofn)) {
        if (dataStore_->ExportToCsv(filename, currentSession_)) {
            statusMessage_ = "CSV exported successfully";
        }
        else {
            statusMessage_ = "Export failed: " + dataStore_->GetLastError();
        }
    }
}

} // namespace RLA
