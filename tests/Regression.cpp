#include "src/App.h"
#include "src/WindowMessages.h"
#include "src/ResponsivenessMonitor.h"
#include "src/FramePacer.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <imgui.h>
#include <implot.h>
#include <implot_internal.h>
#include <string_view>
#include <cstring>
#include <climits>

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void RunFramePacingChecks() {
    using Clock=std::chrono::steady_clock;
    const auto oldStart=Clock::now();
    for (int i=0;i<24;++i) {
        const auto deadline=GetTickCount64()+16;
        for (;;) {
            const auto now=GetTickCount64();
            if (now>=deadline) break;
            MsgWaitForMultipleObjectsEx(0,nullptr,static_cast<DWORD>(deadline-now),QS_ALLINPUT,MWMO_INPUTAVAILABLE);
            MSG msg{}; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
        }
    }
    const double oldMs=std::chrono::duration<double,std::milli>(Clock::now()-oldStart).count()/24;
    RLA::FramePacer pacer; const auto start=Clock::now();
    for (int i=0;i<24;++i) {
        pacer.BeginFrame();
        while (!pacer.Ready()) {
            pacer.Wait();
            MSG msg{}; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
        }
    }
    const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count()/24;
    Check(ms>=8,"Frame pacing must not run before its deadline");
    std::cout<<"UI pacing mean interval: old="<<oldMs<<" ms, new="<<ms<<" ms.\n";
}

namespace RLA {
struct RendererRegressionAccess {
    static void Run() {
        WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"RLA_RenderCheck";
        RegisterClassW(&wc);
        HWND window=CreateWindowW(wc.lpszClassName,L"Render test",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,wc.hInstance,nullptr);
        Check(window!=nullptr,"Renderer test window must initialize");
        {
            Renderer renderer; Check(renderer.Initialize(window,640,480),"Native renderer must initialize");
            ImGui::GetIO().IniFilename=nullptr;
            for (int frame=0;frame<12;++frame) {
                const int previous=renderer.width_, width=frame%2 ? 800 : 640;
                renderer.OnResize(width+20,480); renderer.OnResize(width,480);
                Check(renderer.width_==previous && renderer.pendingWidth_==width,"Resize notifications must defer and coalesce graphics work");
                renderer.BeginFrame();
                Check(renderer.width_==width && renderer.renderTargetView_,"Frame boundary must apply the pending resize");
                ImGui::Begin("Render check"); ImGui::TextUnformatted("Frame"); ImGui::End(); renderer.EndFrame();
                Check(SUCCEEDED(renderer.lastPresent_) || renderer.lastPresent_==DXGI_ERROR_WAS_STILL_DRAWING,"Nonblocking presentation must be supported by the swap chain");
            }
        }
        const auto directory=std::filesystem::temp_directory_path()/(L"RLA-monitor-test-"+std::to_wstring(GetCurrentProcessId()));
        {
            ResponsivenessMonitor monitor(window,directory,250);
            monitor.Checkpoint("nested message loop test");
            // Emulate a driver/modal loop that answers messages but produces no frames.
            const auto end=GetTickCount64()+2200;
            while (GetTickCount64()<end) {
                MSG msg{}; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
                Sleep(5);
            }
        }
        const auto log=directory/(L"hang-"+std::to_wstring(GetCurrentProcessId())+L".txt");
        std::ifstream input(log); std::string contents((std::istreambuf_iterator<char>(input)),{}); input.close();
        Check(contents.find("nested message loop test")!=std::string::npos && contents.find("window answers messages")!=std::string::npos,
              "Stalled frames must be logged even while the window responds to messages");
        std::filesystem::remove(log); std::filesystem::remove(directory);
        DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
        std::cout<<"Native deferred resize, nonblocking presentation and stalled-frame diagnostics passed.\n";
    }
};
}

static void RunWindowMessageChecks() {
    // Simulate continuously available cursor motion. An unfiltered drain
    // would never reach rendering; only one of each motion type may dispatch.
    std::vector<UINT> queued = {WM_KEYDOWN, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEWHEEL, WM_SIZE};
    unsigned clientMoves = 0, frameMoves = 0, otherMessages = 0, calls = 0;
    MSG message{};
    auto peek = [&](MSG& out, UINT first, UINT last) {
        Check(++calls < 100, "Cursor motion must not keep the UI drain running");
        for (UINT motion : {UINT(WM_MOUSEMOVE), UINT(WM_NCMOUSEMOVE)}) {
            if (motion >= first && motion <= last) { out.message = motion; return true; }
        }
        for (auto it = queued.begin(); it != queued.end(); ++it) {
            if (*it >= first && *it <= last) {
                out.message = *it; queued.erase(it); return true;
            }
        }
        return false;
    };
    auto dispatch = [&](MSG& msg) {
        if (msg.message == WM_MOUSEMOVE) ++clientMoves;
        else if (msg.message == WM_NCMOUSEMOVE) ++frameMoves;
        else ++otherMessages;
    };
    Check(RLA::PumpWindowMessages(message, peek, dispatch), "Motion must not stop the app");
    Check(clientMoves == 1 && frameMoves == 1 && otherMessages == 5 && queued.empty(),
          "Motion must be bounded while clicks, keys, wheel and resize still dispatch");
    calls = 0;
    Check(RLA::PumpWindowMessages(message, peek, dispatch) && clientMoves == 2 && frameMoves == 2,
          "Cursor motion must resume on the next frame");
    Check(!RLA::PumpWindowMessages(message,
        [](MSG& msg, UINT, UINT) { msg.message = WM_QUIT; return true; },
        [](MSG&) { Check(false, "WM_QUIT must not dispatch"); }), "Quit must stop the message pump");
    std::cout << "UI message checks passed with continuous cursor motion.\n";
    unsigned dispatched = 0;
    Check(RLA::PumpWindowMessages(message,
        [](MSG& msg, UINT first, UINT last) {
            if (WM_KEYDOWN < first || WM_KEYDOWN > last) return false;
            msg.message = WM_KEYDOWN; return true;
        }, [&](MSG&) { Check(++dispatched <= 256, "A non-motion flood must not block rendering"); }), "Message flood must continue next frame");
    Check(dispatched == 256, "Non-motion messages need a finite frame budget");
}

static RLA::RecordingSession PauseSession() {
    RLA::RecordingSession session;
    session.startTimestamp = 987654321;
    session.qpcFrequency = 1000000;
    session.endTimestamp = session.startTimestamp + 1450000;
    for (int i = 0; i <= 200; ++i)
        session.eventsA.push_back({nullptr, session.startTimestamp + i * 500, 1, 0});
    for (int i = 0; i <= 200; ++i)
        session.eventsA.push_back({nullptr, session.startTimestamp + 1100000 + i * 500, 1, 0});
    session.eventsB = session.eventsA;
    return session;
}

namespace RLA {
struct InputEngineRegressionAccess {
    static void Push(InputEngine& engine, const MouseEvent& event) {
        Check(engine.eventBuffer_.push(event), "Test event could not be queued");
    }

    inline static std::vector<std::vector<BYTE>> batches;
    inline static std::vector<UINT> counts;
    inline static size_t nextBatch = 0;
    inline static DWORD forcedError = 0;
    inline static DWORD registrationFlags = 0;
    inline static bool failRegistration = false;
    inline static RAWINPUT primary{};
    static UINT WINAPI ReadOne(HRAWINPUT, UINT command, LPVOID output, PUINT size, UINT headerSize) {
        Check(command == RID_INPUT && headerSize == sizeof(RAWINPUTHEADER) && *size >= sizeof(RAWINPUT), "Primary report read must use the native layout");
        std::memcpy(output, &primary, sizeof(primary)); *size = sizeof(primary); return sizeof(primary);
    }
    static bool Responsive(InputEngine& engine) {
        DWORD_PTR result = 0;
        return SendMessageTimeoutW(engine.inputHwnd_, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &result) != 0;
    }
    static BOOL WINAPI Register(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
        Check(count == 1 && size == sizeof(RAWINPUTDEVICE) && devices[0].usUsagePage == 1 && devices[0].usUsage == 2,
              "Capture mode must only change mouse registration");
        if (failRegistration) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        registrationFlags = devices[0].dwFlags;
        return TRUE;
    }

    static void EnableTestCapture(InputEngine& engine) {
        engine.initialized_ = true;
        engine.rawDeviceRegistrar_ = &Register;
        failRegistration = false;
        Check(engine.SetRawCapture(true), "Test raw capture must enable");
    }
    static UINT WINAPI Read(PRAWINPUT output, PUINT size, UINT headerSize) {
        Check(headerSize == sizeof(RAWINPUTHEADER), "Buffered read header size is incorrect");
        if (forcedError) { SetLastError(forcedError); return UINT_MAX; }
        if (nextBatch == batches.size()) return 0;
        const auto& bytes = batches[nextBatch];
        if (*size < bytes.size()) {
            *size = static_cast<UINT>(bytes.size());
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return UINT_MAX;
        }
        std::memcpy(output, bytes.data(), bytes.size());
        *size = static_cast<UINT>(bytes.size());
        return counts[nextBatch++];
    }

