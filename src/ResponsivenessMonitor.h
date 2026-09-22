#pragma once
#include <Windows.h>
#include <ShlObj.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

namespace RLA {
// Records the last main-loop phase only if the window also fails a bounded
// message check. Modal file dialogs therefore do not count as application hangs.
class ResponsivenessMonitor {
public:
    explicit ResponsivenessMonitor(HWND window) : window_(window) {
        wchar_t local[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,local))) return;
        directory_=std::filesystem::path(local)/L"RLA"/L"Diagnostics";
        stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!stop_) return;
        Checkpoint("starting");
        try { worker_=std::thread([this] { Run(); }); } catch (...) { CloseHandle(stop_); stop_=nullptr; }
    }
    ~ResponsivenessMonitor() {
        if (stop_) { SetEvent(stop_); worker_.join(); CloseHandle(stop_); }
    }
    void Checkpoint(const char* phase) {
        phase_.store(phase,std::memory_order_relaxed); heartbeat_.store(GetTickCount64(),std::memory_order_release);
    }
private:
    void Run() {
        ULONGLONG reported=0;
        while (WaitForSingleObject(stop_,1000)==WAIT_TIMEOUT) {
            const auto beat=heartbeat_.load(std::memory_order_acquire);
            if (GetTickCount64()-beat<5000 || beat==reported) continue;
            DWORD_PTR reply=0;
            if (SendMessageTimeoutW(window_,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,200,&reply)) continue;
            reported=beat;
            try {
                std::filesystem::create_directories(directory_);
                std::ofstream output(directory_/(L"hang-"+std::to_wstring(GetCurrentProcessId())+L".txt"),std::ios::app);
                SYSTEMTIME now{}; GetSystemTime(&now);
                output << now.wYear << '-' << now.wMonth << '-' << now.wDay << ' ' << now.wHour << ':' << now.wMinute << ':' << now.wSecond
                    << " UTC; UI stalled in " << phase_.load(std::memory_order_relaxed) << " for " << GetTickCount64()-beat << " ms\n";
            } catch (...) {} // A diagnostic failure must never stop the UI.
        }
    }
    HWND window_;
    HANDLE stop_=nullptr;
    std::filesystem::path directory_;
    std::atomic<ULONGLONG> heartbeat_{0};
    std::atomic<const char*> phase_{"starting"};
    std::thread worker_;
};
}
