#pragma once
#include "DataStore.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace RLA {
// One worker owns all archive I/O. Recording and rendering never wait for disk.
class Autosave {
public:
    struct Status { bool busy = false; std::string message; std::filesystem::path lastFile; };
    explicit Autosave(std::filesystem::path directory);
    ~Autosave();
    void Save(RecordingSession session, std::wstring nameA, std::wstring nameB);
    bool Clear(); // Only accepted when no save is pending.
    Status GetStatus() const;
    const std::filesystem::path& Directory() const { return directory_; }
private:
    struct Job { bool clear = false; RecordingSession session; std::wstring nameA, nameB; };
    void Run();
    std::filesystem::path SaveFile(const Job& job);
    size_t ClearFiles();
    const std::filesystem::path directory_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    Status status_;
    bool closing_ = false;
    std::thread worker_;
};
}