    static void Run() {
        auto engine = std::make_unique<InputEngine>();
        engine->running_ = true;
        engine->rawBufferReader_ = &Read;
        batches.clear(); counts.clear(); nextBatch = 0; forcedError = 0;
        // One second of two 8 kHz mice, in 250 batches of 64 reports.
        for (int batch = 0; batch < 250; ++batch) {
            std::vector<BYTE> bytes(64 * sizeof(RAWINPUT));
            for (int i = 0; i < 64; ++i) {
                RAWINPUT raw{};
                raw.header.dwType = RIM_TYPEMOUSE;
                raw.header.dwSize = sizeof(raw);
                raw.header.hDevice = reinterpret_cast<HANDLE>(uintptr_t(1 + i % 2));
                raw.data.mouse.lLastX = batch * 64 + i + 1;
                raw.data.mouse.lLastY = -raw.data.mouse.lLastX;
                std::memcpy(bytes.data() + i * sizeof(raw), &raw, sizeof(raw));
            }
            batches.push_back(std::move(bytes)); counts.push_back(64);
        }
        while (nextBatch != batches.size()) Check(engine->DrainRawInput(), "Buffered drain failed");
        Check(engine->DrainRawInput(), "Empty queue must succeed");
        size_t received = 0;
        int64_t previous = 0;
        engine->ProcessEvents([&](const MouseEvent& e) {
            Check(e.deltaX == received + 1 && e.deltaY == -e.deltaX, "Buffered reports must not be summed, lost or duplicated");
            Check(e.deviceHandle == reinterpret_cast<HANDLE>(uintptr_t(1 + received % 2)), "Mouse identity must be preserved");
            Check(e.timestamp >= previous && (received % 64 == 0 || e.timestamp == previous),
                  "A batch must share a read timestamp, with ordered batches");
            previous = e.timestamp; ++received;
        }, engine->GetBufferSize());
        const auto stats = engine->GetCaptureDiagnostics();
        Check(received == 16000 && stats.packets == 16000 && stats.groupedPackets == 16000 &&
              stats.maxBatch == 64 && stats.droppedEvents == 0 && stats.readErrors == 0,
              "Buffered diagnostics must account for every report");
        Push(*engine, {nullptr, 0, 1, 0});
        const auto drained = engine->ProcessEvents([&](const MouseEvent& e) { Push(*engine, e); });
        Check(drained == 1 && engine->GetBufferSize() == 1, "Newly queued events must not extend the current UI drain");
        engine->ProcessEvents([](const MouseEvent&) {});
        primary = {}; primary.header.dwType = RIM_TYPEMOUSE; primary.header.dwSize = sizeof(primary);
        primary.data.mouse.lLastX = 42; primary.header.hDevice = reinterpret_cast<HANDLE>(uintptr_t(2));
        engine->rawDataReader_ = &ReadOne;
        Check(engine->ReadRawInput(nullptr), "The report removed by GetMessage must be read separately");
        Check(engine->ProcessEvents([](const MouseEvent& e) { Check(e.deltaX == 42 && e.deviceHandle == reinterpret_cast<HANDLE>(uintptr_t(2)), "Primary report must preserve data"); }) == 1,
              "The primary report must appear exactly once");

        // A larger variable-size non-mouse record requires a retry and must be skipped.
        batches = {std::vector<BYTE>(70000)}; counts = {1}; nextBatch = 0;
        RAWINPUTHEADER header{}; header.dwType = RIM_TYPEHID; header.dwSize = 70000;
        std::memcpy(batches[0].data(), &header, sizeof(header));
        Check(engine->DrainRawInput() && nextBatch == 1 && engine->eventBuffer_.empty(),
              "The buffer must grow and retry without treating HID data as mouse movement");
        Check(engine->GetCaptureDiagnostics().readErrors == 0, "A successful resize is not a read failure");

        forcedError = ERROR_ACCESS_DENIED;
        Check(!engine->DrainRawInput(), "Read failures must be reported");
        Check(engine->GetCaptureDiagnostics().readErrors == 1 &&
              engine->GetCaptureDiagnostics().lastError == ERROR_ACCESS_DENIED, "Read errors must be counted");
        forcedError = 0;
        batches = {std::vector<BYTE>(sizeof(RAWINPUTHEADER))}; counts = {1}; nextBatch = 0;
        header.dwType = RIM_TYPEMOUSE; header.dwSize = sizeof(RAWINPUTHEADER);
        std::memcpy(batches[0].data(), &header, sizeof(header));
        Check(!engine->DrainRawInput(), "Truncated mouse records must be rejected");
        Check(engine->GetCaptureDiagnostics().lastError == ERROR_INVALID_DATA, "Malformed batches must be diagnosed");

        RAWINPUT raw{}; raw.header.dwType = RIM_TYPEMOUSE; raw.header.dwSize = sizeof(raw);
        Check(engine->ProcessRawBatch(reinterpret_cast<BYTE*>(&raw), sizeof(raw), 1, 100), "Zero movement is valid");
        Check(engine->eventBuffer_.empty(), "Zero movement must not change the saved movement stream");
        raw.data.mouse.lLastX = 7;
        engine->running_ = false;
        Check(engine->ProcessRawBatch(reinterpret_cast<BYTE*>(&raw), sizeof(raw), 1, 101), "Stopped input must drain");
        Check(engine->eventBuffer_.empty(), "Stopped input must not queue events");
        engine->running_ = true;
        EnableTestCapture(*engine);
        Check(registrationFlags == (RIDEV_INPUTSINK | RIDEV_NOLEGACY) && engine->GetCaptureDiagnostics().legacySuppressed,
              "Raw capture must suppress only legacy mouse input and record the mode");
        raw.data.mouse.lLastX = 0;
        raw.data.mouse.usButtonFlags = RI_MOUSE_LEFT_BUTTON_DOWN;
        Check(engine->ProcessRawBatch(reinterpret_cast<BYTE*>(&raw), sizeof(raw), 1, 102), "Stationary stop click must parse");
        Check(engine->eventBuffer_.empty() && engine->TakeStopClick() && !engine->TakeStopClick(),
              "A stop click must be delivered once without a movement event");
        failRegistration = true;
        Check(!engine->SetRawCapture(false) && engine->IsRawCapture(), "Failed restoration must not report success");
        failRegistration = false;
        Check(engine->SetRawCapture(false) && registrationFlags == RIDEV_INPUTSINK && !engine->IsRawCapture(),
              "Stopping must restore ordinary mouse controls");
        Check(engine->ProcessRawBatch(reinterpret_cast<BYTE*>(&raw), sizeof(raw), 1, 103) && !engine->TakeStopClick(),
              "Ordinary UI clicks must not request a capture stop");
        raw.data.mouse.lLastX = 7;
        raw.data.mouse.usButtonFlags = 0;
        while (engine->eventBuffer_.push({nullptr, 0, 1, 0})) {}
        Check(engine->ProcessRawBatch(reinterpret_cast<BYTE*>(&raw), sizeof(raw), 1, 102), "A full ring must not block capture");
        Check(engine->GetCaptureDiagnostics().droppedEvents == 1, "Ring overflow must be counted");
        engine.reset();
        // Exercise actual registration, idle waiting and shutdown without a visible window.
        engine = std::make_unique<InputEngine>();
        Check(engine->Initialize() && engine->Start(), "Buffered input thread must initialize");
        auto registeredFlags = []() {
            RAWINPUTDEVICE devices[4]{};
            UINT count = 4;
            const UINT found = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
            Check(found != UINT_MAX, "Windows registration query failed");
            for (UINT i = 0; i < found; ++i)
                if (devices[i].usUsagePage == 1 && devices[i].usUsage == 2) return devices[i].dwFlags;
            throw std::runtime_error("Mouse registration not found");
        };
        Check(engine->SetRawCapture(true) && registeredFlags() == (RIDEV_INPUTSINK | RIDEV_NOLEGACY),
              "Windows must accept raw-only mouse registration");
        Check(engine->SetRawCapture(false) && registeredFlags() == RIDEV_INPUTSINK,
              "Windows must restore ordinary mouse registration");
        Sleep(20);
        engine.reset();
        std::cout << "Buffered capture: 16000 reports preserved; resize, errors, overflow and shutdown passed.\n";
    }
};

struct AppRegressionAccess {
    static void CheckMovementCache(const RecordingSession& session) {
        App app; app.currentSession_=session; app.RebuildPlotData();
        Check(!app.showLibrary_ && !app.highlightLatencyMatch_,"Startup must show recording with highlights off");
        const auto original=MouseLibrary::RecordingKey(app.currentSession_);
        const auto& curve=app.PrepareMovementPlot(false);
        Check(!curve.times.empty() && curve.times.size()<session.eventsA.size(),"Default comparison must use common time bins");
        const auto builds=curve.builds;
        for (int i=0;i<100;++i) app.PrepareMovementPlot(false);
        Check(app.movementA_.builds==builds,"Unchanged movement curves must not be rebuilt while panning");
        app.enableTimeBinning_=false; app.enableSmoothing_=false; app.enableGapInterpolation_=false;
        const auto& raw=app.PrepareMovementPlot(false);
        Check(raw.times==app.liveTimesA_ && raw.values==app.liveVelocitiesA_,"Raw reports must remain available without smoothing");
        app.enableYScaleB_=true; app.yScaleB_=2;
        const auto& b=app.PrepareMovementPlot(true);
        Check(b.values.front()==2*app.liveVelocitiesB_.front(),"Cached B must use the current scale");
        app.yScaleB_=3;
        Check(app.PrepareMovementPlot(true).values.front()==3*app.liveVelocitiesB_.front(),"Scale changes must invalidate cached B");
        Check(MouseLibrary::RecordingKey(app.currentSession_)==original,"Display preparation must preserve recorded events");
        auto changed=session; changed.eventsA[0].deltaX+=100;
        app.currentSession_=changed; app.RebuildPlotData();
        Check(app.PrepareMovementPlot(false).values.front()==app.liveVelocitiesA_.front(),"Loading an equal-length recording must invalidate the cache");
        app.state_=AppState::Recording;
        app.movementA_.updatedAt=std::chrono::steady_clock::now();
        const auto liveBuilds=app.movementA_.builds;
        auto extra=app.currentSession_.eventsA.back(); ++extra.timestamp;
        app.currentSession_.eventsA.push_back(extra);
        app.liveTimesA_.push_back(app.liveTimesA_.back()+0.001); app.liveVelocitiesA_.push_back(123);
        app.PrepareMovementPlot(false);
        Check(app.movementA_.builds==liveBuilds,"Live event changes must reuse a recent display cache");
        app.enableSmoothing_=true; app.PrepareMovementPlot(false);
        Check(app.movementA_.builds==liveBuilds+1,"Display setting changes must bypass the live throttle");
        ++extra.timestamp; app.currentSession_.eventsA.push_back(extra);
        app.liveTimesA_.push_back(app.liveTimesA_.back()+0.001); app.liveVelocitiesA_.push_back(124);
        app.state_=AppState::Ready; app.PrepareMovementPlot(false);
        Check(app.movementA_.key.events==app.currentSession_.eventsA.size(),"Stopping must display every final event immediately");
    }
    static void RunLibrary(RecordingSession session, const std::filesystem::path& directory) {
        App app;
        app.deviceManager_=std::make_unique<DeviceManager>(); app.dataStore_=std::make_unique<DataStore>();
        app.libraryPath_=directory/L"mice.json";
        const auto a=app.mouseLibrary_.AddMouse("Reference"), b=app.mouseLibrary_.AddMouse("Test");
        const auto sa=app.mouseLibrary_.AddSetup(a,"Wired",0,"Default"), sb=app.mouseLibrary_.AddSetup(b,"Wireless",0,"Performance mode");
        session.mouseA=app.mouseLibrary_.Identity(sa,"reference-interface"); session.mouseB=app.mouseLibrary_.Identity(sb,"test-interface");
        app.currentSession_=session; app.RebuildPlotData(); app.DetectSessionRates();
        Check(app.currentSession_.mouseA.pollingHz==8000 && app.currentSession_.mouseB.pollingHz==4000,
              "Recording completion must split detected rates into separate setups");
        app.StartLatencyAnalysis();
        Check(app.latencyTask_.wait_for(std::chrono::seconds(5))==std::future_status::ready,"Library analysis must finish");
        app.PollLatencyAnalysis();
        Check(app.mouseLibrary_.Comparisons().size()==1 && app.runsToKeep_.size()==1,"Valid named analysis must queue one comparison and recording");
        const auto key=MouseLibrary::RecordingKey(app.currentSession_);
        // A second accepted run can arrive before the first save finishes.
        ++app.currentSession_.endTimestamp;
        app.SaveComparison();
        const auto secondKey=MouseLibrary::RecordingKey(app.currentSession_);
        const auto blocked=directory/L"blocked";
        { std::ofstream output(blocked); output<<"Not a directory"; }
        app.libraryPath_=blocked/L"mice.json";
        app.PollLibrarySave(); app.librarySaveTask_.wait(); app.PollLibrarySave();
        Check(app.librarySaveFailed_ && app.runsToKeep_.size()==2 && !std::filesystem::exists(directory/L"mice.json"),
              "A failed save must keep pending recordings and must not publish a library that refers to unsaved files");
        app.libraryPath_=directory/L"mice.json"; app.librarySaveFailed_=false;
        for (int attempt=0;attempt<5 && (app.libraryDirty_ || !app.runsToKeep_.empty() || app.librarySaveTask_.valid());++attempt) {
            app.PollLibrarySave(); if (app.librarySaveTask_.valid()) app.librarySaveTask_.wait();
        }
        Check(!app.librarySaveFailed_ && app.runsToKeep_.empty(),"Background library persistence must complete");
        Check(std::filesystem::exists(directory/L"Runs"/(key+".json")),"Ranked run must survive outside the autosave folder");
        Check(std::filesystem::exists(directory/L"Runs"/(secondKey+".json")),"All pending comparison recordings must be saved before the library");
        app.OpenRankedRun(key);
        Check(app.currentSession_.mouseB.name=="Test" && app.currentSession_.mouseB.pollingHz==4000,"Opening a retained run must restore identities");
        ImGui::CreateContext(); ImPlot::CreateContext();
        auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2(1280,1000); io.DeltaTime=1.f/60;
        unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
        for (int frame=0;frame<4;++frame) {
            app.libraryPage_=frame/2;
            ImGui::NewFrame(); ImGui::SetNextWindowSize(ImVec2(1280,1000)); ImGui::Begin("Library UI");
            app.RenderMouseLibrary(); ImGui::End(); ImGui::Render();
            Check(ImGui::GetDrawData()->TotalVtxCount>0,"Mouse library page must render without a GPU");
        }
        MouseDevice previewDevice{}; previewDevice.path=L"test-interface"; previewDevice.name=L"Test device";
        for (int frame=0;frame<2;++frame) {
            ImGui::NewFrame(); ImGui::SetNextWindowSize(ImVec2(1280,1000)); ImGui::Begin("Link UI");
            ImGui::PushID("Reference A"); ImGui::OpenPopup("Link this device"); ImGui::PopID();
            app.RenderDeviceBinding("Reference A",&previewDevice);
            ImGui::End(); ImGui::Render();
            Check(ImGui::GetDrawData()->TotalVtxCount>0,"Device link dialog must render");
        }
        ImPlot::DestroyContext(); ImGui::DestroyContext();
        std::filesystem::remove(directory/L"Runs"/(key+".json")); std::filesystem::remove(directory/L"Runs"/(secondKey+".json"));
        std::filesystem::remove(directory/L"Runs"); std::filesystem::remove(blocked);
        std::filesystem::remove(directory/L"mice.json");
    }
    static void RunSoak(const RecordingSession& session, int seconds) {
        WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"RLA_SoakWindow";
        RegisterClassW(&wc);
        HWND window = CreateWindowW(wc.lpszClassName,L"RLA regression",WS_OVERLAPPEDWINDOW,0,0,1280,900,nullptr,nullptr,wc.hInstance,nullptr);
        Check(window != nullptr, "Soak window must initialize");
        {
            App app; Check(app.Initialize(window,1280,900), "Soak app must initialize");
            if (app.libraryLock_) { ReleaseMutex(app.libraryLock_); CloseHandle(app.libraryLock_); app.libraryLock_=nullptr; }
            app.libraryReadOnly_=true;
            ImGui::GetIO().IniFilename=nullptr;
            app.currentSession_=session; app.RebuildPlotData(); app.state_=AppState::Ready; app.showLibrary_=false;
            app.mouseLibrary_.autoRank=false; // Soak tests must not change the user's library.
            app.ApplyAutoScaleB(); app.StartLatencyAnalysis();
            const auto start=std::chrono::steady_clock::now(); int reported=-1; size_t frames=0;
            while (std::chrono::steady_clock::now()-start<std::chrono::seconds(seconds)) {
                MSG message{};
                PumpWindowMessages(message,[](MSG& msg,UINT first,UINT last){return PeekMessageW(&msg,nullptr,first,last,PM_REMOVE)!=0;},
                    [](MSG& msg){TranslateMessage(&msg); DispatchMessageW(&msg);});
                app.Update(); app.Render(); ++frames;
                const int elapsed=static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-start).count());
                if (elapsed/30 != reported) {
                    reported=elapsed/30;
                    app.OnResize(reported%2 ? 1200 : 1280,900);
                    Check(app.latencyFit_.valid || app.latencyTask_.valid(),"Replay latency analysis must produce a result");
                    Check(InputEngineRegressionAccess::Responsive(*app.inputEngine_), "Capture thread stopped responding during soak");
                    std::cout << "Soak " << elapsed << " s, " << frames << " frames, input responsive.\n" << std::flush;
                }
                Sleep(16); // UI test pacing only; the capture thread has no sleep.
            }
        }
        DestroyWindow(window); UnregisterClassW(wc.lpszClassName,wc.hInstance);
        std::cout << "Soak completed successfully.\n";
    }
    static void RunLatency(const RecordingSession& session) {
        App app;
        app.currentSession_ = session;
        app.RebuildPlotData();
        app.StartLatencyAnalysis();
        Check(app.latencyTask_.valid(), "Latency analysis must start in the background");
        Check(app.latencyTask_.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Latency worker must finish");
        app.PollLatencyAnalysis();
        Check(app.latencyFit_.valid, "Completed latency analysis must reach the UI");
        ImGui::CreateContext(); ImPlot::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 1000); io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        Check(app.selectedLatencyMatch_>=0,"Analysis must select a match for inspection");
        app.selectedLatencyMatch_ = 0; app.focusLatencyMatch_ = true; app.highlightLatencyMatch_=true;
        const auto match = app.latencyFit_.matches.front();
        for (int frame=0; frame<2; ++frame) {
            ImGui::NewFrame(); ImGui::SetNextWindowSize(ImVec2(1280, 1000)); ImGui::Begin("Latency UI");
            app.RenderPlotPanel(); ImGui::End(); ImGui::Render();
            Check(ImGui::GetDrawData()->TotalVtxCount > 0, "Latency markers must render");
        }
        auto& plots = ImPlot::GetCurrentContext()->Plots;
        Check(plots.GetBufSize() == 1, "Latency uses the movement plot");
        const auto* plot = plots.GetByIndex(0);
        Check(plot->Axes[ImAxis_X1].Range.Contains(match.timeA) && plot->Axes[ImAxis_X1].Range.Contains(match.timeB) &&
              plot->Axes[ImAxis_X1].Range.Size() < 50, "Selected match must center the movement plot");
        ImPlot::DestroyContext(); ImGui::DestroyContext();
        app.StartLatencyAnalysis();
        app.RebuildPlotData(); // Simulate loading a new recording while the worker runs.
        Check(app.latencyTask_.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Second latency worker must finish");
        app.PollLatencyAnalysis();
        Check(!app.latencyFit_.valid && app.latencyFit_.matches.empty(), "Stale results must not reach a different recording");
        app.state_ = AppState::Recording;
        app.StartLatencyAnalysis();
        Check(!app.latencyTask_.valid(), "Analysis must not read a live recording");
        app.state_ = AppState::Ready;
    }

    static void RunComparison(const RecordingSession& session) {
        App app;
        app.currentSession_ = session;
        app.RebuildPlotData();
        app.ApplyAutoScaleB();
        Check(app.enableYScaleB_ && app.enableTimeBinning_ && app.timeWeightedBins_ && app.timeBinMs_ == 2.0f,
              "Auto scale must enable comparable time bins");
        Check(std::abs(app.yScaleB_ - 2.0f) < 0.08f, "Auto scale must reach the plot controls");
        const float previous = app.yScaleB_;
        app.currentSession_.eventsB.clear();
        app.ApplyAutoScaleB();
        Check(app.yScaleB_ == previous, "A failed fit must preserve the previous scale");
        app.currentSession_ = session;

        // Exercise all plot modes without a GPU or real mouse input.
        ImGui::CreateContext();
        ImPlot::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 1000);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels; int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        for (int mode = 0; mode < 3; ++mode) {
            for (int frame = 0; frame < 2; ++frame) {
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(1280, 1000));
                ImGui::Begin("Regression UI");
                app.plotMode_ = mode;
                app.timeWeightedBins_ = frame == 0;
                app.showInstantHz_ = true;
                app.RenderPlotPanel();
                ImGui::End();
                ImGui::Render();
                Check(ImGui::GetDrawData()->TotalVtxCount > 0, "Plot mode must produce UI geometry");
            }
        }
        app.currentSession_ = PauseSession();
        app.RebuildPlotData();
        app.plotMode_ = 2;
        auto findPlot = [](const char* title) -> ImPlotPlot* {
            auto& plots = ImPlot::GetCurrentContext()->Plots;
            // A single-rate view has a new cell ID. Prefer its newly created
            // plot over the inactive rate plot from the two-row layout.
            for (int i = plots.GetBufSize() - 1; i >= 0; --i) {
                auto* plot = plots.GetByIndex(i);
                if (std::string_view(plot->GetTitle()) == title) return plot;
            }
            return nullptr;
        };
        // Check the actual axes after drawing, including hiding a gap that
        // previously expanded the Y axis and resetting an excessively wide view.
        for (int variant = 0; variant < 3; ++variant) {
            app.showTimingGaps_ = variant == 0;
            app.timingFitPending_ = true;
            if (variant == 2) {
                auto* plot = findPlot("Time between events");
                Check(plot != nullptr, "Timing plot must exist");
                plot->Axes[ImAxis_X1].Range = ImPlotRange(-13000, 23000);
                plot->Axes[ImAxis_Y1].Range = ImPlotRange(-500, 1500);
            }
            for (int frame = 0; frame < 2; ++frame) {
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(1280, 1000));
                ImGui::Begin("Regression UI");
                app.RenderPlotPanel();
                ImGui::End();
                ImGui::Render();
            }
            const auto* intervals = findPlot("Time between events");
            const auto* rates = findPlot("Movement event rate");
            Check(intervals && rates, "Both timing plots must render");
            for (const auto* plot : {intervals, rates}) {
                Check(plot->Axes[ImAxis_X1].Range.Min < 0 && plot->Axes[ImAxis_X1].Range.Max > 1450,
                      "Reset view must leave space around the recording");
            }
            Check(variant == 0 ? intervals->Axes[ImAxis_Y1].Range.Max >= 1000 : intervals->Axes[ImAxis_Y1].Range.Max < 20,
                  "Gap visibility and view reset must control interval axis fitting");
        }
        auto drawTiming = [&]() {
            for (int frame = 0; frame < 2; ++frame) {
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(1280, 1000));
                ImGui::Begin("Regression UI");
                app.RenderPlotPanel();
                ImGui::End();
                ImGui::Render();
            }
        };
        // User panning must remain free on both axes, including below zero.
        for (const char* title : {"Time between events", "Movement event rate"}) {
            auto* plot = findPlot(title);
            plot->Axes[ImAxis_X1].SetRange(-500, 1800);
            plot->Axes[ImAxis_X1].PushLinks();
            plot->Axes[ImAxis_Y1].SetRange(-100, 2200);
        }
        drawTiming();
        for (const char* title : {"Time between events", "Movement event rate"}) {
            const auto* plot = findPlot(title);
            Check(plot->Axes[ImAxis_X1].Range.Min == -500 && plot->Axes[ImAxis_Y1].Range.Min == -100,
                  "Rendering must preserve manual panning below zero");
        }
        const float compactHeight = findPlot("Time between events")->PlotRect.GetHeight();
        app.showTimingSettings_ = app.showTimingSummary_ = true;
        drawTiming();
        Check(findPlot("Time between events")->PlotRect.GetHeight() < compactHeight - 30,
              "Hiding settings and summary must return space to the graphs");
        app.showTimingSettings_ = app.showTimingSummary_ = false;
        for (int graph : {1, 2, 0}) {
            app.timingGraph_ = graph;
            app.timingFitPending_ = true;
            drawTiming();
            if (graph != 0) {
                const auto* plot = findPlot(graph == 1 ? "Time between events" : "Movement event rate");
                std::cout << "Timing graph " << graph << ": compact=" << compactHeight << ", single=" << plot->PlotRect.GetHeight() << '\n';
                Check(plot->PlotRect.GetHeight() > compactHeight * 1.5f,
                      "A single timing graph must use the available height");
            }
        }
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        app.RebuildPlotData();
        Check(!app.enableYScaleB_ && app.yScaleB_ == 1.0f && app.autoScaleMessage_.empty() && app.timingDirty_,
              "Loading a recording must clear old scale and timing state");
    }

    static void Run(const RecordingSession& session) {
        App app;
        app.currentSession_ = session;
        app.qpcFrequency_ = 10000000; // Deliberately differs from the imported recording.
        app.liveTimesA_ = { -123.0 };
        app.liveTimesB_ = { -123.0 };
        app.RebuildPlotData();
        Check(app.liveTimesA_.size() == 2 && app.liveTimesA_[0] == 1.0,
              "Loaded plot must replace stale data and use the saved frequency");
        Check(app.liveTimesB_.empty(), "Loading must clear stale Mouse B data");
        Check(std::abs(app.liveVelocitiesA_[0] - 500000.0) < 0.01,
              "Movement magnitude must not overflow");

        std::vector<double> times, values;
        app.timeWeightedBins_ = false; // Original whole-event totals remain available.
        app.ApplyTimeBinning(session.eventsA, session.startTimestamp, times, values, 1.0);
        Check(times.size() == 2 && times[0] == 1.5 && times[1] == 2.5,
              "Time binning must use the saved frequency");
        Check(std::abs(values[0] - 500000.0) < 0.01, "Binned magnitude must not overflow");
        app.InterpolateGaps({0, 1e12}, {2, 4}, times, values, 5, 1);
        Check(times.size() <= 102 && times.front() == 0 && times.back() == 1e12 &&
              std::is_sorted(times.begin(), times.end()), "Long idle gaps must have bounded, ordered plot points");
        Check(values.front() == 2 && values.back() == 4 && values[1] == 0, "Gap filling must preserve event values");
        app.InterpolateGaps({0, 10}, {2, 4}, times, values, 5, 0);
        Check(times.size() == 2, "Invalid interpolation spacing must not loop forever");

        app.deviceManager_ = std::make_unique<DeviceManager>();
        app.inputEngine_ = std::make_unique<InputEngine>();
        const auto mouse = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(1));
        app.deviceManager_->StartAssigningMouseA();
        app.deviceManager_->ProcessEvent({mouse, 0, 1000, 0});
        app.deviceManager_->StartAssigningMouseB();
        app.analysisResult_.valid = true;
        app.StartRecording();
        Check(!app.deviceManager_->IsAssigning(), "Recording must cancel device assignment");
        Check(!app.analysisResult_.valid && app.currentSession_.eventsA.empty(),
              "Recording must reset previous results and events");
        Check(app.currentSession_.qpcFrequency == 10000000,
              "A new recording must use the local clock");
        Check(app.keepCursorInWindow_ && !app.cursorConfined_,
              "Cursor confinement must default on but must not apply without an active app window");
        const auto start = app.currentSession_.startTimestamp;
        InputEngineRegressionAccess::Push(*app.inputEngine_, {mouse, start - 1, 10, 0});
        InputEngineRegressionAccess::Push(*app.inputEngine_, {mouse, start, 20, 0});
        InputEngineRegressionAccess::Push(*app.inputEngine_, {mouse, start + 1000000000, 30, 0});
        app.StopRecording();
        Check(app.currentSession_.eventsA.size() == 1 && app.currentSession_.eventsA[0].deltaX == 20,
              "Stopping must drain queued input and exclude events outside the recording");
        app.StartRecording();
        InputEngineRegressionAccess::EnableTestCapture(*app.inputEngine_);
        app.OnCaptureFocusLost();
        Check(app.state_ == AppState::Ready && !app.inputEngine_->IsRawCapture() && app.currentSession_.capture.legacySuppressed,
              "Focus loss must stop raw recording, restore controls and retain capture mode metadata");
        app.StartRecording();
        InputEngineRegressionAccess::EnableTestCapture(*app.inputEngine_);
        app.OnCaptureEscape();
        Check(app.state_ == AppState::Ready && !app.inputEngine_->IsRawCapture(), "Esc must stop raw capture");

        app.inputEngine_->UpdateEventRates(100, 200);
        Sleep(120);
        app.inputEngine_->UpdateEventRates(110, 210);
        Check(app.inputEngine_->GetEventRateA() > 0, "Event rates must update");
        app.inputEngine_->UpdateEventRates(0, 0);
        Check(app.inputEngine_->GetEventRateA() == 0 && app.inputEngine_->GetEventRateB() == 0,
              "Reset counts must not underflow");
        app.inputEngine_->ResetEventRates();
        Check(app.inputEngine_->GetEventRateA() == 0, "Explicit rate reset must clear the display");
    }
};
}

