#include "src/App.h"
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
        });
        const auto stats = engine->GetCaptureDiagnostics();
        Check(received == 16000 && stats.packets == 16000 && stats.groupedPackets == 16000 &&
              stats.maxBatch == 64 && stats.droppedEvents == 0 && stats.readErrors == 0,
              "Buffered diagnostics must account for every report");

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
        while (engine->eventBuffer_.push({nullptr, 0, 1, 0})) {}
        Check(engine->ProcessRawBatch(reinterpret_cast<BYTE*>(&raw), sizeof(raw), 1, 102), "A full ring must not block capture");
        Check(engine->GetCaptureDiagnostics().droppedEvents == 1, "Ring overflow must be counted");
        engine.reset();
        // Exercise actual registration, idle waiting and shutdown without a visible window.
        engine = std::make_unique<InputEngine>();
        Check(engine->Initialize() && engine->Start(), "Buffered input thread must initialize");
        Sleep(20);
        engine.reset();
        std::cout << "Buffered capture: 16000 reports preserved; resize, errors, overflow and shutdown passed.\n";
    }
};

struct AppRegressionAccess {
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
        const auto start = app.currentSession_.startTimestamp;
        InputEngineRegressionAccess::Push(*app.inputEngine_, {mouse, start - 1, 10, 0});
        InputEngineRegressionAccess::Push(*app.inputEngine_, {mouse, start, 20, 0});
        InputEngineRegressionAccess::Push(*app.inputEngine_, {mouse, start + 1000000000, 30, 0});
        app.StopRecording();
        Check(app.currentSession_.eventsA.size() == 1 && app.currentSession_.eventsA[0].deltaX == 20,
              "Stopping must drain queued input and exclude events outside the recording");

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

int main(int argc, char** argv) {
    try {
        // Optional targeted check on a local recording. No recording is copied
        // into the repository. The expected manual scale is supplied by the user.
        if (argc > 1) {
            RLA::DataStore store;
            const auto recording = store.LoadFromJson(argv[1]);
            Check(recording.has_value(), "Could not load the supplied recording");
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
        session.capture = {true, 16000, 12000, 64, 1, 2, ERROR_ACCESS_DENIED};
        session.eventsA = { {nullptr, 101, 300000, 400000},
                            {nullptr, 102, (std::numeric_limits<int32_t>::min)(), 0} };
        DataStore store;
        Check(store.SaveToJson(path, session, {}, L"Mouse A", L"Mouse B"), "Save failed");
        auto loaded = store.LoadFromJson(path);
        Check(loaded && loaded->capture.available && loaded->capture.packets == 16000 &&
              loaded->capture.groupedPackets == 12000 && loaded->capture.maxBatch == 64 &&
              loaded->capture.readErrors == 1 && loaded->capture.droppedEvents == 2 &&
              loaded->capture.lastError == ERROR_ACCESS_DENIED, "Capture diagnostics must survive JSON round trip");
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
