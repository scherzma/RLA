#pragma once

#include "Types.h"

#include <string>
#include <optional>
#include <filesystem>

namespace RLA {

class DataStore {
public:
    DataStore() = default;
    ~DataStore() = default;

    // Save session to JSON file
    bool SaveToJson(const std::filesystem::path& path,
                    const RecordingSession& session,
                    const AnalysisResult& result,
                    const std::wstring& mouseAName,
                    const std::wstring& mouseBName);

    // Load session from JSON file
    std::optional<RecordingSession> LoadFromJson(const std::filesystem::path& path);

    // Export to CSV for spreadsheet analysis
    bool ExportToCsv(const std::filesystem::path& path,
                     const RecordingSession& session);

    // Get last error message
    const std::string& GetLastError() const { return lastError_; }

private:
    std::string lastError_;
};

} // namespace RLA
