#pragma once
#include "Analyzer.h"
#include <filesystem>
#include <limits>
#include <map>

namespace RLA {
// Saved result ids. 0-4 are time-weighted pipeline results; 4 is the current
// feature set. 5 is the arrival-time data-age result of the same features.
// Rankings with PipelineRanking use the newest pipeline id of each recording.
// Rankings with DataAge use the measured result, or calculate it from the
// newest pipeline result when a recording has none (see ResultRows).
namespace LatencyMethod {
constexpr int Pipeline = 4, DataAge = 5, Last = DataAge, PipelineRanking = -1;
constexpr bool IsDataAge(int method) { return method == DataAge; }
}
struct SavedMouse { std::string id, name; };
struct MouseSetup {
    std::string id, mouseId, connection, label;
    int pollingHz = 0;
};
struct SavedComparison {
    std::string recordingKey, setupA, setupB, savedAt;
    int method = LatencyMethod::Pipeline;
    double differenceMs = 0, spreadMs = 0, binMs = 0;
    size_t cycles = 0;
    bool enabled = true;
    // Mouse angle of the recording (Analyzer::FitOrientation, B onto A). NaN if unknown.
    double angleDeg = std::numeric_limits<double>::quiet_NaN();
};
struct ResultRow {
    SavedComparison run;
    size_t index = 0;        // Stored comparison this row comes from.
    bool calculated = false; // Data age = pipeline + (T_B - T_A) / 2, not measured.
};
struct DirectRunSummary {
    size_t recordings = 0;
    double fastestMs = 0, latestMs = 0;
    std::string fastestKey, latestKey;
};
struct MouseRank {
    std::string setupId;
    int group = 0, place = 0; // Group zero means not yet compared.
    double relativeMs = 0, residualMs = 0;
    size_t recordings = 0;
    size_t pairs = 0, correctedPairs = 0; // Mouse pairs of this setup; pairs with both placements when corrected.
};

// Runs of one setup pair split by mouse placement. Values are second minus
// first (setup IDs in sorted order), so A/B order does not matter. A placement
// is the sign of the mouse angle; |angle| below PlacementAngleDeg is unassigned.
struct PlacementGroup {
    size_t runs = 0;
    double medianMs = 0, meanAngleDeg = 0;
};
struct PlacementBalance {
    std::string first, second;
    PlacementGroup positive, negative, aligned;
    bool balanced = false;  // Both placements present.
    double balancedMs = 0;  // Mean of the two placement medians.
};
struct PlacementNote {
    size_t same = 0, opposite = 0;  // Other enabled runs of the pair with a known angle.
    double oppositeAngleDeg = 0;    // Mean angle of the opposite runs, in this run's A/B order.
};
constexpr double PlacementAngleDeg = 2.0;

class MouseLibrary {
public:
    bool autoRank = true;
    const std::vector<SavedMouse>& Mice() const { return mice_; }
    const std::vector<MouseSetup>& Setups() const { return setups_; }
    const std::vector<SavedComparison>& Comparisons() const { return comparisons_; }
    const SavedMouse* FindMouse(const std::string& id) const;
    const MouseSetup* FindSetup(const std::string& id) const;
    std::string AddMouse(std::string name);
    void RenameMouse(const std::string& id, std::string name);
    std::string AddSetup(const std::string& mouseId, std::string connection, int pollingHz, std::string label);
    void LinkSetup(const std::string& setupId, const std::string& mouseId, std::string label);
    void Bind(const std::string& devicePath, const std::string& setupId);
    std::string BoundSetup(const std::string& devicePath) const;
    std::string Label(const std::string& setupId) const;
    RecordingMouse Identity(const std::string& setupId, std::string devicePath = {}) const;
    void ImportIdentity(const RecordingMouse& identity);
    // One result per recording and method. A rerun replaces it instead of adding weight.
    // DataAge requires an arrival-time fit; other methods require a time-weighted fit.
    bool AddComparison(const RecordingSession& session, const LatencyFit& fit, int method);
    void SetEnabled(size_t index, bool enabled);
    // Sets the angle of every result of one recording. Returns the number of results changed.
    size_t SetAngle(const std::string& recordingKey, double angleDeg);
    std::vector<std::string> KeysWithoutAngle() const;
    // One row per recording (newest method of the selected kind), enabled runs only.
    std::vector<PlacementBalance> Placements(bool dataAge) const;
    PlacementNote Placement(const std::string& setupA, const std::string& setupB, double angleDeg,
        const std::string& excludeKey) const;
    void DeleteMouse(const std::string& id);
    void DeleteSetup(const std::string& id);
    void DeleteComparison(const std::string& key, int method);
    // placementCorrected: a pair with runs in both placements uses the mean of the
    // two placement medians (PlacementBalance::balancedMs) instead of the median of all runs.
    std::vector<MouseRank> Ranking(int method, bool placementCorrected = false) const;
    // Rows of one result kind. Pipeline: every pipeline result (ids 0-4). Data age:
    // the measured result, or for a recording without one, its newest pipeline
    // result plus (T_B - T_A) / 2 with T = 1000 / detected Hz of each setup.
    std::vector<ResultRow> ResultRows(bool dataAge) const;
    double ReportIntervalMs(const std::string& setupId) const; // 0 if unknown.
    DirectRunSummary DirectRuns(const std::string& setup, const std::string& reference, int method) const;
    bool Load(const std::filesystem::path& path, std::string& error);
    std::string Save(const std::filesystem::path& path) const; // Empty string means success.
    static std::string DeviceKey(const std::wstring& path);
    static std::string RecordingKey(const RecordingSession& session);
    static int DetectRate(const std::vector<MouseEvent>& events, double frequency);
private:
    std::vector<SavedComparison> Selected(int method) const; // Enabled runs for rankings.
    std::vector<SavedMouse> mice_;
    std::vector<MouseSetup> setups_;
    std::map<std::string, std::string> bindings_;
    std::vector<SavedComparison> comparisons_;
};
}
