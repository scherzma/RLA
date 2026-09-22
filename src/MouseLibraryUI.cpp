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
    if (currentSession_.captureTestMode) { comparisonMessage_="Capture tests are diagnostic recordings and are not added to rankings."; return; }
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
    ImGui::SetNextItemWidth((std::min)(420.0f, ImGui::GetContentRegionAvail().x * 0.65f));
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
    if (ImGui::Button("New mouse or connection...")) {
        editMouse_.clear(); mouseName_[0]='\0'; setupLabel_[0]='\0'; setupRate_=0;
        ImGui::OpenPopup("Link this device");
    }
    if (ImGui::BeginPopup("Link this device")) {
        ImGui::TextUnformatted("Save this device as");
        const auto* mouse=mouseLibrary_.FindMouse(editMouse_);
        ImGui::SetNextItemWidth(300);
        if (ImGui::BeginCombo("Mouse",mouse ? mouse->name.c_str() : "New mouse")) {
            if (ImGui::Selectable("New mouse",editMouse_.empty())) editMouse_.clear();
            for (const auto& entry : mouseLibrary_.Mice()) {
                ImGui::PushID(entry.id.c_str());
                if (ImGui::Selectable(entry.name.c_str(),editMouse_==entry.id)) editMouse_=entry.id;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (editMouse_.empty()) { ImGui::SetNextItemWidth(300); ImGui::InputText("Name",mouseName_,sizeof(mouseName_)); }
        const char* connections[]={"Wired","Wireless","Bluetooth","Other"};
        ImGui::SetNextItemWidth(180); ImGui::Combo("Connection",&connectionIndex_,connections,4);
        ImGui::TextUnformatted("Polling rate: detect after recording");
        if (ImGui::TreeNode("Optional settings")) {
            ImGui::SetNextItemWidth(180); ImGui::InputInt("Known Hz (0 = detect)",&setupRate_,0,0);
            ImGui::SetNextItemWidth(300); ImGui::InputText("Notes",setupLabel_,sizeof(setupLabel_));
            ImGui::TreePop();
        }
        ImGui::TextUnformatted("For another connection, select the same mouse name above.");
        ImGui::BeginDisabled((editMouse_.empty() && mouseName_[0]=='\0') || setupRate_<0 || setupRate_>32000);
        if (ImGui::Button("Save and link device")) {
            try {
                if (editMouse_.empty()) editMouse_=mouseLibrary_.AddMouse(mouseName_);
                const auto setup=mouseLibrary_.AddSetup(editMouse_,connections[connectionIndex_],setupRate_,setupLabel_);
                mouseLibrary_.Bind(key,setup); LibraryChanged(); ImGui::CloseCurrentPopup();
            } catch (const std::exception& e) { libraryMessage_=e.what(); }
        }
        ImGui::EndDisabled(); ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
    if (libraryPage_==0) ImGui::TextWrapped("Save one name per mouse. Add wired or wireless setups under that name. Polling rate is detected after recording.");
    if (libraryMessage_!="Mouse library saved." && libraryMessage_!="Saving mouse library...") ImGui::TextWrapped("%s",libraryMessage_.c_str());
    if (librarySaveFailed_ && ImGui::Button("Retry saving")) librarySaveFailed_=false;
    ImGui::BeginDisabled(libraryReadOnly_);
    if (libraryPage_==0) {
    ImGui::SeparatorText("Name and link your mice");
    RenderDeviceBinding("Reference A",deviceManager_->GetMouseADevice());
    RenderDeviceBinding("Test B",deviceManager_->GetMouseBDevice());
    ImGui::TextWrapped("Choose a saved setup, or use New mouse or connection. RLA remembers the link.");
    if (ImGui::CollapsingHeader("Create or edit a mouse profile")) {
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
            if (ImGui::TreeNode("Move an existing setup to another mouse")) {
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
            ImGui::TextWrapped("Use this to link wired and wireless setups to the same mouse name. Their results stay separate.");
            ImGui::TreePop();
            }
        }
    }
    ImGui::SeparatorText("After recording");
    if (ImGui::Checkbox("Analyze and add valid results to rankings automatically",&mouseLibrary_.autoRank)) LibraryChanged();
    ImGui::TextWrapped("Move both mice together through several smooth back-and-forth cycles. Click or press Esc to stop raw capture. Each completed recording is saved automatically.");
    if (ImGui::Button("Continue to recording")) showLibrary_=false;
    ImGui::EndDisabled();
    return;
    }
    ImGui::SeparatorText("Latest recording");
    if (!latencyFit_.valid) {
        ImGui::TextWrapped("Analyze a recording with both mice before adding a result.");
        if (ImGui::Button("Go to analysis")) showLibrary_=false;
    } else {
        ImGui::Text("B is %.3f ms %s than A",std::abs(latencyFit_.differenceMs),latencyFit_.differenceMs>=0 ? "later" : "earlier");
    }
    if ((!currentSession_.eventsA.empty() || !currentSession_.eventsB.empty()) && ImGui::CollapsingHeader("Check recording labels and save result",ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(state_==AppState::Recording);
        auto a=currentSession_.mouseA.setupId,b=currentSession_.mouseB.setupId;
        if (SetupCombo("Recorded A",a)) currentSession_.mouseA=mouseLibrary_.Identity(a,currentSession_.mouseA.devicePath);
        if (SetupCombo("Recorded B",b)) currentSession_.mouseB=mouseLibrary_.Identity(b,currentSession_.mouseB.devicePath);
        ImGui::TextDisabled("These labels belong to this recording, not to the currently connected mice.");
        ImGui::BeginDisabled(!latencyFit_.valid);
        if (ImGui::Button("Save result to rankings")) SaveComparison();
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
        ImGui::TableSetupColumn("Group / place",ImGuiTableColumnFlags_WidthFixed,100);
        ImGui::TableSetupColumn("Mouse setup",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Relative ms",ImGuiTableColumnFlags_WidthFixed,110);
        ImGui::TableSetupColumn("Recordings",ImGuiTableColumnFlags_WidthFixed,90);
        ImGui::TableSetupColumn("Group mismatch ms",ImGuiTableColumnFlags_WidthFixed,150);
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
    if (ImGui::TreeNode("How rankings work")) {
        ImGui::TextWrapped("Pair medians feed the ranking. Group mismatch measures disagreement in comparison loops, not measurement accuracy. Best run means the smallest spread, not the lowest latency.");
        ImGui::TreePop();
    }
    if (ImGui::CollapsingHeader("Saved runs",ImGuiTreeNodeFlags_DefaultOpen)) {
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
            ImGui::TableSetupColumn("Use",ImGuiTableColumnFlags_WidthFixed,35);
            ImGui::TableSetupColumn("Reference A",ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Test B",ImGuiTableColumnFlags_WidthStretch);
            for (const char* name : {"B-A ms","Spread ms","Cycles","Run"}) ImGui::TableSetupColumn(name,ImGuiTableColumnFlags_WidthFixed,85);
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