static RLA::RecordingSession ComparisonSession(int rateB = 1000, int delayMs = 4) {
    RLA::RecordingSession session;
    session.startTimestamp = 123456789;
    session.qpcFrequency = 1000000;
    session.endTimestamp = session.startTimestamp + 1200000;
    // Vary both direction and magnitude. Integral values allow exact splitting
    // of the same movement into 1 kHz and 8 kHz reports.
    for (int ms = 0; ms < 1000; ++ms) {
        const int x = 8 * static_cast<int>(std::round(20 + 12 * std::sin(ms * 0.065)));
        const int y = 8 * static_cast<int>(std::round(12 * std::cos(ms * 0.047)));
        session.eventsA.push_back({nullptr, session.startTimestamp + ms * 1000 + 100, 2 * x, 2 * y});
        const int reports = rateB / 1000;
        for (int k = 0; k < reports; ++k) {
            // Last 300 ms: mouse B moves independently in the opposite direction.
            session.eventsB.push_back({nullptr, session.startTimestamp + (ms + delayMs) * 1000 + k * (1000 / reports) + 100,
                ms < 700 ? x / reports : -3 * x / reports, ms < 700 ? y / reports : -3 * y / reports});
        }
    }
    return session;
}

static void RunBinningChecks() {
    using namespace RLA;
    std::vector<MouseEvent> events;
    constexpr int64_t start = 987654321;
    for (int i = 0; i < 1000; ++i)
        events.push_back({nullptr, start + i * 1000 + (i % 4 == 0 ? 100 : -100), 100, -50});
    const auto raw = Analyzer::BinMovement(events, start, 1000000, 2.0, false);
    const auto weighted = Analyzer::BinMovement(events, start, 1000000, 2.0, true);
    auto rms = [](const std::vector<MovementBin>& bins, double target) {
        double error = 0; size_t count = 0;
        for (const auto& bin : bins) {
            if (bin.index < 3 || bin.index > 490) continue;
            error += (bin.x - target) * (bin.x - target); ++count;
        }
        Check(count > 0, "Binning must produce interior samples");
        return std::sqrt(error / count);
    };
    const double rawError = rms(raw, 200), weightedError = rms(weighted, 200);
    Check(rawError > 50 && weightedError < rawError * 0.25,
          "Time weighting must reduce bin-boundary spikes in steady motion with arrival jitter");
    std::cout << "Steady-motion bin RMS error: event totals=" << rawError
              << ", time-weighted=" << weightedError << " counts (target 200).\n";

    auto conserved = [&](const std::vector<MouseEvent>& input, double binMs, bool useTime) {
        const auto bins = Analyzer::BinMovement(input, start, 1000000, binMs, useTime);
        double x = 0, y = 0, expectedX = 0, expectedY = 0;
        int64_t previous = -1;
        for (const auto& event : input) { expectedX += event.deltaX; expectedY += event.deltaY; }
        for (const auto& bin : bins) {
            Check(bin.index > previous, "Binning must produce ordered, unique bins");
            previous = bin.index; x += bin.x; y += bin.y;
        }
        Check(std::abs(x - expectedX) < 1e-7 && std::abs(y - expectedY) < 1e-7,
              "Binning must preserve signed X/Y counts");
        return bins;
    };
    for (double binMs : {0.1, 0.3, 1.0, 2.0, 7.5}) {
        conserved(events, binMs, false);
        conserved(events, binMs, true);
    }
    // Report period is not a divisor of the destination bin. Interior weighted
    // bins must still show a constant rate, independent of report/bin phase.
    for (size_t i = 0; i < events.size(); ++i) events[i].timestamp = start + 500 + i * 1001;
    const auto drift = conserved(events, 2.0, true);
    Check(rms(drift, 200000.0 / 1001) < 1e-8, "Clock drift must not cause whole-report spikes");

    std::vector<MouseEvent> gaps = {{nullptr, start + 1000, 10, -10}, {nullptr, start + 2000, 20, -20},
        {nullptr, start + 2000, -5, 5}, {nullptr, start + 3000, 10, -10},
        {nullptr, start + 3600000000LL, 40, -40}, {nullptr, start + 3600001000LL, 10, -10}};
    const auto sparse = conserved(gaps, 0.1, true);
    Check(sparse.size() < 50, "Long idle gaps must not generate interpolated movement or large allocations");
    for (const auto& bin : sparse)
        Check(bin.index < 31 || bin.index >= 36000000, "Movement must not spread into the idle gap");
    conserved({{nullptr, start, (std::numeric_limits<int32_t>::min)(), 0}}, 0.1, true);
    Check(Analyzer::BinMovement(events, start, 0, 1, true).empty(), "Invalid binning clocks must be rejected");
    Check(Analyzer::BinMovement(events, start, 1000000, 0, true).empty(), "Zero bin width must be rejected");
}

