#include "DataStore.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>

namespace RLA {

using json = nlohmann::json;

// Helper to convert wide string to UTF-8
static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";

    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

// Helper to convert UTF-8 to wide string
static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";

    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                   nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        result.data(), size);
    return result;
}

// Get current ISO 8601 timestamp
static std::string GetIsoTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::tm tm;
    localtime_s(&tm, &time);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

bool DataStore::SaveToJson(const std::filesystem::path& path,
                           const RecordingSession& session,
                           const AnalysisResult& result,
                           const std::wstring& mouseAName,
                           const std::wstring& mouseBName) {
    try {
        json j;

        j["version"] = 1;
        j["timestamp"] = GetIsoTimestamp();

        j["mouseA"] = {
            {"name", WideToUtf8(mouseAName)},
            {"eventCount", session.eventsA.size()}
        };

        j["mouseB"] = {
            {"name", WideToUtf8(mouseBName)},
            {"eventCount", session.eventsB.size()}
        };

        j["analysis"] = {
            {"valid", result.valid},
            {"latencyDiffMicroseconds", result.latencyDiffMicroseconds},
            {"impactTimestampA", result.impactTimestampA},
            {"impactTimestampB", result.impactTimestampB},
            {"errorMessage", result.errorMessage}
        };

        j["session"] = {
            {"startTimestamp", session.startTimestamp},
            {"endTimestamp", session.endTimestamp},
            {"qpcFrequency", session.qpcFrequency}
        };

        // Store events
        json eventsA = json::array();
        for (const auto& e : session.eventsA) {
            eventsA.push_back({
                {"t", e.timestamp},
                {"dx", e.deltaX},
                {"dy", e.deltaY}
            });
        }
        j["eventsA"] = eventsA;

        json eventsB = json::array();
        for (const auto& e : session.eventsB) {
            eventsB.push_back({
                {"t", e.timestamp},
                {"dx", e.deltaX},
                {"dy", e.deltaY}
            });
        }
        j["eventsB"] = eventsB;

        // Write to file
        std::ofstream file(path);
        if (!file) {
            lastError_ = "Failed to open file for writing: " + path.string();
            return false;
        }

        file << std::setw(2) << j;
        return true;
    }
    catch (const std::exception& e) {
        lastError_ = std::string("JSON save error: ") + e.what();
        return false;
    }
}

std::optional<RecordingSession> DataStore::LoadFromJson(const std::filesystem::path& path) {
    try {
        std::ifstream file(path);
        if (!file) {
            lastError_ = "Failed to open file for reading: " + path.string();
            return std::nullopt;
        }

        json j = json::parse(file);

        RecordingSession session;

        // Load session metadata
        if (j.contains("session")) {
            session.startTimestamp = j["session"]["startTimestamp"].get<int64_t>();
            session.endTimestamp = j["session"]["endTimestamp"].get<int64_t>();
            session.qpcFrequency = j["session"]["qpcFrequency"].get<double>();
        }

        // Load events A
        if (j.contains("eventsA")) {
            for (const auto& e : j["eventsA"]) {
                MouseEvent event{};
                event.deviceHandle = nullptr; // Handle not preserved
                event.timestamp = e["t"].get<int64_t>();
                event.deltaX = e["dx"].get<int32_t>();
                event.deltaY = e["dy"].get<int32_t>();
                session.eventsA.push_back(event);
            }
        }

        // Load events B
        if (j.contains("eventsB")) {
            for (const auto& e : j["eventsB"]) {
                MouseEvent event{};
                event.deviceHandle = nullptr;
                event.timestamp = e["t"].get<int64_t>();
                event.deltaX = e["dx"].get<int32_t>();
                event.deltaY = e["dy"].get<int32_t>();
                session.eventsB.push_back(event);
            }
        }

        return session;
    }
    catch (const std::exception& e) {
        lastError_ = std::string("JSON load error: ") + e.what();
        return std::nullopt;
    }
}

bool DataStore::ExportToCsv(const std::filesystem::path& path,
                            const RecordingSession& session) {
    try {
        std::ofstream file(path);
        if (!file) {
            lastError_ = "Failed to open file for writing: " + path.string();
            return false;
        }

        // Write header
        file << "timestamp_us,mouse,deltaX,deltaY,velocity\n";

        // Calculate microseconds per tick
        double usPerTick = 1000000.0 / session.qpcFrequency;

        // Write Mouse A events
        for (const auto& e : session.eventsA) {
            double us = (e.timestamp - session.startTimestamp) * usPerTick;
            double velocity = std::sqrt(static_cast<double>(e.deltaX * e.deltaX + e.deltaY * e.deltaY));
            file << std::fixed << std::setprecision(2)
                 << us << ",A,"
                 << e.deltaX << ","
                 << e.deltaY << ","
                 << velocity << "\n";
        }

        // Write Mouse B events
        for (const auto& e : session.eventsB) {
            double us = (e.timestamp - session.startTimestamp) * usPerTick;
            double velocity = std::sqrt(static_cast<double>(e.deltaX * e.deltaX + e.deltaY * e.deltaY));
            file << std::fixed << std::setprecision(2)
                 << us << ",B,"
                 << e.deltaX << ","
                 << e.deltaY << ","
                 << velocity << "\n";
        }

        return true;
    }
    catch (const std::exception& e) {
        lastError_ = std::string("CSV export error: ") + e.what();
        return false;
    }
}

} // namespace RLA
