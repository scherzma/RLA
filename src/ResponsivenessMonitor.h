#pragma once
#include <Windows.h>
#include <ShlObj.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

namespace RLA {
// A nested Windows/DXGI loop can answer messages while the frame loop is stuck.
// Record stalled frame progress regardless of the result of a message probe.
class ResponsivenessMonitor {
public:
    explicit ResponsivenessMonitor(HWND window, std::filesystem::path directory = {}, DWORD thresholdMs = 5000)
        : window_(window), thresholdMs_(thresholdMs) {
        wchar_t local[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,local))) return;
        directory_=directory.empty() ? std::filesystem::path(local)/L"RLA"/L"Diagnostics" : std::move(directory);
        Write("monitor started",0,"not checked");
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
            if (GetTickCount64()-beat<thresholdMs_ || beat==reported) continue;
            // Write before probing Windows, so that even a stuck probe cannot
            // hide the phase that stopped progressing.
            Write(phase_.load(std::memory_order_relaxed),GetTickCount64()-beat,"not checked");
            DWORD_PTR reply=0;
            if (SendMessageTimeoutW(window_,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,200,&reply))
                Write("window answers messages but frame progress stopped",GetTickCount64()-beat,"yes");
            reported=beat;
        }
    }
    void Write(const char* phase, ULONGLONG elapsed, const char* responds) {
        try {
            std::filesystem::create_directories(directory_);
            std::ofstream output(directory_/(L"hang-"+std::to_wstring(GetCurrentProcessId())+L".txt"),std::ios::app);
            SYSTEMTIME now{}; GetSystemTime(&now);
            output << now.wYear << '-' << now.wMonth << '-' << now.wDay << ' ' << now.wHour << ':' << now.wMinute << ':' << now.wSecond
                << " UTC; phase=" << phase << "; elapsed=" << elapsed << " ms; message response=" << responds << "\n";
        } catch (...) {} // A diagnostic failure must never stop the UI.
    }
    HWND window_;
    DWORD thresholdMs_;
    HANDLE stop_=nullptr;
    std::filesystem::path directory_;
    std::atomic<ULONGLONG> heartbeat_{0};
    std::atomic<const char*> phase_{"starting"};
    std::thread worker_;
};
}
