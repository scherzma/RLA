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

App::~App() { ReleaseRecordingCursor(); }

bool App::Initialize(HWND hwnd, int width, int height) {
    hwnd_ = hwnd;
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

    // Initialize input engine (creates its own dedicated thread for accurate timestamps)
    if (!inputEngine_->Initialize()) {
        statusMessage_ = "Failed to initialize input engine";
        return false;
    }

    inputEngine_->Start();
    statusMessage_ = "Ready - Assign mice to begin";

    return true;
}

void App::Update() {
    UpdateRecordingCursor();
    // Process input events
    inputEngine_->ProcessEvents([this](const MouseEvent& event) {
        // Handle device assignment
        if (deviceManager_->IsAssigning()) {
            if (deviceManager_->ProcessEvent(event)) {
                // Assignment completed
                if (deviceManager_->IsMouseAAssigned() && !deviceManager_->IsMouseBAssigned() &&
                    state_ == AppState::AssigningMouseA) {
                    statusMessage_ = "Mouse A assigned - Ready to record (or assign Mouse B)";
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
        else if (state_ == AppState::Recording &&
                 event.timestamp >= currentSession_.startTimestamp &&
                 (currentSession_.endTimestamp == 0 || event.timestamp <= currentSession_.endTimestamp)) {
            double timeMs = (event.timestamp - currentSession_.startTimestamp) * 1000.0 / currentSession_.qpcFrequency;
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
    if (state_ == AppState::Recording) currentSession_.capture = inputEngine_->GetCaptureDiagnostics();
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
            ImGui::BeginDisabled(state_ == AppState::Recording);
            if (ImGui::MenuItem("Save Session...")) {
                SaveSession();
            }
            if (ImGui::MenuItem("Load Session...")) {
                LoadSession();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export CSV...")) {
                ExportCsv();
            }
            ImGui::EndDisabled();
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
    ImGui::SetNextItemOpen(!deviceManager_->IsMouseAAssigned() && !deviceManager_->IsMouseBAssigned(), ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Device assignment")) RenderDevicePanel();
    RenderControlPanel();
    ImGui::Separator();

    if (showPlotWindow_) {
        RenderPlotPanel();
    }

    RenderStatusBar();

    ImGui::End();
}

void App::RenderDevicePanel() {
    ImGui::BeginDisabled(state_ == AppState::Recording);

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
    ImGui::EndDisabled();
}

void App::RenderControlPanel() {

    bool canRecord = deviceManager_->IsMouseAAssigned() || deviceManager_->IsMouseBAssigned();
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

    // Buffer utilization
    ImGui::SameLine();
    float utilization = inputEngine_->GetBufferUtilization() * 100.0f;
    ImGui::Text("Buffer: %.1f%%", utilization);
    ImGui::SameLine();
    if (ImGui::Checkbox("Keep cursor in window", &keepCursorInWindow_)) UpdateRecordingCursor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps the pointer over RLA while recording. Releases it on stop or focus loss.\nThis controls which window receives ordinary cursor input; raw movement counts are unchanged.");
}

void App::ApplyAutoScaleB() {
    const auto fit = Analyzer::FitScaleB(currentSession_);
    autoScaleMessage_ = fit.message;
    if (!fit.valid) return;
    yScaleB_ = static_cast<float>(fit.scale);
    enableYScaleB_ = true;
    // Equal time bins make count amplitudes comparable at different event rates.
    enableTimeBinning_ = true;
    timeWeightedBins_ = true;
    timeBinMs_ = static_cast<float>(fit.binMs);
    autoScaleMessage_ = std::format("Auto: x{:.3f}. Used {} of {} sections ({:.0f} ms each). Weighted bins: {:.0f} ms.",
        fit.scale, fit.matchedWindows, fit.testedWindows, fit.windowMs, fit.binMs);
}

void App::RenderScaleControls() {
    if (ImGui::Checkbox("Scale B", &enableYScaleB_)) autoScaleMessage_.clear();
    if (enableYScaleB_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderFloat("##YScaleB", &yScaleB_, 0.01f, 100.0f, "x%.3f", ImGuiSliderFlags_Logarithmic))
            autoScaleMessage_.clear();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(75);
        if (ImGui::InputFloat("##YScaleBInput", &yScaleB_, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
            yScaleB_ = std::isfinite(yScaleB_) ? std::clamp(yScaleB_, 0.01f, 100.0f) : 1.0f;
            autoScaleMessage_.clear();
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(state_ == AppState::Recording || currentSession_.eventsA.empty() || currentSession_.eventsB.empty());
    if (ImGui::Button("Auto Scale B")) ApplyAutoScaleB();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Stop recording, then fit B's movement magnitude to A. Direction and shape select matching sections.\n"
            "Rejects idle, conflicting and weak matches. Needs at least three matching sections.\n"
            "Enables time-weighted bins (2 ms or longer for slower mice). Original counts and timestamps stay unchanged.");
    }
    if (!autoScaleMessage_.empty()) ImGui::TextWrapped("%s", autoScaleMessage_.c_str());
}

void App::RenderTimingPanel() {
    ImGui::SetNextItemWidth(155);
    const char* graphs[] = { "Both graphs", "Intervals only", "Hz only" };
    if (ImGui::Combo("##TimingGraph", &timingGraph_, graphs, 3)) timingFitPending_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show one graph at full height, or drag the divider between both graphs to resize them.");
    ImGui::SameLine();
    ImGui::Checkbox("Settings", &showTimingSettings_);
    ImGui::SameLine();
    ImGui::Checkbox("Summary", &showTimingSummary_);
    ImGui::SameLine();
    ImGui::Checkbox("Raw points", &showTimingRaw_);
    ImGui::SameLine();
    ImGui::Checkbox("Follow recording", &followTiming_);
    ImGui::SameLine();
    if (ImGui::Button("Reset timing view")) timingFitPending_ = true;
    if (showTimingSettings_) {
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderFloat("Rate window", &rateWindowMs_, 10.0f, 500.0f, "%.0f ms")) timingDirty_ = true;
        ImGui::SameLine();
        ImGui::Checkbox("Show 1000 / interval Hz", &showInstantHz_);
        ImGui::SetNextItemWidth(180);
        ImGui::SliderFloat("Point opacity", &timingRawOpacity_, 0.05f, 1.0f, "%.2f");
        ImGui::SetNextItemWidth(120);
        if (ImGui::SliderFloat("Long gap", &timingGapMs_, 2.0f, 1000.0f, "> %.0f ms", ImGuiSliderFlags_Logarithmic)) {
            timingDirty_ = true;
            timingFitPending_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Show long gaps", &showTimingGaps_)) timingFitPending_ = true;
        ImGui::TextWrapped("Recorded movement events only. Long gaps break the mean curve; the event rate still includes idle time.");
    }

    const double now = ImGui::GetTime();
    if (timingDirty_ || (state_ == AppState::Recording && now - lastTimingUpdate_ >= 0.1)) {
        int64_t end = currentSession_.endTimestamp;
        if (state_ == AppState::Recording) {
            LARGE_INTEGER counter;
            QueryPerformanceCounter(&counter);
            end = counter.QuadPart;
        }
        const double endMs = currentSession_.qpcFrequency > 0 ?
            (end - currentSession_.startTimestamp) * 1000.0 / currentSession_.qpcFrequency : 0;
        timingEndMs_ = (std::max)(1.0, endMs);
        timingA_ = Analyzer::BuildEventTiming(currentSession_.eventsA, currentSession_.startTimestamp,
            currentSession_.qpcFrequency, endMs, rateWindowMs_, timingGapMs_);
        timingB_ = Analyzer::BuildEventTiming(currentSession_.eventsB, currentSession_.startTimestamp,
            currentSession_.qpcFrequency, endMs, rateWindowMs_, timingGapMs_);
        timingDirty_ = false;
        lastTimingUpdate_ = now;
    }
    if (showTimingSummary_) {
        if (ImGui::BeginTable("Timing summary", 7, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
            for (const char* label : { "Mouse", "Events", "Median ms", "P95 ms", "Max gap ms", "Long gaps", "Intervals <= 0" })
                ImGui::TableSetupColumn(label);
            ImGui::TableHeadersRow();
            auto row = [](const char* label, size_t count, const EventTiming& data) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
                ImGui::TableNextColumn(); ImGui::Text("%zu", count);
                for (double value : { data.medianMs, data.p95Ms, data.maxMs }) {
                    ImGui::TableNextColumn();
                    if (value > 0) ImGui::Text("%.3f", value); else ImGui::TextUnformatted("--");
                }
                ImGui::TableNextColumn(); ImGui::Text("%zu", data.longGapCount);
                ImGui::TableNextColumn(); ImGui::Text("%zu", data.nonPositiveIntervals);
            };
            row("A (Reference)", currentSession_.eventsA.size(), timingA_);
            row("B (Test)", currentSession_.eventsB.size(), timingB_);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Statistics include long gaps. Arrival times are measured in the application, not at the USB device.");
        const auto& capture = currentSession_.capture;
        if (capture.available) {
            ImGui::TextWrapped("Buffered capture totals since app start: %llu mouse reports, %llu read in groups, largest batch %llu. Queue drops: %llu. Read errors: %llu (last code %u).",
                capture.packets, capture.groupedPackets, capture.maxBatch, capture.droppedEvents, capture.readErrors, capture.lastError);
            ImGui::TextWrapped("Reports in one batch share a read timestamp. Their individual arrival intervals are unknown; the window rate still counts each movement event.");
        }
    }
    if (currentSession_.eventsA.empty() && currentSession_.eventsB.empty()) {
        ImGui::TextUnformatted("Record movement or load a session to see event timing.");
        return;
    }
    const float height = (std::max)(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
    const ImPlotAxisFlags axisFlags = state_ == AppState::Recording && followTiming_ ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;
    auto setupAxes = [&](const char* yLabel) {
        ImPlot::SetupAxes("Time (ms)", yLabel, ImPlotAxisFlags_None, axisFlags);
        if (timingFitPending_ || (state_ == AppState::Recording && followTiming_))
            ImPlot::SetupAxisLimits(ImAxis_X1, -timingEndMs_ * 0.03, timingEndMs_ * 1.03, ImPlotCond_Always);
    };
    if (ImPlot::BeginSubplots("Event timing", timingGraph_ == 0 ? 2 : 1, 1, ImVec2(-1, height),
        ImPlotSubplotFlags_LinkCols | ImPlotSubplotFlags_NoTitle)) {
        const ImVec4 colorA(0.2f, 0.6f, 1.0f, 1.0f), colorB(1.0f, 0.4f, 0.2f, 1.0f);
        const ImVec4 meanA(0.55f, 0.85f, 1.0f, 1.0f), meanB(1.0f, 0.85f, 0.45f, 1.0f);
        auto scatter = [&](const char* label, const EventTiming& data, const std::vector<double>& values, ImVec4 color) {
            if (!showTimingRaw_ || data.timesMs.empty()) return;
            color.w = timingRawOpacity_;
            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 1.5f, color, 0.0f, color);
            ImPlot::PlotScatter(label, data.timesMs.data(), values.data(), static_cast<int>(data.timesMs.size()));
        };
        if (timingGraph_ != 2) {
            if (timingFitPending_) ImPlot::SetNextAxisToFit(ImAxis_Y1);
            if (ImPlot::BeginPlot("Time between events")) {
                setupAxes("Interval (ms)");
                // Draw both point clouds first so neither can cover the mean curves.
                scatter("Mouse A - raw", timingA_, showTimingGaps_ ? timingA_.intervalsMs : timingA_.shortIntervalsMs, colorA);
                scatter("Mouse B - raw", timingB_, showTimingGaps_ ? timingB_.intervalsMs : timingB_.shortIntervalsMs, colorB);
                auto plot = [&](const char* meanLabel, const EventTiming& data, ImVec4 color) {
                    if (data.timesMs.empty()) return;
                    ImPlot::SetNextLineStyle(color, 3.0f);
                    ImPlot::PlotLine(meanLabel, data.timesMs.data(), data.meanIntervalsMs.data(), static_cast<int>(data.timesMs.size()));
                };
                plot("Mouse A - mean", timingA_, meanA);
                plot("Mouse B - mean", timingB_, meanB);
                ImPlot::EndPlot();
            }
        }
        if (timingGraph_ != 1) {
            if (timingFitPending_) ImPlot::SetNextAxisToFit(ImAxis_Y1);
            if (ImPlot::BeginPlot("Movement event rate")) {
                setupAxes("Rate (Hz)");
                if (showInstantHz_) {
                    scatter("Mouse A - interval Hz", timingA_, timingA_.instantHz, colorA);
                    scatter("Mouse B - interval Hz", timingB_, timingB_.instantHz, colorB);
                }
                auto plot = [&](const char* label, const EventTiming& data, ImVec4 color) {
                    if (!data.rateTimesMs.empty()) {
                        ImPlot::SetNextLineStyle(color, 3.0f);
                        ImPlot::PlotLine(label, data.rateTimesMs.data(), data.ratesHz.data(), static_cast<int>(data.rateTimesMs.size()));
                    }
                };
                plot("Mouse A - window rate", timingA_, meanA);
                plot("Mouse B - window rate", timingB_, meanB);
                ImPlot::EndPlot();
            }
        }
        ImPlot::EndSubplots();
        timingFitPending_ = false;
    }
}

void App::RenderPlotPanel() {
    // Plot mode selector
    ImGui::Text("Plot Mode:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(210);
    const char* plotModes[] = { "Movement magnitude", "Raw X/Y Deltas", "Event timing / Hz" };
    ImGui::Combo("##PlotMode", &plotMode_, plotModes, 3);
    ImGui::Separator();

    if (plotMode_ == 2) {
        RenderTimingPanel();
        return;
    }
    RenderScaleControls();

    // Measure after controls so the graph uses the remaining height.
    auto plotSize = []() {
        return ImVec2(-1, (std::max)(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()));
    };

    if (plotMode_ == 0) {
        // ==================== VELOCITY MODE ====================
        // Velocity mode controls
        ImGui::Checkbox("Smoothing", &enableSmoothing_);
        if (enableSmoothing_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            const char* modes[] = { "Samples", "Time (ms)" };
            ImGui::Combo("##SmoothMode", &smoothingMode_, modes, 2);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (smoothingMode_ == 0) {
                ImGui::SliderInt("##SmoothSamples", &smoothingSamples_, 2, 50, "%d pts");
            } else {
                ImGui::SliderFloat("##SmoothTime", &smoothingTimeMs_, 0.1f, 10.0f, "%.1f ms");
            }
        }

        ImGui::Checkbox("Time Binning", &enableTimeBinning_);
        if (enableTimeBinning_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderFloat("##BinSize", &timeBinMs_, 0.1f, 50.0f, "%.1f ms", ImGuiSliderFlags_Logarithmic);
            ImGui::SameLine();
            if (ImGui::Checkbox("Weight by time", &timeWeightedBins_)) autoScaleMessage_.clear();
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Weight by time estimates uniform movement between nearby reports and splits counts across bin boundaries.\n"
                    "First reports and reports after long gaps stay at arrival time. Turn off to sum whole events.\n"
                    "Binning changes the plotted time resolution. Use raw events to inspect exact arrival timing.");
            }
        }

        ImGui::Checkbox("Fill Gaps", &enableGapInterpolation_);
        if (enableGapInterpolation_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderFloat("##GapThreshold", &gapThresholdMs_, 1.0f, 20.0f, "%.1f ms");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Fills time gaps with zero-velocity points.\nShows stationary periods as velocity=0 instead of gaps.");
            }
        }

        ImGui::SameLine();
        ImGui::Checkbox("Show Markers", &showDataPointMarkers_);
        if (showDataPointMarkers_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::SliderFloat("##MarkerSize", &markerSize_, 1.0f, 8.0f, "%.0f px");
        }

        // Velocity plot
        if (ImPlot::BeginPlot("Movement magnitude vs Time", plotSize())) {
            ImPlot::SetupAxes("Time (ms)", enableTimeBinning_ ? "Counts / bin" : "Counts / event");

            double maxTime = 100.0;
            if (state_ == AppState::Recording) {
                if (!liveTimesA_.empty()) maxTime = (std::max)(maxTime, liveTimesA_.back());
                if (!liveTimesB_.empty()) maxTime = (std::max)(maxTime, liveTimesB_.back());
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, maxTime * 1.1, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImGuiCond_Once);
            } else {
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, 1000, ImGuiCond_Once);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImGuiCond_Once);
            }

            // Get events to plot (works for both recording and stopped states)
            const auto& eventsA = currentSession_.eventsA;
            const auto& eventsB = currentSession_.eventsB;

            // Plot Mouse A
            if (!eventsA.empty()) {
                std::vector<double> plotTimesA, plotVelsA;

                // 1. Apply time binning FIRST if enabled (operates on raw events, sums deltas correctly)
                if (enableTimeBinning_) {
                    ApplyTimeBinning(eventsA, currentSession_.startTimestamp, plotTimesA, plotVelsA, timeBinMs_);
                } else {
                    // Use pre-calculated times/velocities
                    plotTimesA = liveTimesA_;
                    plotVelsA = liveVelocitiesA_;
                }

                // 2. Apply gap interpolation SECOND (fills gaps with zero-velocity points)
                if (enableGapInterpolation_ && !plotTimesA.empty()) {
                    std::vector<double> interpTimes, interpVels;
                    InterpolateGaps(plotTimesA, plotVelsA, interpTimes, interpVels,
                        enableTimeBinning_ ? (std::max)(gapThresholdMs_, timeBinMs_ * 1.5f) : gapThresholdMs_,
                        enableTimeBinning_ ? timeBinMs_ : gapSampleIntervalMs_);
                    plotTimesA = std::move(interpTimes);
                    plotVelsA = std::move(interpVels);
                }

                // 3. Apply smoothing LAST
                if (enableSmoothing_ && !plotTimesA.empty()) {
                    std::vector<double> smoothedTimes, smoothedVels;
                    if (smoothingMode_ == 0) {
                        ApplyMovingAverageSmoothing(plotTimesA, plotVelsA, smoothedTimes, smoothedVels, smoothingSamples_);
                    } else {
                        ApplyTimeWindowSmoothing(plotTimesA, plotVelsA, smoothedTimes, smoothedVels, smoothingTimeMs_);
                    }
                    plotTimesA = std::move(smoothedTimes);
                    plotVelsA = std::move(smoothedVels);
                }

                if (!plotTimesA.empty()) {
                    ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), 2.0f);
                    if (showDataPointMarkers_) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, markerSize_, ImVec4(0.2f, 0.6f, 1.0f, 1.0f), 1.0f);
                    }
                    ImPlot::PlotLine("Mouse A (Reference)", plotTimesA.data(), plotVelsA.data(), static_cast<int>(plotTimesA.size()));
                }
            }

            // Plot Mouse B
            if (!eventsB.empty()) {
                std::vector<double> plotTimesB, plotVelsB;

                // 1. Apply time binning FIRST if enabled (operates on raw events, sums deltas correctly)
                if (enableTimeBinning_) {
                    ApplyTimeBinning(eventsB, currentSession_.startTimestamp, plotTimesB, plotVelsB, timeBinMs_);
                } else {
                    // Use pre-calculated times/velocities
                    plotTimesB = liveTimesB_;
                    plotVelsB = liveVelocitiesB_;
                }

                // 2. Apply gap interpolation SECOND (fills gaps with zero-velocity points)
                if (enableGapInterpolation_ && !plotTimesB.empty()) {
                    std::vector<double> interpTimes, interpVels;
                    InterpolateGaps(plotTimesB, plotVelsB, interpTimes, interpVels,
                        enableTimeBinning_ ? (std::max)(gapThresholdMs_, timeBinMs_ * 1.5f) : gapThresholdMs_,
                        enableTimeBinning_ ? timeBinMs_ : gapSampleIntervalMs_);
                    plotTimesB = std::move(interpTimes);
                    plotVelsB = std::move(interpVels);
                }

                // 3. Apply smoothing LAST
                if (enableSmoothing_ && !plotTimesB.empty()) {
                    std::vector<double> smoothedTimes, smoothedVels;
                    if (smoothingMode_ == 0) {
                        ApplyMovingAverageSmoothing(plotTimesB, plotVelsB, smoothedTimes, smoothedVels, smoothingSamples_);
                    } else {
                        ApplyTimeWindowSmoothing(plotTimesB, plotVelsB, smoothedTimes, smoothedVels, smoothingTimeMs_);
                    }
                    plotTimesB = std::move(smoothedTimes);
                    plotVelsB = std::move(smoothedVels);
                }

                if (enableYScaleB_ && yScaleB_ != 1.0f) {
                    for (auto& v : plotVelsB) v *= yScaleB_;
                }

                if (!plotTimesB.empty()) {
                    ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), 2.0f);
                    if (showDataPointMarkers_) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, markerSize_, ImVec4(1.0f, 0.4f, 0.2f, 1.0f), 1.0f);
                    }
                    ImPlot::PlotLine("Mouse B (Test)", plotTimesB.data(), plotVelsB.data(), static_cast<int>(plotTimesB.size()));
                }
            }

            ImPlot::EndPlot();
        }
    }
    else {
        // ==================== RAW X/Y DELTAS MODE ====================
        // Same controls as velocity mode
        ImGui::Checkbox("Smoothing", &enableSmoothing_);
        if (enableSmoothing_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            const char* modes[] = { "Samples", "Time (ms)" };
            ImGui::Combo("##SmoothModeXY", &smoothingMode_, modes, 2);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            if (smoothingMode_ == 0) {
                ImGui::SliderInt("##SmoothSamplesXY", &smoothingSamples_, 2, 50, "%d pts");
            } else {
                ImGui::SliderFloat("##SmoothTimeXY", &smoothingTimeMs_, 0.1f, 10.0f, "%.1f ms");
            }
        }

        ImGui::Checkbox("Time Binning", &enableTimeBinning_);
        if (enableTimeBinning_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderFloat("##BinSizeXY", &timeBinMs_, 0.1f, 50.0f, "%.1f ms", ImGuiSliderFlags_Logarithmic);
            ImGui::SameLine();
            if (ImGui::Checkbox("Weight by time", &timeWeightedBins_)) autoScaleMessage_.clear();
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Weight by time splits X/Y counts across bins using the interval between nearby reports.\n"
                    "This estimates uniform movement within each interval. Turn off to sum whole events.\n"
                    "Binning changes the plotted time resolution. Use raw events to inspect exact arrival timing.");
            }
        }

        ImGui::Checkbox("Fill Gaps", &enableGapInterpolation_);
        if (enableGapInterpolation_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderFloat("##GapThresholdXY", &gapThresholdMs_, 1.0f, 20.0f, "%.1f ms");
        }

        ImGui::SameLine();
        ImGui::Checkbox("Show Markers", &showDataPointMarkers_);
        if (showDataPointMarkers_) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60);
            ImGui::SliderFloat("##MarkerSizeXY", &markerSize_, 1.0f, 8.0f, "%.0f px");
        }

        // Raw X/Y plot
        if (ImPlot::BeginPlot("Raw X/Y Deltas vs Time", plotSize())) {
            ImPlot::SetupAxes("Time (ms)", "Delta (counts)");

            double maxTime = 100.0;
            if (state_ == AppState::Recording) {
                if (!liveTimesA_.empty()) maxTime = (std::max)(maxTime, liveTimesA_.back());
                if (!liveTimesB_.empty()) maxTime = (std::max)(maxTime, liveTimesB_.back());
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, maxTime * 1.1, ImPlotCond_Always);
            } else {
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, 1000, ImGuiCond_Once);
            }
            ImPlot::SetupAxisLimits(ImAxis_Y1, -50, 50, ImGuiCond_Once);

            const auto& eventsA = currentSession_.eventsA;
            const auto& eventsB = currentSession_.eventsB;

            // Helper lambda to apply processing to X/Y data
            auto processXYData = [this](const std::vector<MouseEvent>& events,
                                        std::vector<double>& outTimes,
                                        std::vector<double>& outX,
                                        std::vector<double>& outY) {
                if (events.empty()) return;

                // Extract raw data
                std::vector<double> rawTimes, rawX, rawY;
                rawTimes.reserve(events.size());
                rawX.reserve(events.size());
                rawY.reserve(events.size());

                if (enableTimeBinning_) {
                    for (const auto& bin : Analyzer::BinMovement(events, currentSession_.startTimestamp,
                        currentSession_.qpcFrequency, timeBinMs_, timeWeightedBins_)) {
                        rawTimes.push_back((bin.index + 0.5) * timeBinMs_);
                        rawX.push_back(bin.x);
                        rawY.push_back(bin.y);
                    }
                } else {
                    for (const auto& event : events) {
                        double timeMs = (event.timestamp - currentSession_.startTimestamp) * 1000.0 / currentSession_.qpcFrequency;
                        rawTimes.push_back(timeMs);
                        rawX.push_back(static_cast<double>(event.deltaX));
                        rawY.push_back(static_cast<double>(event.deltaY));
                    }
                }

                // Fill missing bins only after resampling, as in magnitude mode.
                if (enableGapInterpolation_ && !rawTimes.empty()) {
                    const double threshold = enableTimeBinning_ ? (std::max)(gapThresholdMs_, timeBinMs_ * 1.5f) : gapThresholdMs_;
                    const double step = enableTimeBinning_ ? timeBinMs_ : gapSampleIntervalMs_;
                    std::vector<double> interpTimes, interpX, interpY;
                    for (size_t i = 0; i < rawTimes.size(); ++i) {
                        if (i > 0) {
                            double gap = rawTimes[i] - rawTimes[i - 1];
                            if (gap > threshold) {
                                double t = rawTimes[i - 1] + step;
                                while (t < rawTimes[i] - step / 2.0) {
                                    interpTimes.push_back(t);
                                    interpX.push_back(0.0);
                                    interpY.push_back(0.0);
                                    t += step;
                                }
                            }
                        }
                        interpTimes.push_back(rawTimes[i]);
                        interpX.push_back(rawX[i]);
                        interpY.push_back(rawY[i]);
                    }
                    rawTimes = std::move(interpTimes);
                    rawX = std::move(interpX);
                    rawY = std::move(interpY);
                }

                // 3. Apply smoothing LAST
                if (enableSmoothing_ && !rawTimes.empty()) {
                    std::vector<double> smoothedTimes, smoothedX, smoothedY;
                    if (smoothingMode_ == 0) {
                        ApplyMovingAverageSmoothing(rawTimes, rawX, smoothedTimes, smoothedX, smoothingSamples_);
                        std::vector<double> dummy;
                        ApplyMovingAverageSmoothing(rawTimes, rawY, dummy, smoothedY, smoothingSamples_);
                    } else {
                        ApplyTimeWindowSmoothing(rawTimes, rawX, smoothedTimes, smoothedX, smoothingTimeMs_);
                        std::vector<double> dummy;
                        ApplyTimeWindowSmoothing(rawTimes, rawY, dummy, smoothedY, smoothingTimeMs_);
                    }
                    rawTimes = std::move(smoothedTimes);
                    rawX = std::move(smoothedX);
                    rawY = std::move(smoothedY);
                }

                outTimes = std::move(rawTimes);
                outX = std::move(rawX);
                outY = std::move(rawY);
            };

            // Plot Mouse A - X and Y deltas
            if (!eventsA.empty()) {
                std::vector<double> timesA, deltasXA, deltasYA;
                processXYData(eventsA, timesA, deltasXA, deltasYA);

                if (!timesA.empty()) {
                    ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), 1.5f);
                    if (showDataPointMarkers_) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, markerSize_, ImVec4(0.2f, 0.6f, 1.0f, 1.0f), 1.0f);
                    }
                    ImPlot::PlotLine("Mouse A - X", timesA.data(), deltasXA.data(), static_cast<int>(timesA.size()));

                    ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.9f, 0.9f, 1.0f), 1.5f);
                    if (showDataPointMarkers_) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, markerSize_, ImVec4(0.2f, 0.9f, 0.9f, 1.0f), 1.0f);
                    }
                    ImPlot::PlotLine("Mouse A - Y", timesA.data(), deltasYA.data(), static_cast<int>(timesA.size()));
                }
            }

            // Plot Mouse B - X and Y deltas
            if (!eventsB.empty()) {
                std::vector<double> timesB, deltasXB, deltasYB;
                processXYData(eventsB, timesB, deltasXB, deltasYB);
                if (enableYScaleB_) {
                    for (auto& value : deltasXB) value *= yScaleB_;
                    for (auto& value : deltasYB) value *= yScaleB_;
                }

                if (!timesB.empty()) {
                    ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), 1.5f);
                    if (showDataPointMarkers_) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, markerSize_, ImVec4(1.0f, 0.4f, 0.2f, 1.0f), 1.0f);
                    }
                    ImPlot::PlotLine("Mouse B - X", timesB.data(), deltasXB.data(), static_cast<int>(timesB.size()));

                    ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), 1.5f);
                    if (showDataPointMarkers_) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, markerSize_, ImVec4(1.0f, 0.7f, 0.2f, 1.0f), 1.0f);
                    }
                    ImPlot::PlotLine("Mouse B - Y", timesB.data(), deltasYB.data(), static_cast<int>(timesB.size()));
                }
            }

            ImPlot::EndPlot();
        }
    }
}

