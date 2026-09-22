#pragma once
#include "Analyzer.h"
#include <filesystem>
#include <map>

namespace RLA {
struct SavedMouse { std::string id, name; };
struct MouseSetup {
    std::string id, mouseId, connection, label;
    int pollingHz = 0;
};
struct SavedComparison {
    std::string recordingKey, setupA, setupB, savedAt;
    int method = 2;
    double differenceMs = 0, spreadMs = 0, binMs = 0;
    size_t cycles = 0;
    bool enabled = true;
};
struct MouseRank {
    std::string setupId;
    int group = 0, place = 0; // Group zero means not yet compared.
    double relativeMs = 0, residualMs = 0;
    size_t recordings = 0;
};

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
    bool AddComparison(const RecordingSession& session, const LatencyFit& fit, int method);
    void SetEnabled(size_t index, bool enabled);
    std::vector<MouseRank> Ranking(int method) const;
    bool Load(const std::filesystem::path& path, std::string& error);
    std::string Save(const std::filesystem::path& path) const; // Empty string means success.
    static std::string DeviceKey(const std::wstring& path);
    static std::string RecordingKey(const RecordingSession& session);
    static int DetectRate(const std::vector<MouseEvent>& events, double frequency);
private:
    std::vector<SavedMouse> mice_;
    std::vector<MouseSetup> setups_;
    std::map<std::string, std::string> bindings_;
    std::vector<SavedComparison> comparisons_;
};
}
