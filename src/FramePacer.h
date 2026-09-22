#pragma once
#include <Windows.h>
#include <chrono>
#include <algorithm>

namespace RLA {
// This timer belongs only to the UI thread. It does not change system timer
// resolution or add a wait to the capture thread.
class FramePacer {
public:
    FramePacer() {
        timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        if (!timer_) timer_=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    }
    ~FramePacer() { if (timer_) CloseHandle(timer_); }
    FramePacer(const FramePacer&)=delete;
    FramePacer& operator=(const FramePacer&)=delete;
    bool Ready() const { return Clock::now()>=next_; }
    void BeginFrame() { next_=Clock::now()+std::chrono::microseconds(8333); }
    void Wait() {
        const auto remaining=next_-Clock::now();
        if (remaining<=Clock::duration::zero()) return;
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
        LARGE_INTEGER due{}; due.QuadPart=-(std::max)(1LL,ns/100);
        if (timer_ && SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE))
            MsgWaitForMultipleObjectsEx(1,&timer_,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        else
            MsgWaitForMultipleObjectsEx(0,nullptr,static_cast<DWORD>((ns+999999)/1000000),QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
private:
    using Clock=std::chrono::steady_clock;
    Clock::time_point next_{};
    HANDLE timer_=nullptr;
};
}
