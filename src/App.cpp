#include "App.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <format>

// Windows file dialogs
#include <commdlg.h>
#include <ShlObj.h>
#include <shellapi.h>

namespace RLA {

App::App() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    qpcFrequency_ = freq.QuadPart;
}

App::~App() {
    if (state_ == AppState::Recording && inputEngine_ && deviceManager_) StopRecording();
    if (inputEngine_) inputEngine_->SetRawCapture(false);
    ReleaseRecordingCursor();
    while (librarySaveTask_.valid() || libraryDirty_ || !runsToKeep_.empty()) {
        PollLibrarySave();
        if (librarySaveTask_.valid()) librarySaveTask_.wait();
        else break;
    }
    if (libraryLock_) { ReleaseMutex(libraryLock_); CloseHandle(libraryLock_); }
}

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
    wchar_t localData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, localData))) {
        try { autosave_ = std::make_unique<Autosave>(std::filesystem::path(localData) / L"RLA" / L"Autosaves"); }
        catch (const std::exception& error) { autosaveError_ = error.what(); }
        libraryPath_=std::filesystem::path(localData)/L"RLA"/L"mice.json";
        HANDLE lock=CreateMutexW(nullptr,TRUE,L"Local\\RLA_MouseLibrary");
        if (!lock || GetLastError()==ERROR_ALREADY_EXISTS) {
            if (lock) CloseHandle(lock);
            libraryReadOnly_=true; libraryMessage_="Another RLA instance owns the mouse library. This instance is read-only.";
        } else libraryLock_=lock;
        std::string error;
        if (!mouseLibrary_.Load(libraryPath_,error)) { libraryReadOnly_=true; libraryMessage_="Mouse library could not be loaded: "+error+". The file will not be overwritten."; }
    } else {
        autosaveError_ = "The local application data folder is unavailable.";
        libraryReadOnly_=true; libraryMessage_=autosaveError_;
    }
    statusMessage_ = "Ready - Assign mice to begin";

    return true;
}