static void RunTimingPauseChecks() {
    using namespace RLA;
    const auto session = PauseSession();
    for (double window : {10.0, 100.0, 500.0}) {
        const auto timing = Analyzer::BuildEventTiming(session.eventsA, session.startTimestamp,
            session.qpcFrequency, 1450, window);
        Check(timing.longGapCount == 1 && timing.maxMs == 1000,
              "The pause must remain in raw statistics and the gap count");
        Check(timing.intervalsMs[200] == 1000 && std::isnan(timing.shortIntervalsMs[200]) &&
              std::isnan(timing.meanIntervalsMs[200]), "A pause must break the interval line and remain available as raw data");
        Check(timing.meanIntervalsMs[201] == 0.5 && timing.meanIntervalsMs.back() == 0.5,
              "The first regular report after a pause must restart the mean without the pause");
        for (double value : timing.meanIntervalsMs)
            Check(std::isnan(value) || std::abs(value - 0.5) < 1e-10, "The pause must not create a mean-interval ramp");
        bool zeroInPause = false;
        for (size_t i = 0; i < timing.ratesHz.size(); ++i) {
            if (timing.rateTimesMs[i] > 650 && timing.rateTimesMs[i] < 1000 && timing.ratesHz[i] == 0)
                zeroInPause = true;
        }
        Check(zeroInPause, "Window rates must still show zero during a pause");
    }
    const auto beforeResume = Analyzer::BuildEventTiming(
        std::vector<MouseEvent>(session.eventsA.begin(), session.eventsA.begin() + 201),
        session.startTimestamp, session.qpcFrequency, 1050, 100);
    Check(beforeResume.ratesHz.back() == 0 && beforeResume.meanIntervalsMs.back() == 0.5,
          "Live idle time must show zero rate without inventing intervals");
    const auto allIntervals = Analyzer::BuildEventTiming(session.eventsA, session.startTimestamp,
        session.qpcFrequency, 1450, 100, 2000);
    Check(allIntervals.longGapCount == 0 && allIntervals.shortIntervalsMs[200] == 1000,
          "The configurable threshold must control long-gap classification");
    std::cout << "Stop/restart timing checks passed.\n";
}

