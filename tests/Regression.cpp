#include "src/App.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <imgui.h>
#include <implot.h>

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

namespace RLA {
struct InputEngineRegressionAccess {
    static void Push(InputEngine& engine, const MouseEvent& event) {
        Check(engine.eventBuffer_.push(event), "Test event could not be queued");
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

int main() {
    try {
        RunBinningChecks();
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
        session.eventsA = { {nullptr, 101, 300000, 400000},
                            {nullptr, 102, (std::numeric_limits<int32_t>::min)(), 0} };
        DataStore store;
        Check(store.SaveToJson(path, session, {}, L"Mouse A", L"Mouse B"), "Save failed");
        auto loaded = store.LoadFromJson(path);
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