void App::Update() {
    PollLatencyAnalysis();
    PollLibrarySave();
    if (autoRankPending_ && state_!=AppState::Recording && !latencyTask_.valid()) {
        autoRankPending_=false; ApplyAutoScaleB(); StartLatencyAnalysis();
    }
    UpdateRecordingCursor();
    const bool stopClick = inputEngine_->TakeStopClick();
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
            const bool preparePlot=currentSession_.captureTestMode<2;
            double velocity = preparePlot ? CalculateVelocity(event.deltaX, event.deltaY) : 0;

            if (deviceManager_->IsMouseA(event.deviceHandle)) {
                currentSession_.eventsA.push_back(event);
                if (preparePlot) { liveTimesA_.push_back(timeMs); liveVelocitiesA_.push_back(velocity); }
            }
            else if (deviceManager_->IsMouseB(event.deviceHandle)) {
                currentSession_.eventsB.push_back(event);
                if (preparePlot) { liveTimesB_.push_back(timeMs); liveVelocitiesB_.push_back(velocity); }
            }
        }
    }, stoppingRecording_ ? inputEngine_->GetBufferSize() : 8192);
    if (state_ == AppState::Recording && !stoppingRecording_ && !currentSession_.captureTestMode) DetectLiveRates();
    if (state_ == AppState::Recording) currentSession_.capture = inputEngine_->GetCaptureDiagnostics();
    const HWND captureWindow=inputEngine_->ForegroundTestWindow() ? inputEngine_->ForegroundTestWindow() : hwnd_;
    if (state_ == AppState::Recording && (inputEngine_->IsRawCapture() || inputEngine_->ForegroundTestWindow()) && !stoppingRecording_ &&
        (stopClick || (captureWindow && GetForegroundWindow() != captureWindow)))
        StopRecording();
    if (state_==AppState::Recording && currentSession_.captureTestMode && !stoppingRecording_) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if ((now.QuadPart-currentSession_.startTimestamp)*1000.0/currentSession_.qpcFrequency>=18000) StopRecording();
    }
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

    RenderDevicePanel();
    RenderControlPanel();
    ImGui::Separator();
    if (state_==AppState::Recording && currentSession_.captureTestMode>=2) {
        ImGui::TextUnformatted("Capture-only test: graph processing is off. All movement reports are still recorded.");
    } else if (showLibrary_) {
        if (ImGui::Button("Back to recording")) showLibrary_=false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Mouse library",libraryPage_==0)) libraryPage_=0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rankings",libraryPage_==1)) libraryPage_=1;
        ImGui::BeginChild("Mouse library page",ImVec2(0,ImGui::GetContentRegionAvail().y-ImGui::GetFrameHeightWithSpacing()));
        RenderMouseLibrary(); ImGui::EndChild();
    } else if (showPlotWindow_) RenderPlotPanel();

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
        ImGui::SameLine();
        if (ImGui::Button("Mouse settings##A")) ImGui::OpenPopup("Reference mouse settings");
        if (ImGui::BeginPopup("Reference mouse settings")) {
            RenderDeviceBinding("Reference A",deviceA);
            ImGui::EndPopup();
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
        ImGui::SameLine();
        if (ImGui::Button("Mouse settings##B")) ImGui::OpenPopup("Test mouse settings");
        if (ImGui::BeginPopup("Test mouse settings")) {
            RenderDeviceBinding("Test B",deviceB);
            ImGui::EndPopup();
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
    ImGui::SameLine();
    if (ImGui::Button("Library / rankings")) { showLibrary_=!showLibrary_; libraryPage_=1; }
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
        if (inputEngine_->IsRawCapture()) {
            ImGui::SameLine();
            ImGui::TextUnformatted("Click or Esc to stop");
        }
        ImGui::SameLine();
        ImGui::Text("Events: A=%zu B=%zu",
                   currentSession_.eventsA.size(),
                   currentSession_.eventsB.size());

        // Show live polling rates
        inputEngine_->UpdateEventRates(currentSession_.eventsA.size(), currentSession_.eventsB.size());
        double rateA = inputEngine_->GetEventRateA();
        double rateB = inputEngine_->GetEventRateB();

        ImGui::SameLine();
        ImGui::Text("| Events/s: A=%.0f B=%.0f", rateA, rateB);
        ImGui::SameLine();
        ImGui::Text("| Highest Hz: A=%s B=%s",
            currentSession_.mouseA.pollingHz ? std::to_string(currentSession_.mouseA.pollingHz).c_str() : "detecting",
            currentSession_.mouseB.pollingHz ? std::to_string(currentSession_.mouseB.pollingHz).c_str() : "detecting");
    }

    if (!canRecord && !isRecording) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    RenderAutosaveControls();
    ImGui::SameLine();
    if (ImGui::Button("Capture options")) ImGui::OpenPopup("Capture options");
    if (ImGui::BeginPopup("Capture options")) {
        ImGui::BeginDisabled(isRecording);
        ImGui::Checkbox("Raw capture (recommended)", &rawCaptureMode_);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(isRecording && currentSession_.captureTestMode!=0);
        if (ImGui::Checkbox("Keep cursor in window", &keepCursorInWindow_)) UpdateRecordingCursor();
        ImGui::EndDisabled();
        ImGui::Text("Buffer: %.1f%%", inputEngine_->GetBufferUtilization()*100.0f);
        ImGui::TextUnformatted("Raw capture: click or press Esc to stop.");
        ImGui::EndPopup();
    }

    RenderCaptureTest();
    if (state_ != AppState::Recording && (!currentSession_.eventsA.empty() || !currentSession_.eventsB.empty())) {
        const auto recordedRate=[](const RecordingMouse& mouse, bool hasEvents) {
            if (!hasEvents) return std::string("--");
            return mouse.pollingHz > 0 ? std::to_string(mouse.pollingHz) + " Hz" : std::string("Unknown");
        };
        ImGui::Text("Recorded rate: A %s | B %s",
            recordedRate(currentSession_.mouseA, !currentSession_.eventsA.empty()).c_str(),
            recordedRate(currentSession_.mouseB, !currentSession_.eventsB.empty()).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rates stored with this recording, not the currently connected mice.");
    }
}

void App::RenderCaptureTest() {
    ImGui::SameLine();
    ImGui::BeginDisabled(state_==AppState::Recording || latencyTask_.valid());
    if (ImGui::Button("Capture test")) ImGui::OpenPopup("Capture test settings");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("Capture test settings")) {
        ImGui::TextUnformatted("Compare the same motion with and without live plotting.");
        ImGui::RadioButton("Live plot (baseline)",&captureTestChoice_,1);
        ImGui::RadioButton("Hidden window / capture only",&captureTestChoice_,2);
        ImGui::RadioButton("Foreground window / capture only",&captureTestChoice_,3);
        ImGui::TextUnformatted("Foreground mode opens a separate window with the stage instructions.");
        ImGui::TextUnformatted("18 seconds: prepare 3s, A only 5s, both 5s, A only 5s.");
        ImGui::TextUnformatted("Keep polling rates, DPI, USB ports and movement speed unchanged.");
        ImGui::TextUnformatted("Repeat once in each mode. Keep RLA in the foreground.");
        ImGui::TextUnformatted("Run only ONE RLA instance. Close other RLA windows first.");
        ImGui::TextUnformatted("Each test autosaves. Tests do not change mouse rates or rankings.");
        ImGui::BeginDisabled(!deviceManager_->IsMouseAAssigned() || !deviceManager_->IsMouseBAssigned());
        if (ImGui::Button("Start timed test")) {
            StartRecording();
            if (state_==AppState::Recording) {
                currentSession_.captureTestMode=captureTestChoice_;
                currentSession_.testCursorConfined=keepCursorInWindow_;
                const auto capture=inputEngine_->GetCaptureDiagnostics();
                currentSession_.testStartDrops=capture.droppedEvents;
                currentSession_.testStartErrors=capture.readErrors;
                showLibrary_=false; plotMode_=0;
                ImGui::CloseCurrentPopup();
                if (captureTestChoice_==3) {
                    ReleaseRecordingCursor();
                    if (!inputEngine_->BeginForegroundTest(currentSession_.startTimestamp)) {
                        StopRecording();
                        statusMessage_="Foreground test could not start. Capture window activation or registration failed; this is not a valid test.";
                    } else UpdateRecordingCursor();
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (!currentSession_.captureTestMode) return;
    const bool recording=state_==AppState::Recording;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double elapsed=((recording ? now.QuadPart : currentSession_.endTimestamp)-currentSession_.startTimestamp)*1000.0/currentSession_.qpcFrequency;
    const char* mode=currentSession_.captureTestMode==3 ? "Foreground capture only" : currentSession_.captureTestMode==2 ? "Hidden capture only" : "Live plot";
    if (recording) {
        const char* instruction=elapsed<3000 ? "Get ready" : elapsed<8000 ? "Move A only" : elapsed<13000 ? "Move BOTH mice" : "Move A only again";
        const double next=elapsed<3000 ? 3000 : elapsed<8000 ? 8000 : elapsed<13000 ? 13000 : 18000;
        ImGui::TextColored(ImVec4(1,0.85f,0.3f,1),"TEST: %s | %s | %.1f s remaining",mode,instruction,(std::max)(0.0,(next-elapsed)/1000.0));
        return;
    }
    ImGui::Text("Capture test: %s | %s",mode,elapsed>=18000 ? "Complete" : "Stopped early");
    if (ImGui::BeginTable("Capture test rates",4,ImGuiTableFlags_RowBg)) {
        for (const char* label : {"Stage","A events/s","B events/s","Total events/s"}) ImGui::TableSetupColumn(label);
        ImGui::TableHeadersRow();
        const char* stages[]={"A only","Both mice","A only again"};
        for (int stage=0;stage<3;++stage) {
            const double begin=4000+stage*5000,end=7500+stage*5000;
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(stages[stage]);
            if (elapsed<end) {
                for (int col=0;col<3;++col) { ImGui::TableNextColumn(); ImGui::TextUnformatted("Incomplete"); }
                continue;
            }
            const double a=Analyzer::EventRateInRange(currentSession_.eventsA,currentSession_.startTimestamp,currentSession_.qpcFrequency,begin,end);
            const double b=Analyzer::EventRateInRange(currentSession_.eventsB,currentSession_.startTimestamp,currentSession_.qpcFrequency,begin,end);
            for (double rate : {a,b,a+b}) { ImGui::TableNextColumn(); ImGui::Text("%.0f",rate); }
        }
        ImGui::EndTable();
    }
    const auto& c=currentSession_.capture;
    ImGui::Text("During test: %llu buffer drops, %llu read errors | Raw capture: %s | Cursor confined: %s",
        static_cast<unsigned long long>(c.droppedEvents>=currentSession_.testStartDrops ? c.droppedEvents-currentSession_.testStartDrops : 0),
        static_cast<unsigned long long>(c.readErrors>=currentSession_.testStartErrors ? c.readErrors-currentSession_.testStartErrors : 0),
        c.legacySuppressed ? "on" : "off",currentSession_.testCursorConfined ? "on" : "off");
    ImGui::TextDisabled("Rates exclude stage transitions. Movement events measured by RLA, not USB polling. Test runs are not ranked.");
}

void App::RenderAutosaveControls() {
    if (ImGui::Button("Autosaves")) ImGui::OpenPopup("Autosave settings");
    bool confirmClear = false;
    if (ImGui::BeginPopup("Autosave settings")) {
        ImGui::TextUnformatted("Each completed recording is saved automatically.");
        if (autosave_) {
            const auto status = autosave_->GetStatus();
            const auto path = autosave_->Directory().u8string();
            ImGui::TextUnformatted(reinterpret_cast<const char*>(path.c_str()));
            ImGui::TextUnformatted(status.message.c_str());
            if (ImGui::Button("Open folder")) {
                std::error_code error;
                std::filesystem::create_directories(autosave_->Directory(), error);
                if (error || reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", autosave_->Directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                    statusMessage_ = "Could not open the autosave folder.";
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(status.busy || state_ == AppState::Recording);
            if (ImGui::Button("Clear autosaves")) { confirmClear = true; ImGui::CloseCurrentPopup(); }
            ImGui::EndDisabled();
        } else ImGui::Text("Autosave unavailable: %s", autosaveError_.c_str());
        ImGui::EndPopup();
    }
    if (confirmClear) ImGui::OpenPopup("Clear autosaves?");
    if (ImGui::BeginPopupModal("Clear autosaves?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Delete RLA-autosave-*.json files in the autosave folder?\nOther files and the current recording will remain.");
        ImGui::BeginDisabled(!autosave_ || autosave_->GetStatus().busy || state_ == AppState::Recording);
        if (ImGui::Button("Delete autosaves")) { autosave_->Clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::ResetLatencyAnalysis() {
    ++sessionRevision_;
    latencyFit_ = {};
    comparisonMessage_.clear();
    selectedLatencyMatch_ = -1;
    focusLatencyMatch_ = false;
}

void App::StartLatencyAnalysis() {
    if (state_ == AppState::Recording || latencyTask_.valid()) return;
    latencyFit_ = {};
    selectedLatencyMatch_ = -1;
    latencyRevision_ = sessionRevision_;
    resultMethod_=latencyMode_;
    try {
        latencyTask_ = std::async(std::launch::async, [session = currentSession_, mode = latencyMode_] {
            return Analyzer::FitLatency(session, mode);
        });
    } catch (const std::exception& error) { latencyFit_.message = std::string("Analysis failed: ") + error.what(); }
}

void App::PollLatencyAnalysis() {
    if (!latencyTask_.valid() || latencyTask_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
        auto fit = latencyTask_.get();
        if (latencyRevision_ == sessionRevision_) {
            latencyFit_ = std::move(fit);
            if (latencyFit_.valid && !latencyFit_.matches.empty()) {
                const auto closest=std::min_element(latencyFit_.matches.begin(),latencyFit_.matches.end(),[&](const auto& a,const auto& b) {
                    return std::abs(a.differenceMs-latencyFit_.differenceMs)<std::abs(b.differenceMs-latencyFit_.differenceMs);
                });
                selectedLatencyMatch_=static_cast<int>(closest-latencyFit_.matches.begin());
            }
            if (latencyFit_.valid && mouseLibrary_.autoRank && !currentSession_.mouseA.setupId.empty() && !currentSession_.mouseB.setupId.empty()) SaveComparison();
        }
    } catch (const std::exception& error) {
        if (latencyRevision_ == sessionRevision_) latencyFit_.message = std::string("Analysis failed: ") + error.what();
    }
}

void App::RenderLatencyControls() {
    ImGui::BeginDisabled(state_ == AppState::Recording || latencyTask_.valid() || currentSession_.eventsA.empty() || currentSession_.eventsB.empty());
    if (ImGui::Button("Analyze latency")) { ApplyAutoScaleB(); StartLatencyAnalysis(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (latencyTask_.valid()) ImGui::TextUnformatted("Analyzing...");
    else if (currentSession_.eventsA.empty() || currentSession_.eventsB.empty()) ImGui::TextDisabled("Record both mice to compare latency.");
    else ImGui::TextDisabled("Compare matching movement; align curve heights automatically.");
    ImGui::SameLine();
    if (ImGui::Button("Method...")) ImGui::OpenPopup("Latency method");
    if (ImGui::BeginPopup("Latency method")) {
        ImGui::BeginDisabled(state_ == AppState::Recording || latencyTask_.valid());
        ImGui::SetNextItemWidth(200);
        if (ImGui::Combo("Features", &latencyMode_, "Minima\0Half-height\0Both (recommended)\0")) ResetLatencyAnalysis();
        ImGui::EndDisabled();
        ImGui::TextWrapped("Uses matching minima and/or half-height crossings. Needs at least three shared cycles. Positive B-A means B is slower than A. Display smoothing does not change the result.");
        ImGui::EndPopup();
    }
    if (!latencyFit_.message.empty() && !latencyFit_.valid) ImGui::TextWrapped("%s", latencyFit_.message.c_str());
    if (!latencyFit_.valid) return;
    if (!comparisonMessage_.empty()) ImGui::TextWrapped("%s",comparisonMessage_.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.85f,0.4f,1));
    ImGui::TextWrapped("%s",GetLatencySummary().c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::Text("| Spread: %.3f ms | %zu matched cycles",latencyFit_.spreadMs,latencyFit_.matchedCycles);
    ImGui::SameLine();
    if (ImGui::Button("Review / save result")) { showLibrary_=true; libraryPage_=1; }
    ImGui::Checkbox("Highlight selected match", &highlightLatencyMatch_);
    ImGui::SameLine();
    ImGui::Checkbox("All matches", &showAllLatencyMarkers_);
    if (!latencyFit_.matches.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Previous")) { selectedLatencyMatch_=(selectedLatencyMatch_<=0 ? static_cast<int>(latencyFit_.matches.size()) : selectedLatencyMatch_)-1; focusLatencyMatch_=true; }
        ImGui::SameLine();
        if (ImGui::Button("Next")) { selectedLatencyMatch_=(selectedLatencyMatch_+1)%static_cast<int>(latencyFit_.matches.size()); focusLatencyMatch_=true; }
        ImGui::SameLine();
        if (ImGui::Button("Zoom to match")) { if (selectedLatencyMatch_<0) selectedLatencyMatch_=0; focusLatencyMatch_=true; }
        ImGui::SameLine();
        ImGui::Text("Match %d / %zu",selectedLatencyMatch_+1,latencyFit_.matches.size());
    }
    if (ImGui::TreeNode("Latency details")) {
        ImGui::TextWrapped("%s", latencyFit_.message.c_str());
        ImGui::TextUnformatted("Spread describes variation between points, not measurement accuracy. Select a row to view it.");
        if (ImGui::BeginTable("Matched features", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 130))) {
            for (const char* label : {"Feature", "A (ms)", "B (ms)", "B-A (ms)", "Shape match"}) ImGui::TableSetupColumn(label);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(latencyFit_.matches.size()));
            while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& match = latencyFit_.matches[i];
                ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableNextColumn();
                const char* name = match.feature == LatencyFeature::Minimum ? "Minimum" : match.feature == LatencyFeature::Rising ? "Rising 50%" : "Falling 50%";
                if (ImGui::Selectable(name, selectedLatencyMatch_ == i, ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedLatencyMatch_ = i; focusLatencyMatch_ = true;
                }
                ImGui::TableNextColumn(); ImGui::Text("%.3f", match.timeA);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", match.timeB);
                ImGui::TableNextColumn(); ImGui::Text("%+.3f", match.differenceMs);
                ImGui::TableNextColumn(); ImGui::Text("%.1f%%", match.quality * 100);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
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
            row(GetRecordingMouseName(false).c_str(), currentSession_.eventsA.size(), timingA_);
            row(GetRecordingMouseName(true).c_str(), currentSession_.eventsB.size(), timingB_);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Statistics include long gaps. Arrival times are measured in the application, not at the USB device.");
        const auto& capture = currentSession_.capture;
        if (capture.available) {
            ImGui::TextWrapped("Buffered capture totals since app start: %llu mouse reports, %llu read in groups, largest batch %llu. Queue drops: %llu. Capture errors: %llu (last code %u).",
                capture.packets, capture.groupedPackets, capture.maxBatch, capture.droppedEvents, capture.readErrors, capture.lastError);
            ImGui::TextWrapped("Reports in one batch share a read timestamp. Their individual arrival intervals are unknown; the window rate still counts each movement event.");
            ImGui::Text("Raw capture (ordinary mouse messages disabled): %s", capture.legacySuppressed ? "On" : "Off");
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
                scatter((GetRecordingMouseName(false)+" - raw###mouse-A-raw").c_str(), timingA_, showTimingGaps_ ? timingA_.intervalsMs : timingA_.shortIntervalsMs, colorA);
                scatter((GetRecordingMouseName(true)+" - raw###mouse-B-raw").c_str(), timingB_, showTimingGaps_ ? timingB_.intervalsMs : timingB_.shortIntervalsMs, colorB);
                auto plot = [&](const char* meanLabel, const EventTiming& data, ImVec4 color) {
                    if (data.timesMs.empty()) return;
                    ImPlot::SetNextLineStyle(color, 3.0f);
                    ImPlot::PlotLine(meanLabel, data.timesMs.data(), data.meanIntervalsMs.data(), static_cast<int>(data.timesMs.size()));
                };
                plot((GetRecordingMouseName(false)+" - mean###mouse-A-mean").c_str(), timingA_, meanA);
                plot((GetRecordingMouseName(true)+" - mean###mouse-B-mean").c_str(), timingB_, meanB);
                ImPlot::EndPlot();
            }
        }
        if (timingGraph_ != 1) {
            if (timingFitPending_) ImPlot::SetNextAxisToFit(ImAxis_Y1);
            if (ImPlot::BeginPlot("Movement event rate")) {
                setupAxes("Rate (Hz)");
                if (showInstantHz_) {
                    scatter((GetRecordingMouseName(false)+" - interval Hz###mouse-A-interval Hz").c_str(), timingA_, timingA_.instantHz, colorA);
                    scatter((GetRecordingMouseName(true)+" - interval Hz###mouse-B-interval Hz").c_str(), timingB_, timingB_.instantHz, colorB);
                }
                auto plot = [&](const char* label, const EventTiming& data, ImVec4 color) {
                    if (!data.rateTimesMs.empty()) {
                        ImPlot::SetNextLineStyle(color, 3.0f);
                        ImPlot::PlotLine(label, data.rateTimesMs.data(), data.ratesHz.data(), static_cast<int>(data.rateTimesMs.size()));
                    }
                };
                plot((GetRecordingMouseName(false)+" - window rate###mouse-A-window rate").c_str(), timingA_, meanA);
                plot((GetRecordingMouseName(true)+" - window rate###mouse-B-window rate").c_str(), timingB_, meanB);
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
    if (plotMode_ == 0) RenderLatencyControls();
    else RenderScaleControls();

    // Measure after controls so the graph uses the remaining height.
    auto plotSize = []() {
        return ImVec2(-1, (std::max)(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing()));
    };

    if (plotMode_ == 0) {
        // ==================== VELOCITY MODE ====================
        if (ImGui::Button("Smooth comparison")) {
            enableTimeBinning_=true; timeWeightedBins_=true; timeBinMs_=2.0f;
            enableSmoothing_=true; smoothingMode_=0; smoothingSamples_=3;
            enableGapInterpolation_=true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Raw reports")) { enableTimeBinning_=false; enableSmoothing_=false; enableGapInterpolation_=false; }
        ImGui::SameLine();
        if (ImGui::Button("Fit whole recording")) { fitMovementView_=true; focusLatencyMatch_=false; }
        ImGui::SameLine();
        if (!enableTimeBinning_ && !enableSmoothing_)
            ImGui::TextColored(ImVec4(1,0.8f,0.3f,1),"RAW counts per report - no smoothing");
        else {
            ImGui::Text("Display: %s | %s",enableTimeBinning_ ? "time bins" : "per report",enableSmoothing_ ? "smoothed" : "no smoothing");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Display processing only. Recorded events and latency analysis are unchanged.");
        }
        if (ImGui::CollapsingHeader("Advanced display settings")) {
        RenderScaleControls();
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

        } // Advanced display settings

        // Velocity plot
        if (fitMovementView_) ImPlot::SetNextAxesToFit();
        if (ImPlot::BeginPlot("Movement magnitude vs Time", plotSize())) {
            fitMovementView_=false;
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

            if (focusLatencyMatch_ && selectedLatencyMatch_ >= 0 && selectedLatencyMatch_ < static_cast<int>(latencyFit_.matches.size())) {
                const auto& match = latencyFit_.matches[selectedLatencyMatch_];
                ImPlot::SetupAxisLimits(ImAxis_X1, (std::min)(match.timeA, match.timeB) - 15, (std::max)(match.timeA, match.timeB) + 15, ImPlotCond_Always);
                double peak=0;
                for (bool mouseB : {false,true}) {
                    const auto& curve=PrepareMovementPlot(mouseB);
                    auto first=std::lower_bound(curve.times.begin(),curve.times.end(),(std::min)(match.timeA,match.timeB)-15);
                    for (size_t i=static_cast<size_t>(first-curve.times.begin()); i<curve.times.size() && curve.times[i]<(std::max)(match.timeA,match.timeB)+15; ++i)
                        peak=(std::max)(peak,curve.values[i]);
                }
                if (peak>0) ImPlot::SetupAxisLimits(ImAxis_Y1,-0.08*peak,peak*1.45,ImPlotCond_Always);
                focusLatencyMatch_ = false;
            }

            // Preparation is cached independently of zoom and pan.
            for (bool mouseB : {false,true}) {
                const auto& curve=PrepareMovementPlot(mouseB);
                if (curve.times.empty()) continue;
                const ImVec4 color=mouseB ? ImVec4(1.0f,0.4f,0.2f,1.0f) : ImVec4(0.2f,0.6f,1.0f,1.0f);
                ImPlot::SetNextLineStyle(color,2.0f);
                if (showDataPointMarkers_) ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle,markerSize_,color,1.0f);
                ImPlot::PlotLine((GetRecordingMouseName(mouseB)+(mouseB ? "###movement-B" : "###movement-A")).c_str(),curve.times.data(),curve.values.data(),static_cast<int>(curve.times.size()));
            }

            if ((highlightLatencyMatch_ || showAllLatencyMarkers_) && latencyFit_.valid) {
                if (showAllLatencyMarkers_) {
                    std::vector<double> marksA,marksB;
                    for (const auto& match : latencyFit_.matches) { marksA.push_back(match.timeA); marksB.push_back(match.timeB); }
                    ImPlot::SetNextLineStyle(ImVec4(0.3f,0.7f,1,0.65f),1.5f);
                    ImPlot::PlotInfLines("A matches",marksA.data(),static_cast<int>(marksA.size()));
                    ImPlot::SetNextLineStyle(ImVec4(1,0.55f,0.25f,0.65f),1.5f);
                    ImPlot::PlotInfLines("B matches",marksB.data(),static_cast<int>(marksB.size()));
                }
                if (highlightLatencyMatch_ && selectedLatencyMatch_>=0 && selectedLatencyMatch_<static_cast<int>(latencyFit_.matches.size())) {
                    const auto& match=latencyFit_.matches[selectedLatencyMatch_];
                    const ImVec4 aColor(0.35f,0.8f,1,1),bColor(1,0.65f,0.25f,1);
                    ImPlot::SetNextLineStyle(aColor,3.0f);
                    ImPlot::PlotInfLines("A selected",&match.timeA,1);
                    ImPlot::SetNextLineStyle(bColor,3.0f);
                    ImPlot::PlotInfLines("B selected",&match.timeB,1);
                    const auto limits=ImPlot::GetPlotLimits();
                    const double y=limits.Y.Max-0.18*limits.Y.Size();
                    if (match.timeA>=limits.X.Min && match.timeA<=limits.X.Max && match.timeB>=limits.X.Min && match.timeB<=limits.X.Max) {
                        ImPlot::Annotation(match.timeA,y,aColor,ImVec2(-65,-30),true,"A %.3f ms",match.timeA);
                        ImPlot::Annotation(match.timeB,y,bColor,ImVec2(65,-30),true,"B %.3f ms",match.timeB);
                        ImPlot::Annotation((match.timeA+match.timeB)*0.5,y,ImVec4(1,0.9f,0.45f,1),ImVec2(0,20),true,"B-A %+.3f ms",match.differenceMs);
                        auto* draw=ImPlot::GetPlotDrawList();
                        draw->AddLine(ImPlot::PlotToPixels(match.timeA,y),ImPlot::PlotToPixels(match.timeB,y),IM_COL32(255,230,115,255),3.0f);
                    }
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
                    ImPlot::PlotLine((GetRecordingMouseName(false)+" - X###mouse-A-X").c_str(), timesA.data(), deltasXA.data(), static_cast<int>(timesA.size()));

                    ImPlot::SetNextLineStyle(ImVec4(0.2f, 0.9f, 0.9f, 1.0f), 1.5f);
                    if (showDataPointMarkers_) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, markerSize_, ImVec4(0.2f, 0.9f, 0.9f, 1.0f), 1.0f);
                    }
                    ImPlot::PlotLine((GetRecordingMouseName(false)+" - Y###mouse-A-Y").c_str(), timesA.data(), deltasYA.data(), static_cast<int>(timesA.size()));
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
                    ImPlot::PlotLine((GetRecordingMouseName(true)+" - X###mouse-B-X").c_str(), timesB.data(), deltasXB.data(), static_cast<int>(timesB.size()));

                    ImPlot::SetNextLineStyle(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), 1.5f);
                    if (showDataPointMarkers_) {
                        ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, markerSize_, ImVec4(1.0f, 0.7f, 0.2f, 1.0f), 1.0f);
                    }
                    ImPlot::PlotLine((GetRecordingMouseName(true)+" - Y###mouse-B-Y").c_str(), timesB.data(), deltasYB.data(), static_cast<int>(timesB.size()));
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
    if (librarySaveFailed_) {
        ImGui::TextColored(ImVec4(1,0.4f,0.3f,1),"Library save failed. Open Setup or Rankings to retry.");
        return;
    }
    ImGui::Text("%s", statusMessage_.c_str());
    if (state_ != AppState::Recording) {
        const auto message = autosave_ ? autosave_->GetStatus().message : autosaveError_;
        if (!message.empty()) {
            ImGui::SameLine();
            ImGui::TextUnformatted(message.c_str());
        }
    }
}

void App::TransitionTo(AppState newState) {
    state_ = newState;
}

void App::StartRecording() {
    if (hwnd_ && !inputEngine_->SetRawCapture(rawCaptureMode_)) {
        statusMessage_ = "Could not change mouse capture mode. Recording was not started.";
        return;
    }
    deviceManager_->CancelAssignment();
    showLibrary_ = false;
    showPlotWindow_ = true;
    currentSession_ = RecordingSession{};
    autoRankPending_=false;
    CaptureMouseIdentity();
    ResetLatencyAnalysis();
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
    statusMessage_ = inputEngine_->IsRawCapture() ?
        "Recording... Move both mice, then click or press Esc to stop" :
        "Recording... Move both mice simultaneously, then click Stop";
}

void App::StopRecording() {
    if (state_ != AppState::Recording || stoppingRecording_) return;
    stoppingRecording_ = true;
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
    if ((currentSession_.captureTestMode==3 || inputEngine_->ForegroundTestWindow()) && !inputEngine_->EndForegroundTest(hwnd_))
        statusMessage_ += " Could not restore the hidden capture window.";
    if (!inputEngine_->SetRawCapture(false))
        statusMessage_ += " Could not restore mouse controls. Press Esc to retry or Alt+F4 to close.";
    ReleaseRecordingCursor();
    stoppingRecording_ = false;
    if (!currentSession_.captureTestMode) DetectSessionRates(true);
    if (currentSession_.captureTestMode>=2) RebuildPlotData();
    if (autosave_) {
        const auto* a = deviceManager_->GetMouseADevice();
        const auto* b = deviceManager_->GetMouseBDevice();
        try { autosave_->Save(currentSession_, a ? a->name : L"Unknown", b ? b->name : L"Unknown"); }
        catch (const std::exception& error) { statusMessage_ += std::string(" Autosave failed: ") + error.what(); }
    }
    autoRankPending_=!currentSession_.captureTestMode && mouseLibrary_.autoRank && !libraryReadOnly_ &&
        !currentSession_.mouseA.setupId.empty() && !currentSession_.mouseB.setupId.empty() &&
        currentSession_.mouseA.pollingHz>0 && currentSession_.mouseB.pollingHz>0 &&
        currentSession_.mouseA.setupId!=currentSession_.mouseB.setupId &&
        !currentSession_.eventsA.empty() && !currentSession_.eventsB.empty();
}

void App::OnCaptureEscape() {
    if (state_ == AppState::Recording) StopRecording();
    else if (inputEngine_) inputEngine_->SetRawCapture(false);
}

void App::OnCaptureFocusLost() {
    if (inputEngine_ && inputEngine_->ForegroundTestWindow()) { ReleaseRecordingCursor(); return; }
    if (inputEngine_ && inputEngine_->IsRawCapture()) OnCaptureEscape();
    ReleaseRecordingCursor();
}

void App::UpdateRecordingCursor() {
    const HWND target=inputEngine_ && inputEngine_->ForegroundTestWindow() ? inputEngine_->ForegroundTestWindow() : hwnd_;
    if (!target || state_ != AppState::Recording || !keepCursorInWindow_ || GetForegroundWindow() != target) {
        ReleaseRecordingCursor();
        return;
    }
    RECT client{};
    POINT origin{};
    if (!GetClientRect(target, &client) || !ClientToScreen(target, &origin) ||
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

std::string App::GetLatencySummary() const {
    const double difference=latencyFit_.differenceMs;
    if (std::abs(difference)<0.0005)
        return GetRecordingMouseName(false)+" and "+GetRecordingMouseName(true)+": difference rounds to 0.000 ms.";
    const bool bFaster=difference<0;
    return std::format("{} is {:.3f} ms faster than {}",GetRecordingMouseName(bFaster),
        std::abs(difference),GetRecordingMouseName(!bFaster));
}

std::string App::GetRecordingMouseName(bool mouseB) const {
    const auto& mouse=mouseB ? currentSession_.mouseB : currentSession_.mouseA;
    if (mouse.name.empty()) return mouseB ? "Mouse B (Test)" : "Mouse A (Reference)";
    auto name=mouse.name;
    // ImGui reserves ## for hidden IDs. Keep user names visible in legends.
    for (auto& c : name) if (c=='#') c=' ';
    return name+(mouseB ? " (B)" : " (A)");
}

std::string App::GetMouseDisplayName(const MouseDevice* device) const {
    if (!device) return "Unknown";
    const auto saved=mouseLibrary_.BoundSetup(MouseLibrary::DeviceKey(device->path));
    if (const auto* setup=mouseLibrary_.FindSetup(saved)) {
        const auto* mouse=mouseLibrary_.FindMouse(setup->mouseId);
        return (mouse ? mouse->name : std::string("Unknown"))+" / "+setup->connection+
            (setup->label.empty() ? "" : " / "+setup->label);
    }

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
            if (!libraryReadOnly_) {
                try {
                    auto imported=mouseLibrary_;
                    imported.ImportIdentity(currentSession_.mouseA); imported.ImportIdentity(currentSession_.mouseB);
                    mouseLibrary_=std::move(imported); LibraryChanged();
                } catch (const std::exception& error) { libraryMessage_=error.what(); }
            }
            RebuildPlotData();
            analysisResult_ = AnalysisResult{}; // Clear analysis
            TransitionTo(AppState::Ready);
            statusMessage_ = std::format("Session loaded. A={} events, B={} events",
                                        currentSession_.eventsA.size(),
                                        currentSession_.eventsB.size());
            showLibrary_=false;
            fitMovementView_=true;
        }
        else {
            statusMessage_ = "Load failed: " + dataStore_->GetLastError();
        }
    }
}

void App::RebuildPlotData() {
    autoRankPending_=false;
    ResetLatencyAnalysis();
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

const App::MovementCache& App::PrepareMovementPlot(bool mouseB) {
    auto& cache=mouseB ? movementB_ : movementA_;
    const auto& events=mouseB ? currentSession_.eventsB : currentSession_.eventsA;
    const MovementKey key{sessionRevision_,events.size(),enableTimeBinning_,timeWeightedBins_,enableGapInterpolation_,enableSmoothing_,
        timeBinMs_,gapThresholdMs_,gapSampleIntervalMs_,smoothingTimeMs_,mouseB && enableYScaleB_ ? yScaleB_ : 1.0f,smoothingMode_,smoothingSamples_};
    if (cache.valid && cache.key==key) return cache;
    // Rebuild at most 30 times per second during capture. Settings and session
    // changes bypass this limit; stopped recordings always receive the final data.
    const auto now=std::chrono::steady_clock::now();
    auto previousSettings=cache.key; previousSettings.events=key.events;
    if (state_==AppState::Recording && cache.valid && previousSettings==key &&
        now-cache.updatedAt<std::chrono::milliseconds(33)) return cache;
    cache.updatedAt=now;
    cache.valid=false;
    if (enableTimeBinning_) ApplyTimeBinning(events,currentSession_.startTimestamp,cache.times,cache.values,timeBinMs_);
    else {
        cache.times=mouseB ? liveTimesB_ : liveTimesA_;
        cache.values=mouseB ? liveVelocitiesB_ : liveVelocitiesA_;
    }
    if (enableGapInterpolation_ && !cache.times.empty()) {
        std::vector<double> times,values;
        InterpolateGaps(cache.times,cache.values,times,values,
            enableTimeBinning_ ? (std::max)(gapThresholdMs_,timeBinMs_*1.5f) : gapThresholdMs_,
            enableTimeBinning_ ? timeBinMs_ : gapSampleIntervalMs_);
        cache.times=std::move(times); cache.values=std::move(values);
    }
    if (enableSmoothing_ && !cache.times.empty()) {
        std::vector<double> times,values;
        if (smoothingMode_==0) ApplyMovingAverageSmoothing(cache.times,cache.values,times,values,smoothingSamples_);
        else ApplyTimeWindowSmoothing(cache.times,cache.values,times,values,smoothingTimeMs_);
        cache.times=std::move(times); cache.values=std::move(values);
    }
    if (key.scale!=1.0f) for (auto& value : cache.values) value*=key.scale;
    cache.key=key; cache.valid=true; ++cache.builds;
    return cache;
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

    if (times.empty() || times.size() != values.size()) return;
    if (!std::isfinite(sampleIntervalMs) || sampleIntervalMs <= 0) {
        outTimes = times; outValues = values; return;
    }

    for (size_t i = 0; i < times.size(); ++i) {
        // Check for gap before this point (except for first point)
        if (i > 0) {
            double gap = times[i] - times[i - 1];
            if (gap > gapThresholdMs) {
                // Insert zero-velocity points to fill the gap
                // Start a bit after the previous point and end a bit before this point
                // Keep short gaps sampled for smoothing. A long idle period
                // needs only boundary samples and a flat line between them.
                // Bound this work independently of the duration of a pause.
                const size_t count = static_cast<size_t>((std::min)(100.0, std::ceil(gap / sampleIntervalMs)));
                for (size_t j = 1; j <= count; ++j) {
                    const double t = j <= count / 2 ? times[i - 1] + j * sampleIntervalMs : times[i] - (count - j + 1) * sampleIntervalMs;
                    if (t <= times[i - 1] || t >= times[i] || t <= outTimes.back()) continue;
                    outTimes.push_back(t);
                    outValues.push_back(0.0);
                }
            }
        }

        // Add the actual data point
        outTimes.push_back(times[i]);
        outValues.push_back(values[i]);
    }
}

} // namespace RLA