static void RunComparisonChecks() {
    using namespace RLA;
    for (int rate : {1000, 8000}) {
        const auto session = ComparisonSession(rate);
        const auto fit = Analyzer::FitScaleB(session);
        Check(fit.valid && std::abs(fit.scale - 2.0) < 0.08,
              "Fit must recover the scale with delay, mismatched sections and different event rates");
        Check(fit.matchedWindows < fit.testedWindows && fit.matchedWindows >= 3,
              "Fit must exclude independent movement");
    }
    auto session = ComparisonSession();
    AppRegressionAccess::RunComparison(session);
    // Small axis differences must not reject the dominant matching motion and
    // leave only a minority of aligned sections with a different scale.
    auto rotated = ComparisonSession(1000, 0);
    const double angle = 14.0 * 3.14159265358979323846 / 180.0;
    for (size_t i = 0; i < 700; ++i) {
        const double x = rotated.eventsA[i].deltaX, y = rotated.eventsA[i].deltaY;
        if (i < 200) {
            rotated.eventsB[i].deltaX = static_cast<int32_t>(std::round(x / 1.85));
            rotated.eventsB[i].deltaY = static_cast<int32_t>(std::round(y / 1.85));
        } else {
            rotated.eventsB[i].deltaX = static_cast<int32_t>(std::round((x * std::cos(angle) - y * std::sin(angle)) / 2));
            rotated.eventsB[i].deltaY = static_cast<int32_t>(std::round((x * std::sin(angle) + y * std::cos(angle)) / 2));
        }
    }
    const auto rotatedFit = Analyzer::FitScaleB(rotated);
    Check(rotatedFit.valid && std::abs(rotatedFit.scale - 2.0) < 0.03 && rotatedFit.matchedWindows >= 8,
          "Magnitude fitting must retain matching movement with small axis differences");
    // Aggregate both streams to 125 Hz to check adaptive bins for slower mice.
    auto slow = session;
    auto aggregate = [](const std::vector<MouseEvent>& input) {
        std::vector<MouseEvent> output;
        for (size_t i = 0; i + 7 < input.size(); i += 8) {
            auto event = input[i + 7]; event.deltaX = 0; event.deltaY = 0;
            for (size_t k = i; k < i + 8; ++k) { event.deltaX += input[k].deltaX; event.deltaY += input[k].deltaY; }
            output.push_back(event);
        }
        return output;
    };
    slow.eventsA = aggregate(session.eventsA); slow.eventsB = aggregate(session.eventsB);
    const auto slowFit = Analyzer::FitScaleB(slow);
    Check(slowFit.valid && slowFit.binMs == 8 && std::abs(slowFit.scale - 2.0) < 0.1,
          "Slow mice need larger common bins");
    auto conflicting = ComparisonSession(1000, 0);
    for (size_t i = 0; i < conflicting.eventsB.size(); ++i) {
        conflicting.eventsB[i].deltaX = conflicting.eventsA[i].deltaX / (i < 500 ? 2 : 4);
        conflicting.eventsB[i].deltaY = conflicting.eventsA[i].deltaY / (i < 500 ? 2 : 4);
    }
    Check(!Analyzer::FitScaleB(conflicting).valid, "Equally supported scales must be rejected");
    auto outliers = ComparisonSession(1000, 0);
    for (size_t i = 100; i < 200; ++i) {
        outliers.eventsB[i].deltaX *= 20; outliers.eventsB[i].deltaY *= 20;
    }
    const auto outlierFit = Analyzer::FitScaleB(outliers);
    Check(outlierFit.valid && std::abs(outlierFit.scale - 2.0) < 0.08,
          "Large collision amplitudes must not dominate the fit");
    for (auto& event : session.eventsB) { event.deltaX = -std::abs(event.deltaX); event.deltaY = 0; }
    Check(!Analyzer::FitScaleB(session).valid, "Opposite movement must not fit");
    session = ComparisonSession();
    for (auto& event : session.eventsA) { event.deltaX = 100; event.deltaY = 0; }
    for (auto& event : session.eventsB) { event.deltaX = 50; event.deltaY = 0; }
    Check(!Analyzer::FitScaleB(session).valid, "Constant movement has insufficient shape evidence");
    session.eventsA.resize(20); session.eventsB.resize(20);
    Check(!Analyzer::FitScaleB(session).valid, "Short recordings must not fit");
    session.eventsB.clear();
    Check(!Analyzer::FitScaleB(session).valid, "One mouse must not fit");
    session.qpcFrequency = 0;
    Check(!Analyzer::FitScaleB(session).valid, "Invalid clocks must not fit");

    std::vector<MouseEvent> events;
    for (int i = 0; i <= 1000; ++i) events.push_back({nullptr, 5000000 + i * 125, 1, 0});
    auto timing = Analyzer::BuildEventTiming(events, 5000000, 1000000, 250, 100);
    Check(timing.intervalsMs.size() == 1000 && timing.medianMs == 0.125 && timing.p95Ms == 0.125,
          "Intervals must use the recording clock and omit the first event");
    Check(timing.instantHz[0] == 8000 && timing.ratesHz[0] == 8000,
          "Both interval and window rates must report an exact 8 kHz stream");
    Check(timing.meanIntervalsMs.back() == 0.125, "Mean intervals must preserve a stable report stream");
    Check(timing.ratesHz.back() == 0, "Window rates must show idle time as zero");
    events = {{nullptr, 100, 1, 0}, {nullptr, 100, 1, 0}, {nullptr, 101, 1, 0}, {nullptr, 111, 1, 0}};
    timing = Analyzer::BuildEventTiming(events, 100, 1000, 20, 10);
    Check(timing.nonPositiveIntervals == 1 && std::isnan(timing.instantHz[0]),
          "Equal timestamps must not produce infinite Hz");
    Check(timing.medianMs == 5.5 && timing.p95Ms == 10 && timing.maxMs == 10,
          "Positive interval statistics must retain long gaps");
    Check(Analyzer::BuildEventTiming({}, 0, 1000, 100, 10).intervalsMs.empty(), "Empty input has no intervals");
    Check(Analyzer::BuildEventTiming(events, 100, 0, 20, 10).timesMs.empty(), "Invalid clocks have no timing");
    std::cout << "Scale, timing and plot-mode checks passed.\n";
}

