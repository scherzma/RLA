// Independent diagnostic. No RLA capture, analysis, or rendering code is linked.
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>

namespace {
constexpr int binCount = 1800; // 10 ms bins, 18 seconds
struct Bin { unsigned reports=0, movement=0, noCoalesce=0; };
struct Counts {
    std::array<Bin, binCount> bins{};
    void add(double ms, bool moving, bool uncombined) {
        if (ms < 0 || ms >= 18000) return;
        auto& b = bins[static_cast<size_t>(ms / 10)];
        ++b.reports; b.movement += moving; b.noCoalesce += uncombined;
    }
    double rate(int begin, int end) const {
        unsigned sum=0;
        for (int i=begin/10; i<end/10; ++i) sum += bins[i].movement;
        return sum * 1000.0 / (end-begin);
    }
};
HWND window, buttons[3];
HANDLE devices[2]{};
std::wstring names[2];
Counts counts[2];
LONGLONG frequency=0, started=0, armAfter=0, lastPaint=0;
int assigning=-1;
bool recording=false;
unsigned readErrors=0, otherReports=0, absoluteReports=0;
std::wstring status=L"Close RLA first. Assign A, then B. Move only the mouse being assigned.";
LONGLONG now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double elapsed() { return (now()-started)*1000.0/frequency; }
bool registration(bool raw) {
    RAWINPUTDEVICE d{1,2,raw ? DWORD(RIDEV_NOLEGACY) : DWORD(0),window};
    return RegisterRawInputDevices(&d,1,sizeof(d)) != FALSE;
}
std::wstring deviceName(HANDLE h) {
    UINT n=0;
    if (GetRawInputDeviceInfoW(h,RIDI_DEVICENAME,nullptr,&n)==UINT(-1)) return L"Unknown device";
    std::wstring s(n,L'\0');
    if (GetRawInputDeviceInfoW(h,RIDI_DEVICENAME,s.data(),&n)==UINT(-1)) return L"Unknown device";
    while (!s.empty() && !s.back()) s.pop_back();
    return s;
}
std::string utf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
    std::string result(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),result.data(),n,nullptr,nullptr);
    return result;
}
void save(bool complete, double duration) {
    using nlohmann::json;
    json j={{"version",1},{"recorder","Independent WM_INPUT / GetRawInputData probe"},
        {"complete",complete},{"durationMs",duration},{"binMs",10},
        {"readErrors",readErrors},{"otherDeviceReports",otherReports},
        {"absoluteReportsIgnored",absoluteReports},{"qpcFrequency",frequency},
        {"timestampScope","application receipt; not USB polling"}};
    constexpr int begins[]={4000,9000,14000}, ends[]={7500,12500,17500};
    for (int d=0;d<2;++d) {
        json bins=json::array();
        for (const auto& b:counts[d].bins) bins.push_back({b.reports,b.movement,b.noCoalesce});
        j[d==0?"A":"B"]={{"device",utf8(names[d])},{"bins",std::move(bins)}};
    }
    j["binColumns"]={"reports","movementReports","noCoalesceFlagReports"};
    std::wostringstream summary;
    summary.precision(0); summary<<std::fixed;
    for (int i=0;i<3;++i) {
        double a=counts[0].rate(begins[i],ends[i]), b=counts[1].rate(begins[i],ends[i]);
        j["stages"].push_back({{"beginMs",begins[i]},{"endMs",ends[i]},
            {"aMovementHz",a},{"bMovementHz",b},{"totalMovementHz",a+b}});
        summary<<(i==1?L"Both":L"A only")<<L": A "<<a<<L", B "<<b<<L", total "<<a+b<<L" events/s\n";
    }
    wchar_t local[32768];
    DWORD len=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    if (!len || len>=32768) throw std::runtime_error("LOCALAPPDATA unavailable");
    auto dir=std::filesystem::path(local)/L"RLA"/L"RawInputProbe";
    std::filesystem::create_directories(dir);
    SYSTEMTIME t; GetSystemTime(&t);
    wchar_t name[120];
    swprintf_s(name,L"probe-%04u%02u%02u-%02u%02u%02u-%03u-%lu.json",t.wYear,t.wMonth,t.wDay,
        t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
    auto path=dir/name, temp=path; temp+=L".tmp";
    std::ofstream file(temp,std::ios::binary);
    file.exceptions(std::ios::failbit|std::ios::badbit);
    file<<j.dump(); file.close(); std::filesystem::rename(temp,path);
    status=(complete?L"Complete.\n":L"Stopped early. Do not compare these stage results.\n")+
        summary.str()+L"Read errors: "+std::to_wstring(readErrors)+L"\nSaved: "+path.wstring();
}
void stop(bool complete) {
    if (!recording) return;
    double duration=elapsed(); recording=false;
    ClipCursor(nullptr); registration(false);
    for (auto b:buttons) EnableWindow(b,TRUE);
    try { save(complete,duration); }
    catch (const std::exception& e) { status=L"Save failed: "+std::wstring(e.what(),e.what()+strlen(e.what())); }
    InvalidateRect(window,nullptr,TRUE);
}
void start() {
    if (!devices[0] || !devices[1] || devices[0]==devices[1]) {
        status=L"Assign two different mice first."; return;
    }
    assigning=-1;
    counts[0]={}; counts[1]={}; readErrors=otherReports=absoluteReports=0;
    if (!registration(true)) { status=L"Raw Input registration failed."; return; }
    RECT r; GetClientRect(window,&r);
    POINT p{r.left,r.top}, q{r.right,r.bottom}; ClientToScreen(window,&p); ClientToScreen(window,&q);
    r={p.x,p.y,q.x,q.y};
    if (!ClipCursor(&r)) { registration(false); status=L"Cannot confine cursor. Test not started."; return; }
    started=now(); lastPaint=0; recording=true;
    for (auto b:buttons) EnableWindow(b,FALSE);
}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    switch(msg) {
    case WM_COMMAND:
        if (!recording) {
            if (LOWORD(w)==3) start();
            else if (LOWORD(w)==1 || LOWORD(w)==2) {
                assigning=LOWORD(w)-1; armAfter=now()+frequency/2;
                status=L"Wait half a second, then move only mouse "+std::wstring(assigning?L"B":L"A")+L".";
            }
            InvalidateRect(h,nullptr,TRUE);
        } return 0;
    case WM_INPUT: {
        const auto t=now();
        RAWINPUT input{}; UINT size=sizeof(input);
        UINT n=GetRawInputData(reinterpret_cast<HRAWINPUT>(l),RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER));
        if (n==UINT(-1) || n<sizeof(RAWINPUTHEADER)) { if(recording) ++readErrors; }
        else if (input.header.dwType==RIM_TYPEMOUSE && n>=offsetof(RAWINPUT,data)+sizeof(RAWMOUSE)) {
            const auto& m=input.data.mouse;
            bool moving=m.lLastX!=0 || m.lLastY!=0;
            if (!recording && assigning>=0 && t>=armAfter && moving && input.header.hDevice) {
                if (input.header.hDevice==devices[1-assigning]) status=L"That mouse is already assigned. Move the other mouse.";
                else {
                    devices[assigning]=input.header.hDevice; names[assigning]=deviceName(input.header.hDevice);
                    assigning=-1; status=L"Mouse assigned. Assign the other mouse, or start the test.";
                }
                InvalidateRect(h,nullptr,TRUE);
            }
            if (recording) {
                if (m.usFlags&MOUSE_MOVE_ABSOLUTE) ++absoluteReports;
                else {
                    int d=input.header.hDevice==devices[0]?0:input.header.hDevice==devices[1]?1:-1;
                    if (d>=0) counts[d].add((t-started)*1000.0/frequency,moving,(m.usFlags&MOUSE_MOVE_NOCOALESCE)!=0);
                    else ++otherReports;
                }
            }
        }
        return DefWindowProcW(h,msg,w,l); // Required foreground Raw Input cleanup.
    }
    case WM_KEYDOWN: if(w==VK_ESCAPE) stop(false); return 0;
    case WM_ACTIVATE: if(LOWORD(w)==WA_INACTIVE) stop(false); return 0;
    case WM_TIMER:
        if(recording && elapsed()>=18000) stop(true);
        // Paint instructions even when continuous input delays WM_PAINT/WM_TIMER.
        if(recording && now()-lastPaint>=frequency/10) {
            lastPaint=now();
            RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
        }
        InvalidateRect(h,nullptr,TRUE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint; auto dc=BeginPaint(h,&paint);
        SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT)); SetBkMode(dc,TRANSPARENT);
        std::wstring text=L"Independent Raw Input test - no RLA code, graphs, or USB tracing\n\n";
        text+=L"A: "+(devices[0]?names[0]:L"Not assigned")+L"\nB: "+(devices[1]?names[1]:L"Not assigned")+L"\n\n";
        if(recording) {
            double s=elapsed()/1000;
            text+=s<3?L"GET READY":s<8?L"MOVE A ONLY":s<13?L"MOVE BOTH MICE":L"MOVE A ONLY";
            text+=L"\n"+std::to_wstring(int(std::ceil(18-s)))+L" seconds left. Keep moving. Esc stops early.";
        } else text+=status;
        RECT r; GetClientRect(h,&r); r.left+=16; r.right-=16; r.top=70;
        DrawTextW(dc,text.c_str(),-1,&r,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
        EndPaint(h,&paint); return 0;
    }
    case WM_CLOSE: stop(false); DestroyWindow(h); return 0;
    case WM_DESTROY: ClipCursor(nullptr); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,msg,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR args,int show) {
    if (std::wstring(args)==L"--self-test") {
        Counts c; c.add(-1,true,true); c.add(18000,true,true);
        for(int i=4000;i<7500;++i) { c.add(i,true,true); c.add(i,false,false); }
        c.add(7500,true,false);
        return c.rate(4000,7500)==1000 && c.bins[400].reports==20 &&
            c.bins[400].noCoalesce==10 && c.bins[1799].reports==0 ? 0:1;
    }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); frequency=f.QuadPart;
    WNDCLASSW cls{}; cls.lpfnWndProc=proc; cls.hInstance=instance;
    cls.lpszClassName=L"IndependentRawInputProbe"; cls.hCursor=LoadCursor(nullptr,IDC_ARROW);
    cls.hbrBackground=GetSysColorBrush(COLOR_WINDOW); RegisterClassW(&cls);
    window=CreateWindowW(cls.lpszClassName,L"Independent mouse input test",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,920,510,nullptr,nullptr,instance,nullptr);
    if(!window) return 1;
    for(int i=0;i<3;++i) buttons[i]=CreateWindowW(L"BUTTON",i==0?L"Assign A":i==1?L"Assign B":L"Start 18-second test",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,16+i*200,16,190,32,window,reinterpret_cast<HMENU>(INT_PTR(i+1)),instance,nullptr);
    if(!registration(false)) { MessageBoxW(window,L"Raw Input registration failed.",L"Error",MB_ICONERROR); return 1; }
    SetTimer(window,1,100,nullptr); ShowWindow(window,show);
    MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
        // WM_TIMER has lower priority than input. Enforce the deadline even under continuous input.
        if(recording && elapsed()>=18000) stop(true);
    }
    return 0;
}
