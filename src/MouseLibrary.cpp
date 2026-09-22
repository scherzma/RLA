#include "MouseLibrary.h"
#include <nlohmann/json.hpp>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <format>
#include <numeric>
#include <set>
#pragma comment(lib, "bcrypt.lib")

namespace RLA {
namespace {
using json = nlohmann::json;
std::string Hex(const unsigned char* data, size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result; result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) { result += digits[data[i] >> 4]; result += digits[data[i] & 15]; }
    return result;
}
std::string NewId() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Could not create a profile ID.");
    return Hex(bytes.data(), bytes.size());
}
std::string CleanName(std::string name, bool required = true) {
    const auto begin = name.find_first_not_of(" \t\r\n");
    name = begin == std::string::npos ? "" : name.substr(begin, name.find_last_not_of(" \t\r\n") - begin + 1);
    if ((required && name.empty()) || name.size() > 160 || name.find_first_of("\r\n\t") != std::string::npos || name.find('\0') != std::string::npos)
        throw std::runtime_error("Use a name from 1 to 160 bytes without control characters.");
    return name;
}
void CheckSetup(const std::string& connection, int hz) {
    if ((connection != "Wired" && connection != "Wireless" && connection != "Bluetooth" && connection != "Other") || hz < 0 || hz > 32000)
        throw std::runtime_error("Invalid connection or polling rate.");
}
double Median(std::vector<double> values) {
    auto middle = values.begin() + values.size()/2;
    std::nth_element(values.begin(), middle, values.end());
    return values.size()%2 ? *middle : (*middle + *std::max_element(values.begin(), middle)) * 0.5;
}
}