double App::CalculateVelocity(int32_t dx, int32_t dy) {
    return std::hypot(static_cast<double>(dx), static_cast<double>(dy));
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
    deviceManager_->CancelAssignment();
    currentSession_ = RecordingSession{};
    enableYScaleB_ = false;
    yScaleB_ = 1.0f;
    autoScaleMessage_.clear();
    timingDirty_ = true;
    timingFitPending_ = true;
    analysisResult_ = AnalysisResult{};
    inputEngine_->ResetEventRates();

    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);
    currentSession_.startTimestamp = timestamp.QuadPart;
    currentSession_.qpcFrequency = static_cast<double>(qpcFrequency_);

    // Clear live plotting data
    liveTimesA_.clear();
    liveVelocitiesA_.clear();
    liveTimesB_.clear();
    liveVelocitiesB_.clear();

    TransitionTo(AppState::Recording);
    UpdateRecordingCursor();
    statusMessage_ = "Recording... Move both mice simultaneously, then click Stop";
}

void App::StopRecording() {
    LARGE_INTEGER timestamp;
    QueryPerformanceCounter(&timestamp);
    currentSession_.endTimestamp = timestamp.QuadPart;
    // Include queued events up to the stop timestamp.
    Update();
    timingDirty_ = true;

    statusMessage_ = std::format("Recording stopped. A={} events, B={} events",
                                 currentSession_.eventsA.size(),
                                 currentSession_.eventsB.size());

    TransitionTo(AppState::Ready);
    ReleaseRecordingCursor();
}

