#include "imgui_overlay.hpp"
#include "effects/effect_registry.hpp"
#include "settings_manager.hpp"
#include "config_serializer.hpp"
#include "params/field_editor.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

namespace vkShade
{
    void ImGuiOverlay::renderMainView(const KeyboardState& keyboard)
    {
        if (!pEffectRegistry)
            return;

        // Normal mode - show profile and effect controls

        // Profile section — auto-detected game with profile selector
        if (!activeGameName.empty())
        {
            auto gameLabel = std::filesystem::path(activeGameName).filename().string();
            if (activeGameName.starts_with("steam/"))
                gameLabel = "Steam " + gameLabel;
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", gameLabel.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            // Profile dropdown
            static std::vector<std::string> profileList;
            static bool                     profileListStale = true;
            if (profileListStale)
            {
                profileList      = ConfigSerializer::listProfilesForGame(activeGameName);
                profileListStale = false;
            }

            ImGui::Text("Profile:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            if (ImGui::BeginCombo("##profile", activeProfileName.c_str()))
            {
                for (const auto& profile : profileList)
                {
                    bool selected = (profile == activeProfileName);
                    if (ImGui::Selectable(profile.c_str(), selected))
                    {
                        if (profile != activeProfileName)
                        {
                            // Save current profile before switching
                            if (profileDirty)
                            {
                                autoSaveProfile();
                                if (profileDirty)
                                {
                                    techniqueNotice = "Could not save the current profile.";
                                    continue;
                                }
                            }

                            // Switch to new profile
                            activeProfileName = profile;
                            activeProfilePath = ConfigSerializer::getProfilePath(activeGameName, profile);
                            ConfigSerializer::setActiveProfile(activeGameName, profile);
                            pendingConfigPath = activeProfilePath;
                            applyRequested    = true;
                            profileListStale  = true;

                            // Load per-profile settings for the new profile
                            ProfileSettings ps    = ConfigSerializer::loadProfileSettings(activeProfilePath);
                            techniqueSettings     = ps;
                            techniqueSettingsPath = activeProfilePath;
                            shaderMetadata.clear();
                            profileSafeAntiCheat       = ps.safeAntiCheat;
                            profileDepthSourceIndex    = ps.depthSourceIndex;
                            profileDepthSourceIndexSet = ps.hasDepthSourceIndex;
                            requestDepthSource(ps.hasDepthSourceIndex ? std::optional<int>(ps.depthSourceIndex) : std::nullopt);
                            requestDepthCopy();
                            settingsManager.setSafeAntiCheat(profileSafeAntiCheat);
                            if (profileSafeAntiCheat)
                            {
                                settingsManager.setDepthCapture(false);
                                // Depth effects will be disabled after reload completes
                                // (the new config hasn't loaded yet — pendingConfigPath triggers reload)
                            }
                        }
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();
            if (ImGui::Button("+##newprofile"))
                ImGui::OpenPopup("NewProfilePopup");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Create new profile");

            // Only allow deleting non-default profiles
            if (activeProfileName != "default")
            {
                ImGui::SameLine();
                if (ImGui::Button("-##delprofile"))
                    ImGui::OpenPopup("DeleteProfilePopup");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Delete this profile");
            }

            // New profile popup
            if (ImGui::BeginPopup("NewProfilePopup"))
            {
                static char newProfileName[64] = "";
                ImGui::Text("New profile name:");
                ImGui::SetNextItemWidth(150);
                ImGui::InputText("##newprofname", newProfileName, sizeof(newProfileName));
                ImGui::SameLine();
                ImGui::BeginDisabled(newProfileName[0] == '\0');
                if (ImGui::Button("Create"))
                {
                    if (profileDirty)
                        autoSaveProfile();
                    if (profileDirty)
                        techniqueNotice = "Could not save the current profile.";
                    if (!profileDirty && ConfigSerializer::createProfile(activeGameName, newProfileName, activeProfileName))
                    {
                        activeProfileName = newProfileName;
                        activeProfilePath = ConfigSerializer::getProfilePath(activeGameName, newProfileName);
                        ConfigSerializer::setActiveProfile(activeGameName, newProfileName);
                        techniqueSettingsPath = activeProfilePath;
                        pendingConfigPath     = activeProfilePath;
                        applyRequested        = true;
                        shaderMetadata.clear();
                        profileListStale  = true;
                        newProfileName[0] = '\0';
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }

            // Delete profile confirmation
            if (ImGui::BeginPopup("DeleteProfilePopup"))
            {
                ImGui::Text("Delete profile '%s'?", activeProfileName.c_str());
                if (ImGui::Button("Yes, delete"))
                {
                    ConfigSerializer::deleteProfile(activeGameName, activeProfileName);
                    activeProfileName = "default";
                    profileDirty      = false;
                    paramsDirty       = false;
                    activeProfilePath = ConfigSerializer::getProfilePath(activeGameName, "default");
                    ConfigSerializer::setActiveProfile(activeGameName, "default");
                    const auto ps              = ConfigSerializer::loadProfileSettings(activeProfilePath);
                    techniqueSettings          = ps;
                    techniqueSettingsPath      = activeProfilePath;
                    profileSafeAntiCheat       = ps.safeAntiCheat;
                    profileDepthSourceIndex    = ps.depthSourceIndex;
                    profileDepthSourceIndexSet = ps.hasDepthSourceIndex;
                    requestDepthSource(ps.hasDepthSourceIndex ? std::optional<int>(ps.depthSourceIndex) : std::nullopt);
                    requestDepthCopy();
                    settingsManager.setSafeAntiCheat(profileSafeAntiCheat);
                    if (profileSafeAntiCheat)
                        settingsManager.setDepthCapture(false);
                    shaderMetadata.clear();
                    pendingConfigPath = activeProfilePath;
                    applyRequested    = true;
                    profileListStale  = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        else
        {
            // Fallback: legacy config UI for unknown executables
            ImGui::Text("Config:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            ImGui::InputText("##configname", saveConfigName, sizeof(saveConfigName));
            ImGui::SameLine();
            ImGui::BeginDisabled(saveConfigName[0] == '\0');
            if (ImGui::Button("Save"))
                saveCurrentConfig();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("..."))
                inConfigManageMode = true;
        }
        if (!pendingConfigPath.empty())
        {
            ImGui::TextDisabled("Loading profile...");
            return;
        }
        ImGui::Separator();

        bool effectsOn = state.effectsEnabled;
        if (ImGui::Checkbox(effectsOn ? "Effects ON" : "Effects OFF", &effectsOn))
            toggleEffectsRequested = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Toggle effects (%s)", settingsManager.getToggleKey().c_str());
        ImGui::SameLine();
        if (ImGui::Button("Disable all"))
        {
            techniqueSettings.enabledTechniques.clear();
            for (const auto& effect : pEffectRegistry->getSelectedEffects())
                pEffectRegistry->setEffectEnabled(effect, false);
            paramsDirty    = true;
            profileDirty   = true;
            lastChangeTime = std::chrono::steady_clock::now();
        }
        ImGui::Separator();

        float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().ItemSpacing.y;
        ImGui::BeginChild("EffectList", ImVec2(0, -footerHeight), false);
        renderTechniqueList();
        ImGui::EndChild();

        ImGui::Separator();
        bool autoApplyVal = settingsManager.getAutoApply();
        if (ImGui::Checkbox("Apply automatically", &autoApplyVal))
        {
            settingsManager.setAutoApply(autoApplyVal);
            settingsManager.save();
        }
        float applyWidth = ImGui::CalcTextSize("Apply").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowWidth() - applyWidth - ImGui::GetStyle().WindowPadding.x);

        // Apply button is always clickable
        if (ImGui::Button("Apply"))
        {
            applyRequested = true;
            paramsDirty    = false;
            profileDirty   = true; // Mark for auto-save to profile
        }
        // Note: Auto-apply is handled globally in imgui_overlay.cpp
    }

} // namespace vkShade
