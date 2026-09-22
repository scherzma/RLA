#include "App.h"
#include <imgui.h>
#include <algorithm>
#include <format>
#include <shellapi.h>

namespace RLA {
namespace {
std::string PersistLibrary(const MouseLibrary& library, const std::filesystem::path& path, const std::deque<RecordingSession>& runs) {
    // Keep every referenced recording before publishing this library snapshot.
    for (const auto& run : runs) {
        try {
            const auto target=path.parent_path()/L"Runs"/(MouseLibrary::RecordingKey(run)+".json");
            std::filesystem::create_directories(target.parent_path());
            auto temporary=target; temporary+=L".part";
            DataStore store;
            if (!store.SaveToJson(temporary,run,{},L"Reference",L"Test")) return store.GetLastError();
            if (!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                return "Could not keep the comparison recording.";
        } catch (const std::exception& e) { return e.what(); }
    }
    return library.Save(path);
}
void FillText(char* buffer, size_t capacity, const std::string& text) {
    const auto count=(std::min)(capacity-1,text.size()); std::copy_n(text.data(),count,buffer); buffer[count]='\0';
}
}

void App::LibraryChanged() { libraryDirty_=true; rankingDirty_=true; }
void App::PollLibrarySave() {
    if (librarySaveTask_.valid()) {
        if (librarySaveTask_.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return;
        std::string error;
        try { error=librarySaveTask_.get(); } catch (const std::exception& e) { error=e.what(); }
        librarySaveFailed_=!error.empty();
        if (librarySaveFailed_) { libraryMessage_="Library save failed: "+error; libraryDirty_=true; }
        else {
            for (size_t i=0;i<savingRuns_;++i) runsToKeep_.pop_front();
            libraryMessage_="Mouse library saved.";
        }
        savingRuns_=0;
    }
    if (libraryReadOnly_ || librarySaveFailed_ || libraryPath_.empty() || (!libraryDirty_ && runsToKeep_.empty())) return;
    try {
        savingRuns_=runsToKeep_.size();
        librarySaveTask_=std::async(std::launch::async,[library=mouseLibrary_,path=libraryPath_,runs=runsToKeep_] {
            return PersistLibrary(library,path,runs);
        });
        libraryDirty_=false;
        libraryMessage_="Saving mouse library...";
    } catch (const std::exception& e) { librarySaveFailed_=true; libraryMessage_=e.what(); }
}

void App::CaptureMouseIdentity() {
    auto identity=[&](const MouseDevice* device) {
        const auto path=device ? MouseLibrary::DeviceKey(device->path) : "";
        return mouseLibrary_.Identity(mouseLibrary_.BoundSetup(path),path);
    };
    currentSession_.mouseA=identity(deviceManager_->GetMouseADevice());
    currentSession_.mouseB=identity(deviceManager_->GetMouseBDevice());
}
void App::DetectSessionRates() {
    if (libraryReadOnly_) return;
    auto detect=[&](RecordingMouse& mouse,const std::vector<MouseEvent>& events) {
        const auto* known=mouseLibrary_.FindSetup(mouse.setupId);
        if (!known || events.empty()) return;
        const auto setup=*known;
        const int hz=MouseLibrary::DetectRate(events,currentSession_.qpcFrequency);
        const auto id=mouseLibrary_.AddSetup(setup.mouseId,setup.connection,hz,setup.label);
        mouse=mouseLibrary_.Identity(id,mouse.devicePath);
        if (!mouse.devicePath.empty()) mouseLibrary_.Bind(mouse.devicePath,id);
        LibraryChanged();
    };
    try { detect(currentSession_.mouseA,currentSession_.eventsA); detect(currentSession_.mouseB,currentSession_.eventsB); }
    catch (const std::exception& e) { libraryMessage_=e.what(); }
}
void App::SaveComparison() {
    if (libraryReadOnly_ || libraryPath_.empty()) { comparisonMessage_="Mouse library is unavailable."; return; }
    try {
        if (!mouseLibrary_.AddComparison(currentSession_,latencyFit_,resultMethod_)) {
            comparisonMessage_="Choose two different saved setups with known Hz, then obtain a valid latency result."; return;
        }
        runsToKeep_.push_back(currentSession_);
        LibraryChanged();
        comparisonMessage_="Comparison added. Repeating this analysis updates the same run.";
    } catch (const std::exception& e) { comparisonMessage_=e.what(); }
}
bool App::SetupCombo(const char* label,std::string& selection) {
    bool changed=false;
    ImGui::SetNextItemWidth(420);
    if (ImGui::BeginCombo(label,mouseLibrary_.Label(selection).c_str())) {
        if (ImGui::Selectable("Not linked",selection.empty())) { selection.clear(); changed=true; }
        for (const auto& setup : mouseLibrary_.Setups()) {
            ImGui::PushID(setup.id.c_str());
            if (ImGui::Selectable(mouseLibrary_.Label(setup.id).c_str(),selection==setup.id)) { selection=setup.id; changed=true; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}
void App::RenderDeviceBinding(const char* label,const MouseDevice* device) {
    ImGui::PushID(label);
    ImGui::Text("%s: %s",label,device ? GetMouseDisplayName(device).c_str() : "Not assigned");
    const auto key=device ? MouseLibrary::DeviceKey(device->path) : "";
    auto selected=mouseLibrary_.BoundSetup(key);
    ImGui::BeginDisabled(key.empty() || state_==AppState::Recording || libraryReadOnly_);
    if (SetupCombo("Saved setup",selected)) {
        try { mouseLibrary_.Bind(key,selected); LibraryChanged(); } catch (const std::exception& e) { libraryMessage_=e.what(); }
    }
    ImGui::EndDisabled();
    ImGui::PopID();
}
void App::OpenRankedRun(const std::string& key) {
    if (state_==AppState::Recording || key.size()!=64) return;
    auto session=dataStore_->LoadFromJson(libraryPath_.parent_path()/L"Runs"/(key+".json"));
    if (!session) { libraryMessage_="Could not open run: "+dataStore_->GetLastError(); return; }
    currentSession_=std::move(*session); RebuildPlotData(); analysisResult_={}; TransitionTo(AppState::Ready);
    showLibrary_=false; statusMessage_="Saved comparison loaded.";
}

void App::RenderMouseLibrary() {
    ImGui::TextUnformatted("One mouse profile can contain wired, wireless, and polling-rate setups. DPI does not create a setup.");
    ImGui::TextWrapped("%s",libraryMessage_.c_str());
    if (librarySaveFailed_ && ImGui::Button("Retry saving")) librarySaveFailed_=false;
    ImGui::BeginDisabled(libraryReadOnly_);
    if (ImGui::Checkbox("Analyze and rank after recording",&mouseLibrary_.autoRank)) LibraryChanged();
    ImGui::TextDisabled("Only valid analyses with two linked setups are added. Accepted recordings are kept in RLA/Runs.");
    if (ImGui::CollapsingHeader("Mouse profiles and connections",ImGuiTreeNodeFlags_DefaultOpen)) {
        auto edit=[&](auto action) {
            try { action(); LibraryChanged(); } catch (const std::exception& e) { libraryMessage_=e.what(); }
        };
        {
            const auto* selected=mouseLibrary_.FindMouse(editMouse_);
            ImGui::SetNextItemWidth(280);
            if (ImGui::BeginCombo("Mouse profile",selected ? selected->name.c_str() : "Select or create")) {
                for (const auto& mouse : mouseLibrary_.Mice()) {
                    ImGui::PushID(mouse.id.c_str());
                    if (ImGui::Selectable(mouse.name.c_str(),editMouse_==mouse.id)) { editMouse_=mouse.id; FillText(mouseName_,sizeof(mouseName_),mouse.name); }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::SetNextItemWidth(280); ImGui::InputText("Mouse name",mouseName_,sizeof(mouseName_));
            ImGui::BeginDisabled(mouseName_[0]=='\0');
            if (ImGui::Button("Add mouse")) edit([&] { editMouse_=mouseLibrary_.AddMouse(mouseName_); });
            ImGui::SameLine(); ImGui::BeginDisabled(!mouseLibrary_.FindMouse(editMouse_));
            if (ImGui::Button("Rename mouse")) edit([&] { mouseLibrary_.RenameMouse(editMouse_,mouseName_); });
            ImGui::EndDisabled(); ImGui::EndDisabled();
            const char* connections[]={"Wired","Wireless","Bluetooth","Other"};
            ImGui::SetNextItemWidth(130); ImGui::Combo("Connection",&connectionIndex_,connections,4);
            ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::InputInt("Hz (0 = auto)",&setupRate_,0,0);
            ImGui::SetNextItemWidth(420); ImGui::InputText("Setup notes",setupLabel_,sizeof(setupLabel_));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("For example: performance mode, firmware, or receiver placement.\nUse a separate setup label when you want to compare these settings.");
            ImGui::BeginDisabled(!mouseLibrary_.FindMouse(editMouse_) || setupRate_<0 || setupRate_>32000);
            if (ImGui::Button("Add setup to selected mouse")) {
                edit([&] { editSetup_=mouseLibrary_.AddSetup(editMouse_,connections[connectionIndex_],setupRate_,setupLabel_); setupParent_=editMouse_; });
            }
            ImGui::EndDisabled();
            if (SetupCombo("Existing setup",editSetup_)) {
                if (const auto* setup=mouseLibrary_.FindSetup(editSetup_)) { setupParent_=setup->mouseId; FillText(setupLabel_,sizeof(setupLabel_),setup->label); }
            }
            const auto* parent=mouseLibrary_.FindMouse(setupParent_);
            ImGui::SetNextItemWidth(280);
            if (ImGui::BeginCombo("Link setup to mouse",parent ? parent->name.c_str() : "Select mouse")) {
                for (const auto& mouse : mouseLibrary_.Mice()) {
                    ImGui::PushID(mouse.id.c_str());
                    if (ImGui::Selectable(mouse.name.c_str(),setupParent_==mouse.id)) setupParent_=mouse.id;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::BeginDisabled(!mouseLibrary_.FindSetup(editSetup_) || !mouseLibrary_.FindMouse(setupParent_));
            if (ImGui::Button("Apply link and notes")) edit([&] { mouseLibrary_.LinkSetup(editSetup_,setupParent_,setupLabel_); });
            ImGui::EndDisabled();
            ImGui::TextDisabled("Linking groups the mouse identities. It does not combine their latency results.");
        }
    }
    if (ImGui::CollapsingHeader("Connected mice",ImGuiTreeNodeFlags_DefaultOpen)) {
        RenderDeviceBinding("A",deviceManager_->GetMouseADevice());
        RenderDeviceBinding("B",deviceManager_->GetMouseBDevice());
        ImGui::TextWrapped("Assign each mouse by movement, then link its saved setup here. RLA remembers each interface. Link a new port or receiver manually if needed.");
        ImGui::TextWrapped("Hz is estimated from steady movement after recording. Grouped or sparse events leave the rate unspecified and are not ranked. Move faster to detect it, or select a known rate under Current recording. A device setting is not read from the mouse.");
    }
    if ((!currentSession_.eventsA.empty() || !currentSession_.eventsB.empty()) && ImGui::CollapsingHeader("Current recording")) {
        ImGui::BeginDisabled(state_==AppState::Recording);
        auto a=currentSession_.mouseA.setupId,b=currentSession_.mouseB.setupId;
        if (SetupCombo("Recorded A",a)) currentSession_.mouseA=mouseLibrary_.Identity(a,currentSession_.mouseA.devicePath);
        if (SetupCombo("Recorded B",b)) currentSession_.mouseB=mouseLibrary_.Identity(b,currentSession_.mouseB.devicePath);
        ImGui::TextDisabled("These labels belong to this recording, not to the currently connected mice.");
        ImGui::BeginDisabled(!latencyFit_.valid);
        if (ImGui::Button("Add current latency result")) SaveComparison();
        ImGui::EndDisabled(); ImGui::EndDisabled();
        ImGui::TextWrapped("%s",comparisonMessage_.c_str());
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("Relative latency ranking");
    ImGui::SetNextItemWidth(150);
    if (ImGui::Combo("Analysis method",&rankMethod_,"Minima\0Half-height\0Both\0")) rankingDirty_=true;
    if (rankingDirty_) {
        try { ranking_=mouseLibrary_.Ranking(rankMethod_); } catch (const std::exception& e) { ranking_.clear(); libraryMessage_=e.what(); }
        rankingDirty_=false;
    }
    ImGui::TextWrapped("Lower is earlier within the same group. Separate groups have no shared reference. Values are relative estimates, not absolute device latency. Small differences can be inconclusive.");
    if (ImGui::BeginTable("Mouse ranking",5,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerV)) {
        for (const char* name : {"Group / place","Mouse setup","Relative ms","Recordings","Group mismatch ms"}) ImGui::TableSetupColumn(name);
        ImGui::TableHeadersRow();
        for (const auto& row : ranking_) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (row.group) ImGui::Text("%d / %d",row.group,row.place); else ImGui::TextUnformatted("Unranked");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mouseLibrary_.Label(row.setupId).c_str());
            ImGui::TableNextColumn(); if (row.group) ImGui::Text("+%.3f",row.relativeMs); else ImGui::TextUnformatted("--");
            ImGui::TableNextColumn(); ImGui::Text("%zu",row.recordings);
            ImGui::TableNextColumn(); if (row.group) ImGui::Text("%.3f",row.residualMs); else ImGui::TextUnformatted("--");
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Pair medians feed the ranking. Group mismatch measures disagreement in comparison loops, not measurement accuracy.");
    if (ImGui::CollapsingHeader("Saved runs")) {
        ImGui::TextWrapped("All accepted runs are retained. Best marks the smallest spread against the same reference and method; ties favor more cycles. It does not select the lowest latency value.");
        const auto& runs=mouseLibrary_.Comparisons();
        std::map<std::pair<std::string,std::string>,size_t> bestRuns;
        for (size_t i=0;i<runs.size();++i) {
            const auto& run=runs[i];
            if (!run.enabled || run.method!=rankMethod_) continue;
            const auto [entry,inserted]=bestRuns.emplace(std::make_pair(run.setupA,run.setupB),i);
            const auto& previous=runs[entry->second];
            if (!inserted && (run.spreadMs<previous.spreadMs || (run.spreadMs==previous.spreadMs && run.cycles>previous.cycles))) entry->second=i;
        }
        std::string openKey;
        if (ImGui::BeginTable("Comparison history",7,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(0,230))) {
            for (const char* name : {"Use","Reference A","Test B","B-A ms","Spread ms","Cycles","Run"}) ImGui::TableSetupColumn(name);
            ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
            for (size_t i=0;i<runs.size();++i) {
                const auto& run=runs[i]; if (run.method!=rankMethod_) continue;
                const auto bestEntry=bestRuns.find({run.setupA,run.setupB});
                const bool best=bestEntry!=bestRuns.end() && bestEntry->second==i;
                ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
                bool enabled=run.enabled; ImGui::BeginDisabled(libraryReadOnly_);
                if (ImGui::Checkbox("##Use run",&enabled)) { mouseLibrary_.SetEnabled(i,enabled); LibraryChanged(); }
                ImGui::EndDisabled();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(mouseLibrary_.Label(run.setupA).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(mouseLibrary_.Label(run.setupB).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%+.3f",run.differenceMs);
                ImGui::TableNextColumn(); ImGui::Text("%.3f",run.spreadMs);
                ImGui::TableNextColumn(); ImGui::Text("%zu",run.cycles);
                ImGui::TableNextColumn(); ImGui::BeginDisabled(state_==AppState::Recording);
                if (ImGui::SmallButton(best ? "Open best" : "Open")) openKey=run.recordingKey;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",run.savedAt.c_str());
                ImGui::EndDisabled(); ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!openKey.empty()) OpenRankedRun(openKey);
    }
}
}
