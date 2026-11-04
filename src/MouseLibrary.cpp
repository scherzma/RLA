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
#include <tuple>
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
        !std::isfinite(fit.binMs) || fit.spreadMs < 0 || fit.binMs <= 0 || method < 0 || method > LatencyMethod::Last ||
        LatencyMethod::IsDataAge(method) != (fit.binning == LatencyBinning::Arrival) ||
        !a || !b || a->pollingHz==0 || b->pollingHz==0 || session.mouseA.setupId == session.mouseB.setupId || session.eventsA.empty() || session.eventsB.empty() ||
        a->connection!=session.mouseA.connection || b->connection!=session.mouseB.connection ||
        a->pollingHz!=session.mouseA.pollingHz || b->pollingHz!=session.mouseB.pollingHz) return false;
    SYSTEMTIME now{}; GetSystemTime(&now);
    SavedComparison result{RecordingKey(session), session.mouseA.setupId, session.mouseB.setupId,
        std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond),
        method, fit.differenceMs, fit.spreadMs, fit.binMs, fit.matchedCycles};
    const auto orientation=Analyzer::FitOrientation(session);
    if (orientation.valid) result.angleDeg=orientation.angleDeg;
    for (auto& old : comparisons_) if (old.recordingKey == result.recordingKey && old.method == method) {
        result.enabled = old.enabled;
        if (!std::isfinite(result.angleDeg)) result.angleDeg = old.angleDeg;
        old = std::move(result); return true;
    }
    comparisons_.push_back(std::move(result)); return true;
}
size_t MouseLibrary::SetAngle(const std::string& recordingKey, double angleDeg) {
    if (!std::isfinite(angleDeg) || std::abs(angleDeg)>180) return 0;
    size_t changed=0;
    for (auto& run : comparisons_) if (run.recordingKey==recordingKey) { run.angleDeg=angleDeg; ++changed; }
    return changed;
}
std::vector<std::string> MouseLibrary::KeysWithoutAngle() const {
    std::set<std::string> keys;
    for (const auto& run : comparisons_) if (!std::isfinite(run.angleDeg)) keys.insert(run.recordingKey);
    return {keys.begin(),keys.end()};
}
std::vector<PlacementBalance> MouseLibrary::Placements(bool dataAge) const {
    // One row per recording: the measured result, else the newest of its kind.
    std::map<std::string,SavedComparison> perRecording;
    for (const auto& row : ResultRows(dataAge)) {
        const auto& run=row.run;
        if (!run.enabled || !std::isfinite(run.angleDeg)) continue;
        const auto [entry,inserted]=perRecording.emplace(run.recordingKey,run);
        if (!inserted && !row.calculated && run.method>=entry->second.method) entry->second=run;
    }
    struct Sums { std::vector<double> ms; double angle=0; size_t n=0; };
    std::map<std::pair<std::string,std::string>,std::array<Sums,3>> pairs; // positive, negative, aligned
    for (const auto& [key,run] : perRecording) {
        const bool forward=run.setupA<run.setupB;
        const double ms=forward ? run.differenceMs : -run.differenceMs, angle=forward ? run.angleDeg : -run.angleDeg;
        auto& groups=pairs[forward ? std::make_pair(run.setupA,run.setupB) : std::make_pair(run.setupB,run.setupA)];
        auto& g=groups[angle>PlacementAngleDeg ? 0 : angle< -PlacementAngleDeg ? 1 : 2];
        g.ms.push_back(ms); g.angle+=angle; ++g.n;
    }
    std::vector<PlacementBalance> result;
    for (const auto& [pair,groups] : pairs) {
        PlacementBalance balance{pair.first,pair.second};
        PlacementGroup* targets[3]={&balance.positive,&balance.negative,&balance.aligned};
        for (size_t i=0;i<3;++i) if (groups[i].n)
            *targets[i]={groups[i].n,Median(groups[i].ms),groups[i].angle/groups[i].n};
        balance.balanced=balance.positive.runs && balance.negative.runs;
        if (balance.balanced) balance.balancedMs=(balance.positive.medianMs+balance.negative.medianMs)/2;
        result.push_back(std::move(balance));
    }
    return result;
}
PlacementNote MouseLibrary::Placement(const std::string& setupA, const std::string& setupB, double angleDeg,
    const std::string& excludeKey) const {
    PlacementNote note;
    if (!std::isfinite(angleDeg) || std::abs(angleDeg)<=PlacementAngleDeg) return note;
    std::set<std::string> seen;
    double sum=0;
    for (const auto& run : comparisons_) {
        if (!run.enabled || !std::isfinite(run.angleDeg) || run.recordingKey==excludeKey || !seen.insert(run.recordingKey).second) continue;
        double angle;
        if (run.setupA==setupA && run.setupB==setupB) angle=run.angleDeg;
        else if (run.setupA==setupB && run.setupB==setupA) angle=-run.angleDeg;
        else continue;
        if (std::abs(angle)<=PlacementAngleDeg) continue;
        if ((angle>0)==(angleDeg>0)) ++note.same;
        else { ++note.opposite; sum+=angle; }
    }
    if (note.opposite) note.oppositeAngleDeg=sum/note.opposite;
    return note;
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

double MouseLibrary::ReportIntervalMs(const std::string& setupId) const {
    const auto* setup=FindSetup(setupId);
    return setup && setup->pollingHz>0 ? 1000.0/setup->pollingHz : 0.0;
}

std::vector<ResultRow> MouseLibrary::ResultRows(bool dataAge) const {
    using Key=std::tuple<std::string,std::string,std::string>;
    std::vector<ResultRow> rows;
    std::set<Key> measured;
    for (size_t i=0;i<comparisons_.size();++i) {
        const auto& run=comparisons_[i];
        if (LatencyMethod::IsDataAge(run.method)!=dataAge) continue;
        rows.push_back({run,i,false});
        if (dataAge) measured.insert({run.recordingKey,run.setupA,run.setupB});
    }
    if (!dataAge) return rows;
    // Data age = pipeline + mean sampling age difference. Measured results agree
    // with this relation; it fills recordings saved before data age existed.
    std::map<Key,size_t> sources;
    for (size_t i=0;i<comparisons_.size();++i) {
        const auto& run=comparisons_[i];
        const Key key{run.recordingKey,run.setupA,run.setupB};
        if (LatencyMethod::IsDataAge(run.method) || measured.count(key)) continue;
        const auto [entry,inserted]=sources.emplace(key,i);
        if (!inserted && run.method>comparisons_[entry->second].method) entry->second=i;
    }
    for (const auto& [key,index] : sources) {
        const auto& source=comparisons_[index];
        const double intervalA=ReportIntervalMs(source.setupA), intervalB=ReportIntervalMs(source.setupB);
        if (intervalA<=0 || intervalB<=0) continue;
        ResultRow row{source,index,true};
        row.run.method=LatencyMethod::DataAge;
        row.run.differenceMs+=(intervalB-intervalA)/2;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<SavedComparison> MouseLibrary::Selected(int method) const {
    std::vector<SavedComparison> runs;
    if (method==LatencyMethod::DataAge) {
        for (auto& row : ResultRows(true)) if (row.run.enabled) runs.push_back(std::move(row.run));
        return runs;
    }
    if (method>=0) {
        for (const auto& run : comparisons_) if (run.enabled && run.method==method) runs.push_back(run);
        return runs;
    }
    // Pipeline ranking: one result per recording, the newest pipeline method.
    std::map<std::tuple<std::string,std::string,std::string>,const SavedComparison*> byRecording;
    for (const auto& run : comparisons_) {
        if (!run.enabled || LatencyMethod::IsDataAge(run.method)) continue;
        auto& selected=byRecording[std::make_tuple(run.recordingKey,run.setupA,run.setupB)];
        if (!selected || run.method>selected->method) selected=&run;
    }
    for (const auto& [key,run] : byRecording) runs.push_back(*run);
    return runs;
}


DirectRunSummary MouseLibrary::DirectRuns(const std::string& setup, const std::string& reference, int method) const {
    DirectRunSummary result;
    std::string latestTime;
    if (setup==reference) return result;
    for (const auto& run : Selected(method)) {
        double delta;
        if (run.setupA==reference && run.setupB==setup) delta=run.differenceMs;
        else if (run.setupB==reference && run.setupA==setup) delta=-run.differenceMs;
        else continue;
        if (!result.recordings || delta<result.fastestMs) {
            result.fastestMs=delta; result.fastestKey=run.recordingKey;
        }
        if (!result.recordings || run.savedAt>=latestTime) {
            result.latestMs=delta; result.latestKey=run.recordingKey; latestTime=run.savedAt;
        }
        ++result.recordings;
    }
    return result;
}

std::vector<MouseRank> MouseLibrary::Ranking(int method, bool placementCorrected) const {
    // Robust median per pair, then fit B-A constraints within each connected group.
    using Pair = std::pair<size_t, size_t>;
    std::map<std::string, size_t> indices;
    std::vector<MouseRank> rows;
    for (const auto& setup : setups_) { indices[setup.id] = rows.size(); rows.push_back({setup.id}); }
    struct PairRuns { std::vector<double> all, positive, negative; };
    std::map<Pair, PairRuns> differences;
    for (const auto& c : Selected(method)) {
        const auto ai = indices.find(c.setupA), bi = indices.find(c.setupB);
        if (ai == indices.end() || bi == indices.end() || ai->second == bi->second) continue;
        const auto a = ai->second, b = bi->second;
        ++rows[a].recordings; ++rows[b].recordings;
        auto& runs = differences[{(std::min)(a,b),(std::max)(a,b)}];
        const double value = a < b ? c.differenceMs : -c.differenceMs;
        runs.all.push_back(value);
        if (std::isfinite(c.angleDeg)) {
            const double angle = a < b ? c.angleDeg : -c.angleDeg;
            if (angle > PlacementAngleDeg) runs.positive.push_back(value);
            else if (angle < -PlacementAngleDeg) runs.negative.push_back(value);
        }
    }
    struct Edge { size_t a,b; double delta,weight; };
    std::vector<Edge> edges;
    std::vector<std::vector<size_t>> adjacent(rows.size());
    for (const auto& [pair, runs] : differences) {
        const bool corrected = placementCorrected && !runs.positive.empty() && !runs.negative.empty();
        const double delta = corrected ? (Median(runs.positive) + Median(runs.negative)) / 2 : Median(runs.all);
        for (size_t member : {pair.first, pair.second}) { ++rows[member].pairs; if (corrected) ++rows[member].correctedPairs; }
        const auto i = edges.size(); edges.push_back({pair.first,pair.second,delta,static_cast<double>(runs.all.size())});
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
        for (const auto& c : comparisons_) {
            j["comparisons"].push_back({{"recordingKey",c.recordingKey},{"setupA",c.setupA},{"setupB",c.setupB},
                {"savedAt",c.savedAt},{"method",c.method},{"differenceMs",c.differenceMs},{"spreadMs",c.spreadMs},{"binMs",c.binMs},{"cycles",c.cycles},{"enabled",c.enabled}});
            if (std::isfinite(c.angleDeg)) j["comparisons"].back()["angleDeg"]=c.angleDeg;
        }
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
            if (!c.at("method").is_number_integer() || c.at("method").get<int64_t>()<0 || c.at("method").get<uint64_t>()>LatencyMethod::Last ||
                !c.at("cycles").is_number_integer() || c.at("cycles").get<int64_t>()<3)
                throw std::runtime_error("Invalid comparison method or cycle count.");
            SavedComparison result{c.at("recordingKey"),c.at("setupA"),c.at("setupB"),c.at("savedAt"),c.at("method"),
                c.at("differenceMs"),c.at("spreadMs"),c.at("binMs"),c.at("cycles"),c.at("enabled")};
            if (result.recordingKey.size()!=64 || result.recordingKey.find_first_not_of("0123456789abcdef")!=std::string::npos ||
                !keys.insert(result.recordingKey+std::to_string(result.method)).second ||
                !loaded.FindSetup(result.setupA) || !loaded.FindSetup(result.setupB) || result.setupA==result.setupB ||
                !std::isfinite(result.differenceMs) || !std::isfinite(result.spreadMs) || !std::isfinite(result.binMs) || result.spreadMs<0 || result.binMs<=0)
                throw std::runtime_error("Invalid stored comparison.");
            if (c.contains("angleDeg")) {
                if (!c.at("angleDeg").is_number() || !std::isfinite(c.at("angleDeg").get<double>()) || std::abs(c.at("angleDeg").get<double>())>180)
                    throw std::runtime_error("Invalid stored mouse angle.");
                result.angleDeg=c.at("angleDeg").get<double>();
            }
            loaded.comparisons_.push_back(std::move(result));
        }
        *this=std::move(loaded); return true;
    } catch (const std::exception& e) { error=e.what(); return false; }
}
}