static RLA::RecordingSession LatencySession(double delayMs = 2.75, int rateA = 8000, int rateB = 4000) {
    RLA::RecordingSession session;
    session.startTimestamp = 3000000;
    session.qpcFrequency = 1000000;
    session.endTimestamp = session.startTimestamp + 1300000;
    auto make = [&](std::vector<RLA::MouseEvent>& events, int rate, double scale, double delay) {
        const double step = 1000.0 / rate;
        auto position = [&](double t) { return std::llround(scale * 20000 * std::sin(t * 6.283185307179586 / 120)); };
        for (double t = step; t <= 1200; t += step) {
            const int dx = static_cast<int>(position(t) - position(t-step));
            if (dx) events.push_back({nullptr, session.startTimestamp + std::llround((50 + t + delay) * 1000), dx, 0});
        }
    };
    make(session.eventsA, rateA, 1, 0);
    make(session.eventsB, rateB, 0.45, delayMs);
    return session;
}

static void RunLatencyChecks() {
    using namespace RLA;
    AppRegressionAccess::RunLatency(LatencySession());
    for (double delay : {2.75, -1.625, 0.0}) for (int mode : {0, 1, 2}) {
        auto session = LatencySession(delay);
        const auto fit = Analyzer::FitLatency(session, mode);
        std::cout << "Latency mode=" << mode << " expected=" << delay << " fit=" << fit.differenceMs
                  << " cycles=" << fit.matchedCycles << " features=" << fit.matches.size() << " " << fit.message << '\n';
        Check(fit.valid && fit.matchedCycles >= 10 && std::abs(fit.differenceMs - delay) < 0.12,
              "Latency must recover signed delay across different report rates and amplitudes");
        for (const auto& match : fit.matches) {
            Check(mode != 0 || match.feature == LatencyFeature::Minimum, "Minima mode must contain only minima");
            Check(mode != 1 || match.feature != LatencyFeature::Minimum, "Half-height mode must contain only slopes");
        }
    }
    auto session = LatencySession();
    // Reject separate motion and collisions but retain clean shared cycles.
    for (auto& e : session.eventsB) {
        const auto t = e.timestamp - session.startTimestamp;
        if (t > 300000 && t < 600000) e.deltaX *= -1;
        if (t > 900000 && t < 902000) e.deltaX *= 20;
    }
    auto fit = Analyzer::FitLatency(session);
    Check(fit.valid && std::abs(fit.differenceMs - 2.75) < 0.15 && fit.matchedCycles < 20,
          "Mismatched motion must not bias the accepted cycles");
    for (auto* events : {&session.eventsA, &session.eventsB}) {
        std::erase_if(*events, [&](const MouseEvent& e) {
            const auto t = e.timestamp-session.startTimestamp; return t > 700000 && t < 1000000;
        });
    }
    fit = Analyzer::FitLatency(session);
    Check(fit.valid && std::abs(fit.differenceMs - 2.75) < 0.15, "Idle gaps must not become fitted movement minima");
    session = LatencySession();
    for (auto& e : session.eventsB) e.deltaX *= -1;
    Check(!Analyzer::FitLatency(session).valid, "Opposite movement must not produce a latency estimate");
    session = LatencySession();
    for (auto* events : {&session.eventsA, &session.eventsB}) for (auto& e : *events) { e.deltaX = 100; e.deltaY = 0; }
    Check(!Analyzer::FitLatency(session).valid, "Flat curves must not produce a latency estimate");
    session.eventsB.clear();
    Check(!Analyzer::FitLatency(session).valid, "Latency requires two mice");
    session = LatencySession(); session.eventsB[5].timestamp = 0;
    Check(!Analyzer::FitLatency(session).valid, "Unordered events must not be fitted");
    for (double delay : {-1.625,0.0,2.75}) {
        auto noisy=LatencySession(delay);
        for (auto* events : {&noisy.eventsA,&noisy.eventsB}) {
            std::vector<MouseEvent> reports;
            int64_t previous=noisy.startTimestamp;
            for (size_t i=0;i<events->size();++i) {
                auto e=(*events)[i];
                e.deltaX=static_cast<int32_t>(std::llround(e.deltaX*0.12));
                // Small counts, arrival jitter and occasional combined reports.
                if (i%13==0 && i+1<events->size()) {
                    const auto next=(*events)[++i]; e.deltaX+=static_cast<int32_t>(std::llround(next.deltaX*0.12)); e.timestamp=next.timestamp;
                }
                e.timestamp+=static_cast<int64_t>(i*37%151)-75;
                e.timestamp=(std::max)(e.timestamp,previous+1); previous=e.timestamp;
                if (e.deltaX) reports.push_back(e);
            }
            *events=std::move(reports);
        }
        for (int mode : {0,1,2}) {
            const auto result=Analyzer::FitLatency(noisy,mode);
            Check(result.valid && result.matchedCycles>=3 && std::abs(result.differenceMs-delay)<0.3,
                  "Quantized, jittered reports must retain a known latency without removing real shifts");
        }
    }
}

