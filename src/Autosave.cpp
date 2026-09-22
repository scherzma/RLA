#include "Autosave.h"
#include <atomic>
#include <format>

namespace RLA {
Autosave::Autosave(std::filesystem::path directory) : directory_(std::move(directory)), worker_(&Autosave::Run, this) {}
Autosave::~Autosave() {
    { std::lock_guard lock(mutex_); closing_ = true; }
    ready_.notify_one();
    worker_.join(); // Finish pending saves before normal shutdown.
}
void Autosave::Save(RecordingSession session, std::wstring nameA, std::wstring nameB) {
    if (session.eventsA.empty() && session.eventsB.empty()) return;
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({false, std::move(session), std::move(nameA), std::move(nameB)});
        status_.busy = true;
        status_.message = "Saving recording...";
    }
    ready_.notify_one();
}
bool Autosave::Clear() {
    {
        std::lock_guard lock(mutex_);
        if (status_.busy) return false;
        jobs_.push_back({true});
        status_.busy = true;
        status_.message = "Clearing autosaves...";
    }
    ready_.notify_one();
    return true;
}
Autosave::Status Autosave::GetStatus() const { std::lock_guard lock(mutex_); return status_; }

std::filesystem::path Autosave::SaveFile(const Job& job) {
    std::filesystem::create_directories(directory_);
    SYSTEMTIME now{}; GetSystemTime(&now);
    static std::atomic<unsigned long long> sequence{0};
    std::filesystem::path path;
    do {
        path = directory_ / std::format(L"RLA-autosave-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}-{}-{}.json",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            GetCurrentProcessId(), sequence++);
    } while (std::filesystem::exists(path));
    auto temporary = path; temporary += L".part";
    try {
        DataStore store;
        if (!store.SaveToJson(temporary, job.session, {}, job.nameA, job.nameB))
            throw std::runtime_error(store.GetLastError());
        // Publish only a complete JSON file. Windows rename will not replace an existing file.
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        throw;
    }
    return path;
}
size_t Autosave::ClearFiles() {
    size_t removed = 0;
    if (!std::filesystem::exists(directory_)) return removed;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        const auto filename = entry.path().filename().wstring();
        // No recursion, directory removal, or following links. Preserve manual saves.
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        if (filename.starts_with(L"RLA-autosave-") && entry.path().extension() == L".json")
            removed += std::filesystem::remove(entry.path());
    }
    return removed;
}
void Autosave::Run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [&] { return closing_ || !jobs_.empty(); });
            if (jobs_.empty()) return;
            job = std::move(jobs_.front()); jobs_.pop_front();
        }
        std::string message;
        std::filesystem::path saved;
        bool cleared = false;
        try {
            if (job.clear) { const auto count = ClearFiles(); cleared = true; message = std::format("Cleared {} autosaves.", count); }
            else { saved = SaveFile(job); message = "Recording saved automatically."; }
        } catch (const std::exception& error) {
            message = std::string(job.clear ? "Clear failed: " : "Autosave failed: ") + error.what();
        }
        std::lock_guard lock(mutex_);
        if (!saved.empty()) status_.lastFile = std::move(saved);
        if (cleared) status_.lastFile.clear();
        status_.message = std::move(message);
        status_.busy = !jobs_.empty();
    }
}
}