const SavedMouse* MouseLibrary::FindMouse(const std::string& id) const {
    for (const auto& mouse : mice_) if (mouse.id == id) return &mouse;
    return nullptr;
}
const MouseSetup* MouseLibrary::FindSetup(const std::string& id) const {
    for (const auto& setup : setups_) if (setup.id == id) return &setup;
    return nullptr;
}
std::string MouseLibrary::AddMouse(std::string name) {
    name = CleanName(std::move(name));
    const auto id = NewId(); mice_.push_back({id, std::move(name)}); return id;
}
void MouseLibrary::RenameMouse(const std::string& id, std::string name) {
    name = CleanName(std::move(name));
    for (auto& mouse : mice_) if (mouse.id == id) { mouse.name = std::move(name); return; }
    throw std::runtime_error("Mouse profile not found.");
}
std::string MouseLibrary::AddSetup(const std::string& mouseId, std::string connection, int hz, std::string label) {
    if (!FindMouse(mouseId)) throw std::runtime_error("Select a mouse profile first.");
    CheckSetup(connection, hz); label = CleanName(std::move(label), false);
    for (const auto& setup : setups_) if (setup.mouseId == mouseId && setup.connection == connection && setup.pollingHz == hz && setup.label == label)
        return setup.id;
    const auto id = NewId(); setups_.push_back({id, mouseId, std::move(connection), std::move(label), hz}); return id;
}
void MouseLibrary::LinkSetup(const std::string& id, const std::string& mouseId, std::string label) {
    if (!FindMouse(mouseId)) throw std::runtime_error("Mouse profile not found.");
    label = CleanName(std::move(label), false);
    for (auto& setup : setups_) if (setup.id == id) { setup.mouseId = mouseId; setup.label = std::move(label); return; }
    throw std::runtime_error("Mouse setup not found.");
}
void MouseLibrary::Bind(const std::string& devicePath, const std::string& setupId) {
    if (devicePath.empty()) throw std::runtime_error("Assign a connected mouse first.");
    if (setupId.empty()) { bindings_.erase(devicePath); return; }
    if (!FindSetup(setupId)) throw std::runtime_error("Mouse setup not found.");
    bindings_[devicePath] = setupId;
}
std::string MouseLibrary::BoundSetup(const std::string& path) const {
    const auto it = bindings_.find(path); return it == bindings_.end() ? "" : it->second;
}
std::string MouseLibrary::Label(const std::string& id) const {
    const auto* setup = FindSetup(id);
    if (!setup) return id.empty() ? "Not linked" : "Unknown setup";
    const auto* mouse = FindMouse(setup->mouseId);
    return (mouse ? mouse->name : "Unknown mouse") + " / " + setup->connection +
        (setup->pollingHz ? std::format(" / {} Hz", setup->pollingHz) : " / rate unspecified") +
        (setup->label.empty() ? "" : " / " + setup->label);
}
RecordingMouse MouseLibrary::Identity(const std::string& id, std::string devicePath) const {
    const auto* setup = FindSetup(id);
    if (!setup) { RecordingMouse empty; empty.devicePath = std::move(devicePath); return empty; }
    const auto* mouse = FindMouse(setup->mouseId);
    return {setup->mouseId, setup->id, mouse ? mouse->name : "Unknown", setup->connection, setup->label, std::move(devicePath), setup->pollingHz};
}
void MouseLibrary::ImportIdentity(const RecordingMouse& identity) {
    if (identity.setupId.empty()) return;
    CheckSetup(identity.connection, identity.pollingHz);
    const auto name = CleanName(identity.name), label = CleanName(identity.label, false);
    if (identity.mouseId.empty() || identity.setupId.size() > 128 || identity.mouseId.size() > 128) throw std::runtime_error("Invalid saved mouse identity.");
    if (const auto* setup = FindSetup(identity.setupId)) {
        // A known ID can have a renamed/relinked parent, but its measurement settings are fixed.
        if (setup->connection != identity.connection || setup->pollingHz != identity.pollingHz)
            throw std::runtime_error("Saved mouse setup conflicts with the library.");
        return;
    }
    if (!FindMouse(identity.mouseId)) mice_.push_back({identity.mouseId, name});
    setups_.push_back({identity.setupId, identity.mouseId, identity.connection, label, identity.pollingHz});
}
std::string MouseLibrary::DeviceKey(const std::wstring& path) {
    std::wstring value = path.substr(0, path.find(L'\0'));
    for (auto& c : value) c = static_cast<wchar_t>(std::towlower(c));
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::string MouseLibrary::RecordingKey(const RecordingSession& session) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    auto checked = [](NTSTATUS status) { if (status < 0) throw std::runtime_error("Could not identify the recording."); };
    try {
        checked(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        checked(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
        auto add = [&](const void* data, size_t size) { checked(BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0)); };
        add(&session.startTimestamp, sizeof(session.startTimestamp)); add(&session.endTimestamp, sizeof(session.endTimestamp));
        add(&session.qpcFrequency, sizeof(session.qpcFrequency));
        struct Event { int64_t timestamp; int32_t x, y; };
        static_assert(sizeof(Event) == 16);
        std::array<Event, 1024> block;
        for (const auto* events : {&session.eventsA, &session.eventsB}) {
            const uint64_t count = events->size(); add(&count, sizeof(count));
            for (size_t i = 0; i < events->size();) {
                size_t n = 0;
                while (n < block.size() && i < events->size()) { const auto& e = (*events)[i++]; block[n++] = {e.timestamp, e.deltaX, e.deltaY}; }
                add(block.data(), n * sizeof(Event));
            }
        }
        std::array<unsigned char, 32> digest{};
        checked(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
        BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
        return Hex(digest.data(), digest.size());
    } catch (...) { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
}
bool MouseLibrary::AddComparison(const RecordingSession& session, const LatencyFit& fit, int method) {
    if (session.captureTestMode) return false;
    const auto* a=FindSetup(session.mouseA.setupId); const auto* b=FindSetup(session.mouseB.setupId);
    if (!fit.valid || fit.matchedCycles < 3 || !std::isfinite(fit.differenceMs) || !std::isfinite(fit.spreadMs) ||
        !std::isfinite(fit.binMs) || fit.spreadMs < 0 || fit.binMs <= 0 || method < 0 || method > 2 ||
        !a || !b || a->pollingHz==0 || b->pollingHz==0 || session.mouseA.setupId == session.mouseB.setupId || session.eventsA.empty() || session.eventsB.empty() ||
        a->connection!=session.mouseA.connection || b->connection!=session.mouseB.connection ||
        a->pollingHz!=session.mouseA.pollingHz || b->pollingHz!=session.mouseB.pollingHz) return false;
    SYSTEMTIME now{}; GetSystemTime(&now);
    SavedComparison result{RecordingKey(session), session.mouseA.setupId, session.mouseB.setupId,
        std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond),
        method, fit.differenceMs, fit.spreadMs, fit.binMs, fit.matchedCycles};
    for (auto& old : comparisons_) if (old.recordingKey == result.recordingKey && old.method == method) {
        result.enabled = old.enabled; old = std::move(result); return true;
    }
    comparisons_.push_back(std::move(result)); return true;
}
int MouseLibrary::DetectRate(const std::vector<MouseEvent>& events, double frequency) {
    if (events.size()<100 || !std::isfinite(frequency) || frequency<=0) return 0;
    // Find the highest supported rate in a sustained section. Pauses and slow
    // movement elsewhere must not pull the recording's detected rate down.
    int highest=0;
    size_t first=0;
    auto section=[&](size_t last) {
        if (last<=first) return;
        const double duration=(events[last].timestamp-events[first].timestamp)*1000.0/frequency;
        if (duration<100) return;
        size_t substantial=0;
        std::vector<double> intervals;
        intervals.reserve(last-first);
        for (size_t i=first+1;i<=last;++i) {
            const auto& event=events[i];
            if (std::abs(static_cast<int64_t>(event.deltaX))>=2 || std::abs(static_cast<int64_t>(event.deltaY))>=2) ++substantial;
            const double dt=(event.timestamp-events[i-1].timestamp)*1000.0/frequency;
            if (dt>0) intervals.push_back(dt);
        }
        const size_t count=last-first;
        if (substantial<count/4 || intervals.size()<count*0.85) return;
        const double rate=count*1000.0/duration;
        const double intervalRate=1000.0/Median(intervals);
        for (int candidate : {125,250,500,1000,2000,4000,8000}) {
            if (candidate>highest && std::abs(rate/candidate-1)<0.12 && std::abs(intervalRate/candidate-1)<0.2)
                highest=candidate;
        }
    };
    for (size_t i=1;i<events.size();++i) {
        if (events[i].timestamp<events[i-1].timestamp) return 0;
        if ((events[i].timestamp-events[first].timestamp)*1000.0/frequency>=250) {
            section(i); first=i;
        }
    }
    section(events.size()-1);
    return highest;
}

void MouseLibrary::DeleteComparison(const std::string& key, int method) {
    std::erase_if(comparisons_, [&](const auto& run) { return run.recordingKey==key && run.method==method; });
}
void MouseLibrary::DeleteSetup(const std::string& id) {
    // Remove dependent results and device links before removing their setup.
    std::erase_if(comparisons_, [&](const auto& run) { return run.setupA==id || run.setupB==id; });
    std::erase_if(bindings_, [&](const auto& binding) { return binding.second==id; });
    std::erase_if(setups_, [&](const auto& setup) { return setup.id==id; });
}
void MouseLibrary::DeleteMouse(const std::string& id) {
    std::vector<std::string> children;
    for (const auto& setup : setups_) if (setup.mouseId==id) children.push_back(setup.id);
    for (const auto& child : children) DeleteSetup(child);
    std::erase_if(mice_, [&](const auto& mouse) { return mouse.id==id; });
}

void MouseLibrary::SetEnabled(size_t index, bool enabled) {
    if (index >= comparisons_.size()) throw std::runtime_error("Comparison not found.");
    comparisons_[index].enabled = enabled;
}

std::vector<MouseRank> MouseLibrary::Ranking(int method) const {
    // Robust median per pair, then fit B-A constraints within each connected group.
    using Pair = std::pair<size_t, size_t>;
    std::map<std::string, size_t> indices;
    std::vector<MouseRank> rows;
    for (const auto& setup : setups_) { indices[setup.id] = rows.size(); rows.push_back({setup.id}); }
    std::map<Pair, std::vector<double>> differences;
    for (const auto& c : comparisons_) if (c.enabled && c.method == method) {
        const auto ai = indices.find(c.setupA), bi = indices.find(c.setupB);
        if (ai == indices.end() || bi == indices.end() || ai->second == bi->second) continue;
        const auto a = ai->second, b = bi->second;
        ++rows[a].recordings; ++rows[b].recordings;
        differences[{(std::min)(a,b),(std::max)(a,b)}].push_back(a < b ? c.differenceMs : -c.differenceMs);
    }
    struct Edge { size_t a,b; double delta,weight; };
    std::vector<Edge> edges;
    std::vector<std::vector<size_t>> adjacent(rows.size());
    for (const auto& [pair, values] : differences) {
        const auto i = edges.size(); edges.push_back({pair.first,pair.second,Median(values),static_cast<double>(values.size())});
        adjacent[pair.first].push_back(i); adjacent[pair.second].push_back(i);
    }
    int group = 0;
    for (size_t root = 0; root < rows.size(); ++root) {
        if (rows[root].group || adjacent[root].empty()) continue;
        ++group;
        std::vector<size_t> members{root}; rows[root].group = group;
        for (size_t i = 0; i < members.size(); ++i) for (size_t e : adjacent[members[i]]) {
            const auto other = edges[e].a == members[i] ? edges[e].b : edges[e].a;
            if (!rows[other].group) { rows[other].group = group; members.push_back(other); }
        }
        std::map<size_t,size_t> local;
        for (size_t i = 0; i < members.size(); ++i) local[members[i]] = i;
        std::vector<Edge> links;
        for (const auto& e : edges) if (rows[e.a].group == group) links.push_back({local[e.a],local[e.b],e.delta,e.weight});
        const size_t n = members.size();
        std::vector<double> x(n), diagonal(n), r(n), z(n), p(n), ap(n);
        for (const auto& e : links) { diagonal[e.a]+=e.weight; diagonal[e.b]+=e.weight; r[e.a]-=e.weight*e.delta; r[e.b]+=e.weight*e.delta; }
        r[0] = 0; // Fix one reference; only differences are observable.
        for (size_t i=1; i<n; ++i) p[i] = z[i] = r[i]/diagonal[i];
        double rz = std::inner_product(r.begin(),r.end(),z.begin(),0.0);
        const double tolerance = (std::max)(1e-20,rz*1e-20);
        for (size_t iteration=0; iteration<4*n && rz>tolerance; ++iteration) {
            std::fill(ap.begin(),ap.end(),0);
            for (const auto& e : links) { const double d=e.weight*(p[e.a]-p[e.b]); ap[e.a]+=d; ap[e.b]-=d; }
            ap[0]=0;
            const double denominator=std::inner_product(p.begin(),p.end(),ap.begin(),0.0);
            if (denominator <= 0) break;
            const double alpha=rz/denominator;
            for (size_t i=1;i<n;++i) { x[i]+=alpha*p[i]; r[i]-=alpha*ap[i]; z[i]=r[i]/diagonal[i]; }
            const double next=std::inner_product(r.begin(),r.end(),z.begin(),0.0), beta=next/rz;
            for (size_t i=1;i<n;++i) p[i]=z[i]+beta*p[i];
            rz=next;
        }
        if (!std::isfinite(rz) || rz>tolerance*100) throw std::runtime_error("Ranking could not converge.");
        double residual=0, weight=0;
        for (const auto& e : links) { const double d=x[e.b]-x[e.a]-e.delta; residual+=e.weight*d*d; weight+=e.weight; }
        residual=std::sqrt(residual/weight);
        const double fastest=*std::min_element(x.begin(),x.end());
        for (size_t i=0;i<n;++i) { rows[members[i]].relativeMs=x[i]-fastest; rows[members[i]].residualMs=residual; }
        std::sort(members.begin(),members.end(),[&](size_t a,size_t b){return rows[a].relativeMs<rows[b].relativeMs;});
        for (size_t i=0;i<n;++i) rows[members[i]].place=static_cast<int>(i+1);
    }
    std::stable_sort(rows.begin(),rows.end(),[](const MouseRank& a,const MouseRank& b) {
        if ((a.group==0)!=(b.group==0)) return a.group!=0;
        return a.group==b.group ? a.relativeMs<b.relativeMs : a.group<b.group;
    });
    return rows;
}

std::string MouseLibrary::Save(const std::filesystem::path& path) const {
    auto temp=path; temp+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
    try {
        json j={{"version",1},{"autoRank",autoRank}};
        j["mice"]=json::array(); j["setups"]=json::array(); j["comparisons"]=json::array();
        for (const auto& m : mice_) j["mice"].push_back({{"id",m.id},{"name",m.name}});
        for (const auto& s : setups_) j["setups"].push_back({{"id",s.id},{"mouseId",s.mouseId},{"connection",s.connection},{"pollingHz",s.pollingHz},{"label",s.label}});
        j["bindings"]=bindings_;
        for (const auto& c : comparisons_) j["comparisons"].push_back({{"recordingKey",c.recordingKey},{"setupA",c.setupA},{"setupB",c.setupB},
            {"savedAt",c.savedAt},{"method",c.method},{"differenceMs",c.differenceMs},{"spreadMs",c.spreadMs},{"binMs",c.binMs},{"cycles",c.cycles},{"enabled",c.enabled}});
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(temp); output.exceptions(std::ios::failbit|std::ios::badbit); output << j.dump(2); output.close();
        if (!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Could not replace the mouse library file.");
        return {};
    } catch (const std::exception& e) { std::error_code ignored; std::filesystem::remove(temp,ignored); return e.what(); }
}
bool MouseLibrary::Load(const std::filesystem::path& path, std::string& error) {
    error.clear();
    try {
        if (!std::filesystem::exists(path)) return true;
        std::ifstream input(path); const auto j=json::parse(input);
        if (j.at("version")!=1) throw std::runtime_error("Unsupported mouse library version.");
        if (!j.at("mice").is_array() || !j.at("setups").is_array() || !j.at("comparisons").is_array() || !j.at("bindings").is_object())
            throw std::runtime_error("Invalid mouse library structure.");
        MouseLibrary loaded; loaded.autoRank=j.at("autoRank").get<bool>();
        std::set<std::string> ids, keys;
        for (const auto& m : j.at("mice")) {
            const auto id=m.at("id").get<std::string>();
            if (id.empty() || id.size()>128 || !ids.insert(id).second) throw std::runtime_error("Invalid or duplicate mouse ID.");
            loaded.mice_.push_back({id,CleanName(m.at("name").get<std::string>())});
        }
        ids.clear();
        for (const auto& s : j.at("setups")) {
            if (!s.at("pollingHz").is_number_integer() || s.at("pollingHz").get<int64_t>()<0 || s.at("pollingHz").get<uint64_t>()>32000)
                throw std::runtime_error("Invalid polling rate.");
            MouseSetup setup{s.at("id"),s.at("mouseId"),s.at("connection"),s.at("label"),s.at("pollingHz")};
            CheckSetup(setup.connection,setup.pollingHz); setup.label=CleanName(setup.label,false);
            if (setup.id.empty() || setup.id.size()>128 || !ids.insert(setup.id).second || !loaded.FindMouse(setup.mouseId)) throw std::runtime_error("Invalid setup ID or mouse link.");
            loaded.setups_.push_back(std::move(setup));
        }
        loaded.bindings_=j.at("bindings").get<std::map<std::string,std::string>>();
        for (const auto& [pathKey,setup] : loaded.bindings_) if (pathKey.empty() || !loaded.FindSetup(setup)) throw std::runtime_error("Invalid device binding.");
        for (const auto& c : j.at("comparisons")) {
            if (!c.at("method").is_number_integer() || c.at("method").get<int64_t>()<0 || c.at("method").get<uint64_t>()>2 ||
                !c.at("cycles").is_number_integer() || c.at("cycles").get<int64_t>()<3)
                throw std::runtime_error("Invalid comparison method or cycle count.");
            SavedComparison result{c.at("recordingKey"),c.at("setupA"),c.at("setupB"),c.at("savedAt"),c.at("method"),
                c.at("differenceMs"),c.at("spreadMs"),c.at("binMs"),c.at("cycles"),c.at("enabled")};
            if (result.recordingKey.size()!=64 || result.recordingKey.find_first_not_of("0123456789abcdef")!=std::string::npos ||
                !keys.insert(result.recordingKey+std::to_string(result.method)).second ||
                !loaded.FindSetup(result.setupA) || !loaded.FindSetup(result.setupB) || result.setupA==result.setupB ||
                !std::isfinite(result.differenceMs) || !std::isfinite(result.spreadMs) || !std::isfinite(result.binMs) || result.spreadMs<0 || result.binMs<=0)
                throw std::runtime_error("Invalid stored comparison.");
            loaded.comparisons_.push_back(std::move(result));
        }
        *this=std::move(loaded); return true;
    } catch (const std::exception& e) { error=e.what(); return false; }
}
}