static void RunMouseLibraryChecks() {
    using namespace RLA;
    MouseLibrary library;
    const auto a=library.AddMouse("Reference"), b=library.AddMouse("Test mouse"), c=library.AddMouse("Third");
    const auto sa=library.AddSetup(a,"Wired",8000,""), sb=library.AddSetup(b,"Wireless",4000,"Performance"), sc=library.AddSetup(c,"Wired",1000,"");
    const auto wiredB=library.AddSetup(b,"Wired",1000,""), slowerB=library.AddSetup(b,"Wireless",2000,"Performance");
    Check(wiredB!=sb && slowerB!=sb && library.FindSetup(wiredB)->mouseId==library.FindSetup(sb)->mouseId,
          "Connection and rate setups must share a physical profile without mixing results");
    Check(library.AddSetup(b,"Wireless",4000,"Performance")==sb,"Identical setup detection must reuse its ID");
    library.Bind("hid-a",sa); library.Bind("hid-b",sb);
    Check(library.BoundSetup("hid-b")==sb && library.BoundSetup("other-port").empty(),"Only known interfaces may be linked automatically");
    Check(MouseLibrary::DeviceKey(std::wstring(L"HID#DEVICE\0ignored",18))=="hid#device","Device keys must ignore trailing terminators and letter case");
    library.LinkSetup(wiredB,a,"Relinked");
    Check(library.FindSetup(wiredB)->mouseId==a && library.FindSetup(sb)->mouseId==b,"Relinking must move only the selected setup");
    library.RenameMouse(b,"Renamed test"); Check(library.Label(sb).starts_with("Renamed test"),"Renaming must update setup labels");
    auto session=LatencySession(); session.mouseA=library.Identity(sa); session.mouseB=library.Identity(sb);
    auto fit=Analyzer::FitLatency(session);
    Check(library.AddComparison(session,fit,2),"Valid identified comparison must be stored");
    Check(library.AddComparison(session,fit,2) && library.Comparisons().size()==1,"Rerunning one recording must not add weight");
    library.SetEnabled(0,false); library.AddComparison(session,fit,2);
    Check(!library.Comparisons()[0].enabled,"Reanalysis must preserve an excluded run");
    library.SetEnabled(0,true);
    auto copied=session; copied.eventsA[0].deviceHandle=reinterpret_cast<HANDLE>(1);
    Check(MouseLibrary::RecordingKey(copied)==MouseLibrary::RecordingKey(session),"Process-local handles must not change recording identity");
    copied.eventsA[0].deltaX++;
    Check(MouseLibrary::RecordingKey(copied)!=MouseLibrary::RecordingKey(session),"Different recorded counts must have different identities");
    auto mislabeled=session; mislabeled.mouseB.pollingHz=8000;
    Check(!library.AddComparison(mislabeled,fit,2),"Conflicting saved settings must not enter the ranking");
    const auto unknownRate=library.AddSetup(b,"Wireless",0,"");
    mislabeled.mouseB=library.Identity(unknownRate);
    Check(!library.AddComparison(mislabeled,fit,2),"An unknown rate must not mix different polling settings in the ranking");
    fit.valid=false; Check(!library.AddComparison(session,fit,2),"Failed fits must not enter the ranking");
    fit.valid=true; fit.binMs=.25; fit.spreadMs=.1; fit.matchedCycles=20;
    // Replace the first result with a known constraint, then add a connected graph.
    fit.differenceMs=2; library.AddComparison(session,fit,2);
    int run=0;
    auto add=[&](const std::string& left,const std::string& right,double difference,int method=2) {
        auto sample=LatencySession(); const int64_t shift=++run*2000000;
        sample.startTimestamp+=shift; sample.endTimestamp+=shift;
        for (auto* events : {&sample.eventsA,&sample.eventsB}) for (auto& e : *events) e.timestamp+=shift;
        sample.mouseA=library.Identity(left); sample.mouseB=library.Identity(right); fit.differenceMs=difference;
        Check(library.AddComparison(sample,fit,method),"Test graph comparison must be stored");
    };
    add(sa,sb,2); add(sa,sb,9); // Pair median remains two despite one bad run.
    add(sb,sc,3); add(sc,sa,-5); // Reverse orientation must preserve the sign.
    add(wiredB,slowerB,-1); // No common reference: separate group.
    add(sa,sb,-7,0); // A different analysis method must remain separate.
    const auto ranks=library.Ranking(2);
    auto row=[&](const std::string& id)->const MouseRank& { for (const auto& r:ranks) if (r.setupId==id) return r; throw std::runtime_error("Rank missing"); };
    Check(std::abs(row(sa).relativeMs)<1e-8 && std::abs(row(sb).relativeMs-2)<1e-8 && std::abs(row(sc).relativeMs-5)<1e-8,
          "Relative ranking must solve consistent comparison paths and resist a pair outlier");
    Check(row(sa).group==row(sc).group && row(wiredB).group!=row(sa).group && std::abs(row(slowerB).relativeMs)<1e-8,
          "Disconnected comparison groups must never receive a shared rank");
    for (int rate : {125,500,1000,2000,4000,8000}) {
        const auto recording=LatencySession(0,rate,rate);
        std::cout << "Rate expected=" << rate << " detected=" << MouseLibrary::DetectRate(recording.eventsA,recording.qpcFrequency) << '\n';
        Check(MouseLibrary::DetectRate(recording.eventsA,recording.qpcFrequency)==rate,"Steady movement must detect the nominal rate");
    }
    auto grouped=LatencySession();
    for (size_t i=0;i<grouped.eventsA.size();++i) grouped.eventsA[i].timestamp=grouped.startTimestamp+static_cast<int64_t>(i/100)*15000;
    Check(MouseLibrary::DetectRate(grouped.eventsA,grouped.qpcFrequency)==0,"Batched arrivals must not receive a confident Hz label");
    Check(MouseLibrary::DetectRate({},1000000)==0,"No movement means unknown Hz");
    auto sparse=LatencySession();
    for (auto& e : sparse.eventsA) { e.deltaX=1; e.deltaY=0; }
    Check(MouseLibrary::DetectRate(sparse.eventsA,sparse.qpcFrequency)==0,"Sparse one-count movement must not imply a hardware polling rate");
    const auto directory=std::filesystem::temp_directory_path()/(L"RLA-library-test-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory); const auto path=directory/L"library.json";
    Check(library.Save(path).empty(),"Library save must succeed");
    MouseLibrary restored; std::string error;
    Check(restored.Load(path,error) && restored.Comparisons().size()==library.Comparisons().size() && restored.BoundSetup("hid-b")==sb,
          "Profiles, links, and comparisons must survive restart");
    nlohmann::json document; {std::ifstream input(path); input>>document;}
    document["comparisons"][0]["setupA"]="missing";
    {std::ofstream output(path); output<<document;}
    Check(!restored.Load(path,error) && restored.Mice().size()==library.Mice().size(),"Invalid library loads must preserve memory and report failure");
    DataStore store; const auto recordingPath=directory/L"session.json";
    Check(store.SaveToJson(recordingPath,session,{},L"A",L"B"),"Profile session save must succeed");
    const auto loaded=store.LoadFromJson(recordingPath);
    Check(loaded && loaded->mouseB.setupId==sb && loaded->mouseB.name=="Renamed test" && loaded->mouseB.pollingHz==4000,
          "Saved sessions must retain names, setup IDs, and rates");
    MouseLibrary imported; imported.ImportIdentity(loaded->mouseA); imported.ImportIdentity(loaded->mouseB);
    Check(imported.Mice().size()==2 && imported.FindSetup(sb),"Session identities must import without live hardware");
    AppRegressionAccess::RunLibrary(LatencySession(),directory);
    std::filesystem::remove(path); std::filesystem::remove(recordingPath); std::filesystem::remove(directory);
    std::cout << "Mouse profiles, links, rates, ranking graphs, deduplication, persistence and UI checks passed.\n";
}

static void RunAutosaveChecks() {
    using namespace RLA;
    const auto directory = std::filesystem::temp_directory_path() / (L"RLA-autosave-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    const auto manual = directory / L"manual.json";
    { std::ofstream file(manual); file << "keep me"; }
    const auto child = directory / L"RLA-autosave-subdirectory.json";
    std::filesystem::create_directory(child);
    auto wait = [](Autosave& archive) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (archive.GetStatus().busy && std::chrono::steady_clock::now() < deadline) Sleep(1);
        Check(!archive.GetStatus().busy, "Autosave worker must complete");
        return archive.GetStatus();
    };
    {
        Autosave archive(directory);
        archive.Save(LatencySession(), L"Reference", L"Test");
        archive.Save(LatencySession(-1.0), L"Reference", L"Test");
        const auto status = wait(archive);
        Check(!status.lastFile.empty(), "Completed recording must autosave");
        DataStore store;
        const auto loaded = store.LoadFromJson(status.lastFile);
        Check(loaded && loaded->eventsB.front().timestamp == LatencySession(-1.0).eventsB.front().timestamp,
              "Autosave must preserve the recording snapshot");
        size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.is_regular_file() && entry.path() != manual) ++files;
        Check(files == 2, "Consecutive recordings must have distinct autosaves");
        Check(archive.Clear(), "Clear must start when idle");
        Check(wait(archive).lastFile.empty(), "Clearing must remove the last autosave reference");
        Check(std::filesystem::exists(manual) && std::filesystem::is_directory(child), "Clear must preserve manual saves and directories");
        files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) if (entry.is_regular_file()) ++files;
        Check(files == 1, "Clear must remove owned autosaves");
        archive.Save(LatencySession(), L"Reference", L"Test");
        // Destructor must finish a queued save.
    }
    { Autosave archive(directory); Check(archive.Clear(), "Reopened archive must clear saved files"); wait(archive); }
    {
        Autosave badArchive(manual); // File where a directory is required.
        badArchive.Save(LatencySession(), L"Reference", L"Test");
        Check(wait(badArchive).message.starts_with("Autosave failed:"), "Disk failures must reach the UI status");
    }
    std::filesystem::remove(child);
    std::filesystem::remove(manual);
    std::filesystem::remove(directory);
    std::cout << "Autosave snapshot, queue, clear, shutdown and error checks passed.\n";
}