void App::UpdateRecordingCursor() {
    if (!hwnd_ || state_ != AppState::Recording || !keepCursorInWindow_ || GetForegroundWindow() != hwnd_) {
        ReleaseRecordingCursor();
        return;
    }
    RECT client{};
    POINT origin{};
    if (!GetClientRect(hwnd_, &client) || !ClientToScreen(hwnd_, &origin) ||
        client.right <= client.left || client.bottom <= client.top) {
        ReleaseRecordingCursor();
        return;
    }
    OffsetRect(&client, origin.x, origin.y);
    if ((!cursorConfined_ || !EqualRect(&client, &cursorClip_)) && ClipCursor(&client)) {
        cursorClip_ = client;
        cursorConfined_ = true;
    }
}

void App::ReleaseRecordingCursor() {
    if (!cursorConfined_) return;
    RECT current{};
    // Do not remove a newer restriction installed by another application.
    if (GetClipCursor(&current) && EqualRect(&current, &cursorClip_)) ClipCursor(nullptr);
    cursorConfined_ = false;
}

void App::OnResize(int width, int height) {
    if (renderer_) {
        renderer_->OnResize(width, height);
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
            deviceManager_->CancelAssignment();
            currentSession_ = std::move(*session);
            RebuildPlotData();
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

void App::RebuildPlotData() {
    enableYScaleB_ = false;
    yScaleB_ = 1.0f;
    autoScaleMessage_.clear();
    timingDirty_ = true;
    timingFitPending_ = true;
    auto rebuild = [this](const std::vector<MouseEvent>& events,
                          std::vector<double>& times, std::vector<double>& values) {
        times.clear();
        values.clear();
        times.reserve(events.size());
        values.reserve(events.size());
        for (const auto& event : events) {
            times.push_back((event.timestamp - currentSession_.startTimestamp) *
                            1000.0 / currentSession_.qpcFrequency);
            values.push_back(CalculateVelocity(event.deltaX, event.deltaY));
        }
    };
    rebuild(currentSession_.eventsA, liveTimesA_, liveVelocitiesA_);
    rebuild(currentSession_.eventsB, liveTimesB_, liveVelocitiesB_);
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

void App::ApplyMovingAverageSmoothing(const std::vector<double>& times, const std::vector<double>& values,
                                      std::vector<double>& outTimes, std::vector<double>& outValues, int windowSize) {
    outTimes.clear();
    outValues.clear();

    if (times.empty() || windowSize < 1) return;

    int halfWindow = windowSize / 2;

    for (size_t i = 0; i < times.size(); ++i) {
        double sum = 0.0;
        int count = 0;

        for (int j = -halfWindow; j <= halfWindow; ++j) {
            int idx = static_cast<int>(i) + j;
            if (idx >= 0 && idx < static_cast<int>(values.size())) {
                sum += values[idx];
                ++count;
            }
        }

        outTimes.push_back(times[i]);
        outValues.push_back(count > 0 ? sum / count : 0.0);
    }
}

void App::ApplyTimeWindowSmoothing(const std::vector<double>& times, const std::vector<double>& values,
                                   std::vector<double>& outTimes, std::vector<double>& outValues, double windowMs) {
    outTimes.clear();
    outValues.clear();

    if (times.empty() || windowMs <= 0) return;

    size_t i = 0;
    while (i < times.size()) {
        double windowStart = times[i];
        double windowEnd = windowStart + windowMs;

        double sum = 0.0;
        int count = 0;
        double timeSum = 0.0;

        // Gather all points within the time window
        size_t j = i;
        while (j < times.size() && times[j] < windowEnd) {
            sum += values[j];
            timeSum += times[j];
            ++count;
            ++j;
        }

        if (count > 0) {
            outTimes.push_back(timeSum / count);  // Average time
            outValues.push_back(sum / count);     // Average value
        }

        // Move to next window
        i = j > i ? j : i + 1;
    }
}

void App::ApplyTimeBinning(const std::vector<MouseEvent>& events, int64_t startTimestamp,
                           std::vector<double>& outTimes, std::vector<double>& outVelocities, double binMs) {
    outTimes.clear();
    outVelocities.clear();

    for (const auto& bin : Analyzer::BinMovement(events, startTimestamp,
        currentSession_.qpcFrequency, binMs, timeWeightedBins_)) {
        outTimes.push_back((bin.index + 0.5) * binMs);
        outVelocities.push_back(std::hypot(bin.x, bin.y));
    }
}

void App::InterpolateGaps(const std::vector<double>& times, const std::vector<double>& values,
                          std::vector<double>& outTimes, std::vector<double>& outValues,
                          double gapThresholdMs, double sampleIntervalMs) {
    outTimes.clear();
    outValues.clear();

    if (times.empty()) return;

    for (size_t i = 0; i < times.size(); ++i) {
        // Check for gap before this point (except for first point)
        if (i > 0) {
            double gap = times[i] - times[i - 1];
            if (gap > gapThresholdMs) {
                // Insert zero-velocity points to fill the gap
                // Start a bit after the previous point and end a bit before this point
                double t = times[i - 1] + sampleIntervalMs;
                while (t < times[i] - sampleIntervalMs / 2.0) {
                    outTimes.push_back(t);
                    outValues.push_back(0.0);
                    t += sampleIntervalMs;
                }
            }
        }

        // Add the actual data point
        outTimes.push_back(times[i]);
        outValues.push_back(values[i]);
    }
}

} // namespace RLA
