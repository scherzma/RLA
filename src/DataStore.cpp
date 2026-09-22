#include "DataStore.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace RLA {

using json = nlohmann::json;

// JSON numeric conversions do not check the target integer range.
template<typename T>
static T ReadInteger(const json& value) {
    if (value.is_number_unsigned()) {
        if (value.get<uint64_t>() <= static_cast<uint64_t>((std::numeric_limits<T>::max)())) {
            return value.get<T>();
        }
    } else if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if constexpr (std::is_unsigned_v<T>) {
            if (number >= 0 && static_cast<uint64_t>(number) <= (std::numeric_limits<T>::max)())
                return static_cast<T>(number);
        } else if (number >= (std::numeric_limits<T>::min)() && number <= (std::numeric_limits<T>::max)()) {
            return static_cast<T>(number);
        }
    }
    throw std::runtime_error("Expected an integer within the supported range");
}

static void ValidateSession(const RecordingSession& session) {
    if (!std::isfinite(session.qpcFrequency) || session.qpcFrequency < 1.0 ||
        session.qpcFrequency >= std::ldexp(1.0, 63) ||
        std::floor(session.qpcFrequency) != session.qpcFrequency) {
        throw std::runtime_error("Invalid QPC frequency");
    }
    if (session.startTimestamp < 0 || session.endTimestamp < session.startTimestamp) {
        throw std::runtime_error("Invalid recording time range");
    }
    for (const auto* events : { &session.eventsA, &session.eventsB }) {
        int64_t previous = session.startTimestamp;
        for (const auto& event : *events) {
            if (event.timestamp < previous || event.timestamp > session.endTimestamp) {
                throw std::runtime_error("Events must be ordered and within the recording time range");
            }
            previous = event.timestamp;
        }
    }
}

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
    gmtime_s(&tm, &time);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

bool DataStore::SaveToJson(const std::filesystem::path& path,
                           const RecordingSession& session,
                           const AnalysisResult& result,
                           const std::wstring& mouseAName,
                           const std::wstring& mouseBName) {
    lastError_.clear();
    try {
        ValidateSession(session);
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

        if (session.capture.available) {
            const auto& c = session.capture;
            j["capture"] = {{"method", "buffered"}, {"scope", "application"},
                {"timestampMode", "batch-read-qpc"}, {"packets", c.packets},
                {"groupedPackets", c.groupedPackets}, {"maxBatch", c.maxBatch},
                {"readErrors", c.readErrors}, {"droppedEvents", c.droppedEvents}, {"lastError", c.lastError}};
        }

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

        file.exceptions(std::ios::failbit | std::ios::badbit);
        file << std::setw(2) << j;
        file.close();
        return true;
    }
    catch (const std::exception& e) {
        lastError_ = std::string("JSON save error: ") + e.what();
        return false;
    }
}

std::optional<RecordingSession> DataStore::LoadFromJson(const std::filesystem::path& path) {
    lastError_.clear();
    try {
        std::ifstream file(path);
        if (!file) {
            lastError_ = "Failed to open file for reading: " + path.string();
            return std::nullopt;
        }

        json j = json::parse(file);

        if (ReadInteger<int>(j.at("version")) != 1) {
            throw std::runtime_error("Unsupported session version");
        }
        RecordingSession session;
        const auto& metadata = j.at("session");
        session.startTimestamp = ReadInteger<int64_t>(metadata.at("startTimestamp"));
        session.endTimestamp = ReadInteger<int64_t>(metadata.at("endTimestamp"));
        session.qpcFrequency = metadata.at("qpcFrequency").get<double>();
        if (j.contains("capture")) {
            const auto& c = j.at("capture");
            if (c.at("method") != "buffered" || c.at("scope") != "application" ||
                c.at("timestampMode") != "batch-read-qpc") throw std::runtime_error("Unsupported capture metadata");
            session.capture = {true, ReadInteger<uint64_t>(c.at("packets")),
                ReadInteger<uint64_t>(c.at("groupedPackets")), ReadInteger<uint64_t>(c.at("maxBatch")),
                ReadInteger<uint64_t>(c.at("readErrors")), ReadInteger<uint64_t>(c.at("droppedEvents")),
                ReadInteger<uint32_t>(c.at("lastError"))};
        }
        auto readEvents = [](const json& source, std::vector<MouseEvent>& events) {
            if (!source.is_array()) throw std::runtime_error("Expected an event array");
            events.reserve(source.size());
            for (const auto& e : source) {
                events.push_back({ nullptr, ReadInteger<int64_t>(e.at("t")),
                    ReadInteger<int32_t>(e.at("dx")), ReadInteger<int32_t>(e.at("dy")) });
            }
        };
        readEvents(j.at("eventsA"), session.eventsA);
        readEvents(j.at("eventsB"), session.eventsB);
        ValidateSession(session);
        return session;
    }
    catch (const std::exception& e) {
        lastError_ = std::string("JSON load error: ") + e.what();
        return std::nullopt;
    }
}

bool DataStore::ExportToCsv(const std::filesystem::path& path,
                            const RecordingSession& session) {
    lastError_.clear();
    try {
        ValidateSession(session);
        std::ofstream file(path);
        if (!file) {
            lastError_ = "Failed to open file for writing: " + path.string();
            return false;
        }

        file.exceptions(std::ios::failbit | std::ios::badbit);
        // Write header
        file << "timestamp_us,mouse,deltaX,deltaY,velocity\n";

        // Calculate microseconds per tick
        double usPerTick = 1000000.0 / session.qpcFrequency;

        // Write Mouse A events
        for (const auto& e : session.eventsA) {
            double us = (e.timestamp - session.startTimestamp) * usPerTick;
            double velocity = std::hypot(static_cast<double>(e.deltaX), static_cast<double>(e.deltaY));
            file << std::fixed << std::setprecision(2)
                 << us << ",A,"
                 << e.deltaX << ","
                 << e.deltaY << ","
                 << velocity << "\n";
        }

        // Write Mouse B events
        for (const auto& e : session.eventsB) {
            double us = (e.timestamp - session.startTimestamp) * usPerTick;
            double velocity = std::hypot(static_cast<double>(e.deltaX), static_cast<double>(e.deltaY));
            file << std::fixed << std::setprecision(2)
                 << us << ",B,"
                 << e.deltaX << ","
                 << e.deltaY << ","
                 << velocity << "\n";
        }

        file.close();
        return true;
    }
    catch (const std::exception& e) {
        lastError_ = std::string("CSV export error: ") + e.what();
        return false;
    }
}

} // namespace RLA