int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--soak") {
            auto session=LatencySession();
            if (argc>3) { RLA::DataStore store; auto loaded=store.LoadFromJson(argv[3]); Check(loaded.has_value(),"Could not load soak recording"); session=std::move(*loaded); }
            RLA::AppRegressionAccess::RunSoak(session, argc > 2 ? std::stoi(argv[2]) : 360); return 0;
        }
        // Optional targeted check on a local recording. No recording is copied
        // into the repository. The expected manual scale is supplied by the user.
        if (argc > 1) {
            RLA::DataStore store;
            const auto recording = store.LoadFromJson(argv[1]);
            Check(recording.has_value(), "Could not load the supplied recording");
            if (argc > 2 && std::string_view(argv[2]) == "--latency") {
                for (int mode : {0, 1, 2}) {
                    const auto started = std::chrono::steady_clock::now();
                    const auto latency = RLA::Analyzer::FitLatency(*recording, mode);
                    std::cout << "Latency mode=" << mode << " valid=" << latency.valid << " B-A=" << latency.differenceMs
                              << " ms MAD=" << latency.spreadMs << " bins=" << latency.binMs << " cycles=" << latency.matchedCycles
                              << " points=" << latency.matches.size() << " candidates=" << latency.candidatePairs << " elapsed="
                              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-started).count() << " ms\n"
                              << latency.message << '\n';
                }
                return 0;
            }
            const auto fit = RLA::Analyzer::FitScaleB(*recording);
            Check(fit.valid, "The supplied recording must have a valid scale fit");
            std::cout << "Recording scale=" << fit.scale << ", matched sections=" << fit.matchedWindows
                      << "/" << fit.testedWindows << '\n';
            if (argc > 2) {
                const double expected = std::stod(argv[2]);
                Check(expected > 0 && std::abs(fit.scale / expected - 1) < 0.01,
                      "Recording fit must be within 1 percent of the supplied manual scale");
                std::cout << "Manual scale check passed (within 1 percent).\n";
            }
            return 0;
        }
        RLA::InputEngineRegressionAccess::Run();
        RunWindowMessageChecks();
        RunFramePacingChecks();
        RLA::AppRegressionAccess::CheckMovementCache(LatencySession());
        RLA::RendererRegressionAccess::Run();
        RunLatencyChecks();
        RunMouseLibraryChecks();
        RunAutosaveChecks();
        RunBinningChecks();
        RunTimingPauseChecks();
        RunComparisonChecks();
        using namespace RLA;
        using json = nlohmann::json;
        const auto directory = std::filesystem::temp_directory_path() /
            (L"RLA-regression-" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        const auto path = directory / L"session.json";
        const auto csv = directory / L"session.csv";
        const auto invalidPath = directory / L"invalid.json";
        RecordingSession session;
        session.startTimestamp = 100;
        session.endTimestamp = 110;
        session.qpcFrequency = 1000;
        session.capture = {true, 16000, 12000, 64, 1, 2, ERROR_ACCESS_DENIED, true};
        session.eventsA = { {nullptr, 101, 300000, 400000},
                            {nullptr, 102, (std::numeric_limits<int32_t>::min)(), 0} };
        DataStore store;
        Check(store.SaveToJson(path, session, {}, L"Mouse A", L"Mouse B"), "Save failed");
        auto loaded = store.LoadFromJson(path);
        Check(loaded && loaded->capture.available && loaded->capture.packets == 16000 &&
              loaded->capture.groupedPackets == 12000 && loaded->capture.maxBatch == 64 &&
              loaded->capture.readErrors == 1 && loaded->capture.droppedEvents == 2 &&
              loaded->capture.lastError == ERROR_ACCESS_DENIED && loaded->capture.legacySuppressed,
              "Capture diagnostics must survive JSON round trip");
        Check(loaded && loaded->eventsA.size() == 2 && loaded->eventsA[1].deltaX == session.eventsA[1].deltaX,
              "JSON round trip must preserve movement values");
        AppRegressionAccess::Run(*loaded);
        Check(store.ExportToCsv(csv, *loaded), "CSV export failed");
        std::ifstream csvInput(csv);
        std::string csvText((std::istreambuf_iterator<char>(csvInput)), {});
        Check(csvText.find("1000.00,A,300000,400000,500000.00") != std::string::npos,
              "CSV must use the saved clock and correct magnitude");
        Check(csvText.find("2147483648.00") != std::string::npos, "INT32_MIN magnitude must be valid");
        csvInput.close();

        json baseline;
        { std::ifstream input(path); input >> baseline; }
        auto reject = [&](json document) {
            { std::ofstream output(invalidPath); output << document; }
            Check(!store.LoadFromJson(invalidPath), "Invalid session must be rejected");
            Check(!store.GetLastError().empty(), "Load failure must have an error message");
        };
        auto invalid = baseline; invalid.erase("session"); reject(invalid);
        invalid = baseline; invalid["capture"]["packets"] = -1; reject(invalid);
        invalid = baseline; invalid["version"] = 2; reject(invalid);
        invalid = baseline; invalid["session"]["qpcFrequency"] = 0; reject(invalid);
        invalid = baseline; invalid["session"]["qpcFrequency"] = 0.5; reject(invalid);
        invalid = baseline; invalid["eventsA"] = json::object(); reject(invalid);
        invalid = baseline; invalid["eventsA"][0]["dx"] = 2147483648LL; reject(invalid);
        invalid = baseline; invalid["eventsA"][0]["t"] = 99; reject(invalid);
        invalid = baseline; invalid["eventsA"][1]["t"] = 100; reject(invalid);
        invalid = baseline; invalid["eventsA"][1]["t"] = 111; reject(invalid);
        Check(store.LoadFromJson(path).has_value() && store.GetLastError().empty(),
              "A successful operation must clear the previous error");
        Check(store.LoadFromJson("session.json").has_value(), "Existing repository session must still load");

        // Invalid export must leave an existing file intact.
        session.qpcFrequency = 0;
        Check(!store.ExportToCsv(csv, session), "Invalid export must fail");
        { std::ifstream input(csv); std::string text((std::istreambuf_iterator<char>(input)), {});
          Check(text == csvText, "Invalid export must preserve the previous file"); }
        std::filesystem::remove(path);
        std::filesystem::remove(csv);
        std::filesystem::remove(invalidPath);
        std::filesystem::remove(directory);
        std::cout << "All regression checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
