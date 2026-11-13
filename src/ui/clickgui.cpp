#include "ui/clickgui.h"
#include "input/input_manager.h"
#include "input/key_names.h"
#include "modules/module_manager.h"
#include "config/config_manager.h"
#include "minecraft/sdk/client/client_instance.h"
#include "render/dx12/dx12_hook.h"
#include "ui/notifications.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {
enum class ProfileDialogAction { None, Overwrite, Delete };
struct UiState {
    char search[128]{};
    char profile[64] = "default";
    std::string category = "Visual";
    std::string confirmProfileName;
    ProfileDialogAction profileAction = ProfileDialogAction::None;
    Module* selected = nullptr;
    ImVec2 lastDisplaySize{};
    bool initialized = false;
    bool layoutInitialized = false;
};
UiState g_ui;

const ImVec4 kWindowBg{0.965f, 0.973f, 0.984f, 0.995f};
const ImVec4 kPanelBg{1.000f, 1.000f, 1.000f, 1.000f};
const ImVec4 kRaisedBg{0.940f, 0.953f, 0.973f, 1.000f};
const ImVec4 kBorder{0.835f, 0.865f, 0.905f, 1.000f};
const ImVec4 kText{0.120f, 0.155f, 0.205f, 1.000f};
const ImVec4 kMuted{0.390f, 0.445f, 0.515f, 1.000f};
const ImVec4 kAccent{0.145f, 0.390f, 0.690f, 1.000f};
const ImVec4 kAccentDim{0.875f, 0.925f, 0.980f, 1.000f};
const ImVec4 kWarm{0.690f, 0.390f, 0.120f, 1.000f};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
bool contains_case_insensitive(const std::string& value, const std::string& query) {
    return lower_copy(value).find(lower_copy(query)) != std::string::npos;
}
std::string display_category(const std::string& category) {
    return category == "Configs" ? "Profiles" : category;
}
std::vector<Module*> modules_for_category(const std::string& category) {
    auto modules = ModuleManager::instance().modules_in_category(category);
    modules.erase(std::remove(modules.begin(), modules.end(), nullptr), modules.end());
    return modules;
}
void notify_module_toggle(Module* module, bool desiredState) {
    if (!module) return;
    module->toggle();
    if (desiredState && !module->enabled())
        ui::notifications::warning(module->name(), "The feature could not be enabled. Check the log for details.");
    else if (module->enabled())
        ui::notifications::success(module->name(), "Feature enabled.");
    else
        ui::notifications::info(module->name(), "Feature disabled.");
}
void apply_jhon_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding=ImVec2(16,15); s.FramePadding=ImVec2(10,7);
    s.ItemSpacing=ImVec2(9,9); s.ItemInnerSpacing=ImVec2(7,6);
    s.WindowRounding=13.0f; s.ChildRounding=10.0f; s.FrameRounding=7.0f;
    s.PopupRounding=8.0f; s.ScrollbarRounding=8.0f;
    s.WindowBorderSize=1.0f; s.ChildBorderSize=1.0f; s.FrameBorderSize=1.0f;
    auto* c=s.Colors;
    c[ImGuiCol_WindowBg]=kWindowBg; c[ImGuiCol_ChildBg]=kPanelBg; c[ImGuiCol_PopupBg]=kPanelBg;
    c[ImGuiCol_Border]=kBorder; c[ImGuiCol_Text]=kText; c[ImGuiCol_TextDisabled]=kMuted;
    c[ImGuiCol_FrameBg]=ImVec4(0.965f,0.973f,0.984f,1.0f);
    c[ImGuiCol_FrameBgHovered]=ImVec4(0.920f,0.945f,0.975f,1.0f);
    c[ImGuiCol_FrameBgActive]=ImVec4(0.870f,0.920f,0.975f,1.0f);
    c[ImGuiCol_Button]=kRaisedBg; c[ImGuiCol_ButtonHovered]=ImVec4(0.885f,0.930f,0.980f,1.0f);
    c[ImGuiCol_ButtonActive]=ImVec4(0.825f,0.895f,0.970f,1.0f);
    c[ImGuiCol_Header]=kAccentDim; c[ImGuiCol_HeaderHovered]=ImVec4(0.825f,0.895f,0.970f,1.0f);
    c[ImGuiCol_HeaderActive]=ImVec4(0.760f,0.855f,0.955f,1.0f);
    c[ImGuiCol_CheckMark]=kAccent; c[ImGuiCol_SliderGrab]=kAccent;
    c[ImGuiCol_SliderGrabActive]=ImVec4(0.100f,0.315f,0.590f,1.0f);
    c[ImGuiCol_Separator]=kBorder; c[ImGuiCol_TitleBg]=kPanelBg; c[ImGuiCol_TitleBgActive]=kPanelBg;
    c[ImGuiCol_ScrollbarBg]=ImVec4(0.940f,0.950f,0.965f,1.0f);
    c[ImGuiCol_ScrollbarGrab]=ImVec4(0.740f,0.785f,0.840f,1.0f);
    c[ImGuiCol_ScrollbarGrabHovered]=ImVec4(0.640f,0.705f,0.790f,1.0f);
    c[ImGuiCol_ScrollbarGrabActive]=ImVec4(0.500f,0.610f,0.760f,1.0f);
    c[ImGuiCol_TextSelectedBg]=ImVec4(0.600f,0.760f,0.940f,0.45f);
    c[ImGuiCol_NavHighlight]=kAccent;
}
bool category_button(const std::string& label,bool active) {
    if(active) {
        ImGui::PushStyleColor(ImGuiCol_Button,kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_Text,kAccent);
    }
    const bool clicked=ImGui::Button(label.c_str(),ImVec2(-1.0f,36.0f));
    if(active) ImGui::PopStyleColor(4);
    return clicked;
}
void profile_feedback(bool ok,const char* successText,const char* errorText) {
    if(ok) ui::notifications::success("Profiles",successText);
    else ui::notifications::warning("Profiles",errorText);
}
void draw_profiles() {
    auto& manager=ConfigManager::instance();
    ImGui::TextColored(kAccent,"PROFILES");
    ImGui::TextDisabled("Active profile: %s",manager.active_config().c_str());
    ImGui::TextWrapped("Changes made to ContainerTransfer are saved to this active profile.");
    ImGui::Spacing();

    const float saveButtonWidth=92.0f;
    ImGui::SetNextItemWidth(std::max(110.0f,ImGui::GetContentRegionAvail().x-saveButtonWidth-8.0f));
    ImGui::InputTextWithHint("##profile_name","Profile name (letters, numbers, spaces, _ or -)",g_ui.profile,sizeof(g_ui.profile));
    ImGui::SameLine();
    if(ImGui::Button("Save",ImVec2(saveButtonWidth,0.0f))) {
        const std::string name(g_ui.profile);
        if(!manager.is_valid_config_name(name)) {
            ui::notifications::warning("Profiles","Use 1-48 letters, numbers, spaces, underscores or hyphens.");
        } else if(manager.config_exists(name)) {
            g_ui.confirmProfileName=name; g_ui.profileAction=ProfileDialogAction::Overwrite;
            ImGui::OpenPopup("Confirm Profile Action");
        } else {
            const bool ok=manager.save_config(name);
            profile_feedback(ok,"Profile saved.","Could not save the profile. Check the log.");
            strncpy_s(g_ui.profile,manager.active_config().c_str(),sizeof(g_ui.profile)-1);
        }
    }

    ImGui::Separator();
    ImGui::Text("Saved profiles");
    const auto profiles=manager.get_available_configs();
    if(profiles.empty()) ImGui::TextColored(kMuted,"No saved profiles yet.");
    for(const auto& profile:profiles) {
        ImGui::PushID(profile.c_str());
        if(ImGui::Selectable(profile.c_str(),profile==manager.active_config(),
                ImGuiSelectableFlags_AllowDoubleClick,ImVec2(-105.0f,34.0f)))
            strncpy_s(g_ui.profile,profile.c_str(),sizeof(g_ui.profile)-1);
        ImGui::SameLine();
        if(ImGui::SmallButton("Load")) {
            const bool ok=manager.load_config(profile);
            profile_feedback(ok,"Profile loaded.","Could not load this profile.");
            if(ok) strncpy_s(g_ui.profile,manager.active_config().c_str(),sizeof(g_ui.profile)-1);
        }
        ImGui::SameLine();
        if(profile=="default") ImGui::TextDisabled("Protected");
        else if(ImGui::SmallButton("Delete")) {
            g_ui.confirmProfileName=profile; g_ui.profileAction=ProfileDialogAction::Delete;
            ImGui::OpenPopup("Confirm Profile Action");
        }
        ImGui::PopID();
    }

    if(g_ui.profileAction!=ProfileDialogAction::None) ImGui::OpenPopup("Confirm Profile Action");
    if(ImGui::BeginPopupModal("Confirm Profile Action",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        if(g_ui.profileAction==ProfileDialogAction::Overwrite) {
            ImGui::TextWrapped("A profile named '%s' already exists. Replace it?",g_ui.confirmProfileName.c_str());
            if(ImGui::Button("Overwrite",ImVec2(110.0f,0.0f))) {
                const bool ok=manager.save_config(g_ui.confirmProfileName);
                profile_feedback(ok,"Profile overwritten.","Could not overwrite the profile.");
                if(ok) strncpy_s(g_ui.profile,manager.active_config().c_str(),sizeof(g_ui.profile)-1);
                g_ui.profileAction=ProfileDialogAction::None; ImGui::CloseCurrentPopup();
            }
        } else if(g_ui.profileAction==ProfileDialogAction::Delete) {
            ImGui::TextWrapped("Delete profile '%s'? This cannot be undone.",g_ui.confirmProfileName.c_str());
            if(ImGui::Button("Delete",ImVec2(110.0f,0.0f))) {
                const bool ok=manager.delete_config(g_ui.confirmProfileName);
                profile_feedback(ok,"Profile deleted.","Could not delete the profile.");
                strncpy_s(g_ui.profile,manager.active_config().c_str(),sizeof(g_ui.profile)-1);
                g_ui.profileAction=ProfileDialogAction::None; ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if(ImGui::Button("Cancel",ImVec2(110.0f,0.0f))) {
            g_ui.profileAction=ProfileDialogAction::None; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
}

namespace clickgui {
void render() {
    if(!g_ui.initialized) { apply_jhon_style(); g_ui.initialized=true; }
    ImGuiIO& io=ImGui::GetIO();
    const auto categories=ModuleManager::instance().categories();
    const auto allModules=ModuleManager::instance().modules();

    if(g_ui.category!="Configs") {
        const bool exists=std::find(categories.begin(),categories.end(),g_ui.category)!=categories.end();
        if(!exists) g_ui.category=categories.empty()?"Visual":categories.front();
    }
    if(g_ui.selected && std::find(allModules.begin(),allModules.end(),g_ui.selected)==allModules.end())
        g_ui.selected=nullptr;

    if(!g_ui.layoutInitialized || g_ui.lastDisplaySize.x!=io.DisplaySize.x || g_ui.lastDisplaySize.y!=io.DisplaySize.y) {
        const float windowWidth=std::clamp(io.DisplaySize.x-28.0f,320.0f,1040.0f);
        const float windowHeight=std::clamp(io.DisplaySize.y-28.0f,280.0f,700.0f);
        ImGui::SetNextWindowSize(ImVec2(windowWidth,windowHeight),ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x*0.5f,io.DisplaySize.y*0.5f),ImGuiCond_Always,ImVec2(0.5f,0.5f));
        g_ui.lastDisplaySize=io.DisplaySize; g_ui.layoutInitialized=true;
    }

    bool open=true;
    if(ImGui::Begin("CHANTERCLIENT     /     CONTROL PANEL",&open,ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,kRaisedBg);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(16.0f,10.0f));
        ImGui::BeginChild("##topbar",ImVec2(0,78.0f),true,
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 contentStart=ImGui::GetCursorPos();
        const float lineHeight=ImGui::GetTextLineHeight();
        const float groupHeight=lineHeight*2.0f+ImGui::GetStyle().ItemSpacing.y;
        const float contentHeight=ImGui::GetContentRegionAvail().y;
        const float groupTop=contentStart.y+std::max(0.0f,(contentHeight-groupHeight)*0.5f);
        const float centerLine=groupTop+(groupHeight-lineHeight)*0.5f;
        ImGui::SetCursorPos(ImVec2(16.0f,centerLine)); ImGui::TextColored(kAccent,"CC");
        ImGui::SetCursorPos(ImVec2(60.0f,groupTop));
        ImGui::BeginGroup(); ImGui::Text("CHANTERCLIENT");
        ImGui::TextColored(kMuted,"Minecraft Bedrock Edition 1.26.60"); ImGui::EndGroup();

        const bool inWorld=ClientInstance::get()!=nullptr && ClientInstance::getLocalPlayer()!=nullptr;
        const bool gameReady=ClientInstance::get()!=nullptr;
        const char* status=inWorld?"World Active":(gameReady?"Game Ready":"Waiting for Game");
        const ImVec4 statusColor=inWorld?ImVec4(0.12f,0.52f,0.30f,1.0f):kWarm;
        const float markerSpace=14.0f;
        const float statusWidth=ImGui::CalcTextSize(status).x+markerSpace;
        const float right=ImGui::GetWindowWidth()-ImGui::GetStyle().WindowPadding.x;
        const float statusX=std::max(contentStart.x,right-statusWidth);
        const ImVec2 topbarPos=ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(topbarPos.x+statusX+4.0f,topbarPos.y+centerLine+lineHeight*0.5f),
            3.2f,ImGui::GetColorU32(statusColor));
        ImGui::SetCursorPos(ImVec2(statusX+markerSpace,centerLine));
        ImGui::TextColored(statusColor,"%s",status);
        ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor();

        ImGui::Spacing();
        const float footerReserve=ImGui::GetTextLineHeightWithSpacing()+ImGui::GetStyle().ItemSpacing.y;
        const float bodyHeight=std::max(100.0f,ImGui::GetContentRegionAvail().y-footerReserve);
        const float availableWidth=ImGui::GetContentRegionAvail().x;
        const float sideWidth=std::clamp(availableWidth*0.18f,88.0f,176.0f);
        const float listWidth=std::clamp(availableWidth*0.30f,150.0f,292.0f);

        ImGui::BeginChild("##navigation",ImVec2(sideWidth,bodyHeight),true,
            ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextColored(kMuted,"MENU"); ImGui::Spacing();
        for(const auto& category:categories) {
            if(category=="Configs") continue;
            if(category_button(display_category(category),g_ui.category==category)) {
                g_ui.category=category; g_ui.selected=nullptr;
            }
        }
        ImGui::Spacing(); ImGui::Separator();
        if(category_button("Profiles",g_ui.category=="Configs")) {
            g_ui.category="Configs"; g_ui.selected=nullptr;
        }
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),ImGui::GetWindowHeight()-53.0f));
        ImGui::TextColored(kMuted,"INSERT: Toggle panel");
        ImGui::EndChild();
        ImGui::SameLine();

        // The feature list scrolls independently from both navigation and details.
        ImGui::BeginChild("##module_index",ImVec2(listWidth,bodyHeight),true);
        if(g_ui.category=="Configs") {
            ImGui::TextColored(kAccent,"Profiles");
            ImGui::TextColored(kMuted,"Saved settings");
            ImGui::TextWrapped("Select a profile to load it, or create a named copy.");
        } else {
            const auto modules=modules_for_category(g_ui.category);
            const std::string categoryName=display_category(g_ui.category);
            ImGui::TextColored(kAccent,"FEATURES");
            ImGui::TextColored(kMuted,"%zu features / %s",modules.size(),categoryName.c_str());
            ImGui::Spacing(); ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##module_search","Search features...",g_ui.search,sizeof(g_ui.search));
            ImGui::Spacing();
            std::vector<Module*> visibleModules;
            visibleModules.reserve(modules.size());
            for(Module* module:modules) {
                if(module && (g_ui.search[0]==0 || contains_case_insensitive(module->name(),g_ui.search)))
                    visibleModules.push_back(module);
            }

            if(!visibleModules.empty() && ImGui::BeginTable("##module_rows",2,
                    ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_NoSavedSettings|ImGuiTableFlags_NoBordersInBody)) {
                ImGui::TableSetupColumn("Feature",ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Toggle",ImGuiTableColumnFlags_WidthFixed,48.0f);
                for(Module* module:visibleModules) {
                    ImGui::PushID(module);
                    const bool selected=g_ui.selected==module;
                    const bool wasEnabled=module->enabled();
                    ImGui::TableNextRow(ImGuiTableRowFlags_None,40.0f);
                    ImGui::TableSetColumnIndex(0);
                    if(ImGui::Selectable(module->name().c_str(),selected,ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0.0f,36.0f))) g_ui.selected=module;
                    if(ImGui::IsItemHovered() && !module->tooltip().empty())
                        ImGui::SetTooltip("%s",module->tooltip().c_str());

                    ImGui::TableSetColumnIndex(1);
                    if(wasEnabled) {
                        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.86f,0.95f,0.89f,1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.10f,0.39f,0.23f,1.0f));
                    }
                    if(ImGui::SmallButton(wasEnabled?"ON":"OFF")) {
                        g_ui.selected=module; notify_module_toggle(module,!wasEnabled);
                    }
                    if(wasEnabled) ImGui::PopStyleColor(2);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            } else if(visibleModules.empty()) {
                ImGui::TextDisabled("No features match this search.");
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // The details pane has its own scroll position, so long tool panels remain reachable.
        ImGui::BeginChild("##module_workspace",ImVec2(0,bodyHeight),true);
        if(g_ui.category=="Configs") draw_profiles();
        else if(g_ui.selected) {
            Module* module=g_ui.selected;
            ImGui::TextColored(kAccent,"%s",module->name().c_str());
            if(!module->tooltip().empty()) {
                ImGui::TextWrapped("%s",module->tooltip().c_str());
                ImGui::Spacing();
            }
            const bool enabled=module->enabled();
            if(enabled) {
                ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.865f,0.950f,0.900f,1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.805f,0.925f,0.855f,1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0.105f,0.390f,0.235f,1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,kAccentDim);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.825f,0.895f,0.970f,1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text,kAccent);
            }
            if(ImGui::Button(enabled?"Disable":"Enable",ImVec2(150.0f,30.0f)))
                notify_module_toggle(module,!enabled);
            ImGui::PopStyleColor(3);

            if(module->name()!="SkinStealer") {
                ImGui::Spacing(); ImGui::Separator(); ImGui::TextColored(kMuted,"KEYBIND");
                const bool binding=InputManager::instance().binding_module()==module;
                const std::string keyLabel=binding?"Press a key... (DEL to clear)":
                    (module->keybind()==0?"Key: Not set":std::string("Key: ")+input::get_key_name(module->keybind()));
                if(ImGui::Button(keyLabel.c_str(),ImVec2(210.0f,30.0f))) InputManager::instance().begin_binding(module);
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Press a key to bind. Delete clears the binding; Escape cancels.");
                ImGui::SameLine(); bool hold=module->hold_to_enable();
                if(ImGui::Checkbox("Hold",&hold)) module->set_hold_to_enable(hold);
                if(ImGui::IsItemHovered()) ImGui::SetTooltip("Hold the assigned key to keep this feature enabled.");
            }
            ImGui::Spacing(); ImGui::Separator(); ImGui::TextColored(kMuted,"SETTINGS");
            if(module->has_settings()) module->on_settings_render();
            else ImGui::TextColored(kMuted,"No additional settings.");
        } else {
            ImGui::TextColored(kAccent,"SELECT A FEATURE"); ImGui::Spacing();
            ImGui::TextWrapped("Select a feature to view its description, controls and settings. Use the ON/OFF button in the list for quick toggling.");
            ImGui::Spacing(); ImGui::TextColored(kMuted,"CHANTERCLIENT / Minecraft Bedrock");
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ImGui::TextColored(kMuted,"INSERT: Toggle  |  END: Unload  |  DEL: Clear key  |  ESC: Cancel / Close");
    }
    ImGui::End();
    if(!open) dx12_hook::set_menu_open(false);
}
}
