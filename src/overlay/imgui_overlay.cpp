#include "imgui_overlay.hpp"
#include "technique_id.hpp"
#include "depth_selection.hpp"
#include "effects/effect_registry.hpp"
#include "settings_manager.hpp"
#include "reshade_parser.hpp"
#include "logger.hpp"
#include "mouse_input.hpp"
#include "mouse_input_wayland.hpp"
#include "keyboard_input.hpp"
#include "input_blocker.hpp"
#include "config_serializer.hpp"
#include "util.hpp"
#include "wayland_display.hpp"
#include "wayland_pointer_constraints.hpp"
#include "wayland_input_common.hpp"
#include "wayland_interpose.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_vulkan.h"

namespace vkShade
{
    VKAPI_ATTR VkResult VKAPI_CALL vkShade_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence);

    static std::mutex                                   imguiDeviceMutex;
    static std::unordered_map<VkDevice, LogicalDevice*> imguiDevices;

    static VkResult VKAPI_CALL imguiAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers)
    {
        std::lock_guard<std::mutex> lock(imguiDeviceMutex);
        const auto                  it = imguiDevices.find(device);
        if (it == imguiDevices.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        const auto result = it->second->vkd.AllocateCommandBuffers(device, info, buffers);
        if (result == VK_SUCCESS)
            for (uint32_t i = 0; i < info->commandBufferCount; ++i)
                initializeDispatchTable(buffers[i], device);
        return result;
    }

    // No-op dummy for Vulkan functions ImGui requests but vkShade doesn't intercept.
    // ImGui's LoadFunctions treats nullptr returns as failures, so we need a valid pointer.
    static void VKAPI_CALL dummyVulkanFunc()
    {
    }

    // Function loader using vkShade's dispatch tables
    static PFN_vkVoidFunction imguiVulkanLoaderDummy(const char* function_name, void* user_data)
    {
        LogicalDevice* device = static_cast<LogicalDevice*>(user_data);

// Device functions from vkShade's dispatch table
#define CHECK_FUNC(name) \
    if (strcmp(function_name, "vk" #name) == 0) \
    return (PFN_vkVoidFunction) device->vkd.name

        if (strcmp(function_name, "vkAllocateCommandBuffers") == 0)
            return reinterpret_cast<PFN_vkVoidFunction>(imguiAllocateCommandBuffers);
        CHECK_FUNC(AllocateDescriptorSets);
        CHECK_FUNC(AllocateMemory);
        CHECK_FUNC(BeginCommandBuffer);
        CHECK_FUNC(BindBufferMemory);
        CHECK_FUNC(BindImageMemory);
        CHECK_FUNC(CmdBeginRenderPass);
        CHECK_FUNC(CmdBindDescriptorSets);
        CHECK_FUNC(CmdBindIndexBuffer);
        CHECK_FUNC(CmdBindPipeline);
        CHECK_FUNC(CmdBindVertexBuffers);
        CHECK_FUNC(CmdCopyBufferToImage);
        CHECK_FUNC(CmdDrawIndexed);
        CHECK_FUNC(CmdEndRenderPass);
        CHECK_FUNC(CmdPipelineBarrier);
        CHECK_FUNC(CmdPushConstants);
        CHECK_FUNC(CmdSetScissor);
        CHECK_FUNC(CmdSetViewport);
        CHECK_FUNC(CreateBuffer);
        CHECK_FUNC(CreateCommandPool);
        CHECK_FUNC(CreateDescriptorPool);
        CHECK_FUNC(CreateDescriptorSetLayout);
        CHECK_FUNC(CreateFence);
        CHECK_FUNC(CreateFramebuffer);
        CHECK_FUNC(CreateGraphicsPipelines);
        CHECK_FUNC(CreateImage);
        CHECK_FUNC(CreateImageView);
        CHECK_FUNC(CreatePipelineLayout);
        CHECK_FUNC(CreateRenderPass);
        CHECK_FUNC(CreateSampler);
        CHECK_FUNC(CreateSemaphore);
        CHECK_FUNC(CreateShaderModule);
        CHECK_FUNC(CreateSwapchainKHR);
        CHECK_FUNC(DestroyBuffer);
        CHECK_FUNC(DestroyCommandPool);
        CHECK_FUNC(DestroyDescriptorPool);
        CHECK_FUNC(DestroyDescriptorSetLayout);
        CHECK_FUNC(DestroyFence);
        CHECK_FUNC(DestroyFramebuffer);
        CHECK_FUNC(DestroyImage);
        CHECK_FUNC(DestroyImageView);
        CHECK_FUNC(DestroyPipeline);
        CHECK_FUNC(DestroyPipelineLayout);
        CHECK_FUNC(DestroyRenderPass);
        CHECK_FUNC(DestroySampler);
        CHECK_FUNC(DestroySemaphore);
        CHECK_FUNC(DestroyShaderModule);
        CHECK_FUNC(DestroySwapchainKHR);
        CHECK_FUNC(EndCommandBuffer);
        CHECK_FUNC(FlushMappedMemoryRanges);
        CHECK_FUNC(FreeCommandBuffers);
        CHECK_FUNC(FreeDescriptorSets);
        CHECK_FUNC(FreeMemory);
        CHECK_FUNC(GetBufferMemoryRequirements);
        CHECK_FUNC(GetDeviceQueue);
        CHECK_FUNC(GetImageMemoryRequirements);
        CHECK_FUNC(GetSwapchainImagesKHR);
        CHECK_FUNC(MapMemory);
        CHECK_FUNC(QueueSubmit);
        CHECK_FUNC(QueueWaitIdle);
        CHECK_FUNC(ResetCommandPool);
        CHECK_FUNC(ResetFences);
        CHECK_FUNC(UnmapMemory);
        CHECK_FUNC(UpdateDescriptorSets);
        CHECK_FUNC(WaitForFences);

#undef CHECK_FUNC

// Instance functions from vkShade's dispatch
#define CHECK_IFUNC(name) \
    if (strcmp(function_name, "vk" #name) == 0) \
    return (PFN_vkVoidFunction) device->vki.name
        CHECK_IFUNC(GetPhysicalDeviceMemoryProperties);
        CHECK_IFUNC(GetPhysicalDeviceProperties);
        CHECK_IFUNC(GetPhysicalDeviceQueueFamilyProperties);
#undef CHECK_IFUNC

        // Return a no-op dummy for unknown functions. ImGui's LoadFunctions treats
        // nullptr as a load failure, so we must return a valid function pointer.
        // These functions are never actually called by ImGui in our usage.
        return (PFN_vkVoidFunction) dummyVulkanFunc;
    }

    ImGuiOverlay::ImGuiOverlay(LogicalDevice* device, VkFormat swapchainFormat, uint32_t imageCount, OverlayPersistentState* persistentState) :
        pLogicalDevice(device), pPersistentState(persistentState), textureUploads(device->device, device->queue, device->vkd, vkShade_QueueSubmit)
    {
        {
            std::lock_guard<std::mutex> lock(imguiDeviceMutex);
            imguiDevices[device->device] = device;
        }
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io                          = ImGui::GetIO();
        io.IniFilename                       = nullptr;
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        std::string   iniPath = ConfigSerializer::getBaseConfigDir() + "/imgui.ini";
        std::ifstream iniFile(iniPath);
        std::string   iniContent((std::istreambuf_iterator<char>(iniFile)), std::istreambuf_iterator<char>());

        if (!iniContent.empty())
            ImGui::LoadIniSettingsFromDisk(iniPath.c_str());

        ImGui::StyleColorsDark();

        // Make it semi-transparent
        ImGuiStyle& style    = ImGui::GetStyle();
        style.Alpha          = 0.9f;
        style.WindowRounding = 5.0f;

        initVulkanBackend(swapchainFormat, imageCount);

        // Restore UI preferences from persistent state
        if (pPersistentState)
            visible = pPersistentState->visible;
        setWaylandOverlayMouseActive(visible);
        setInputBlocked(visible);

        initialized = true;
        Logger::info("ImGui overlay initialized");
    }

    ImGuiOverlay::~ImGuiOverlay()
    {
        {
            std::lock_guard<std::mutex> lock(imguiDeviceMutex);
            imguiDevices.erase(pLogicalDevice->device);
        }
        setWaylandOverlayMouseActive(false);
        if (!initialized)
            return;

        // Auto-save profile on shutdown (before GPU cleanup)
        if (profileDirty && !activeProfilePath.empty())
            autoSaveProfile();

        pLogicalDevice->vkd.QueueWaitIdle(pLogicalDevice->queue);
        textureUploads.clearAfterIdle();

        // Clean up Wayland resources before destroying the event queue
        if (isWayland())
            cleanupPointerConstraints();

        std::string iniPath = ConfigSerializer::getBaseConfigDir() + "/imgui.ini";
        ImGui::SaveIniSettingsToDisk(iniPath.c_str());

        if (backendInitialized)
            ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();

        for (auto fb : framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, fb, nullptr);
        }
        for (auto fence : commandBufferFences)
        {
            if (fence != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyFence(pLogicalDevice->device, fence, nullptr);
        }
        if (commandPool != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyCommandPool(pLogicalDevice->device, commandPool, nullptr);
        if (renderPass != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyRenderPass(pLogicalDevice->device, renderPass, nullptr);
        if (descriptorPool != VK_NULL_HANDLE)
            pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, descriptorPool, nullptr);

        Logger::info("ImGui overlay destroyed");
    }

    void ImGuiOverlay::setEffectRegistry(EffectRegistry* registry)
    {
        pEffectRegistry                   = registry;
        Config* config                    = registry ? registry->getConfig() : nullptr;
        auto    inherited                 = ConfigSerializer::loadEffectiveDepthDefinitions(config ? config->getFallback() : nullptr);
        inheritedDepthDefinitions         = std::move(inherited.values);
        inheritedDepthDefinitionConflicts = std::move(inherited.conflicts);
    }

    DepthDefinitionValues ImGuiOverlay::effectiveDepthDefinitions() const
    {
        auto values = inheritedDepthDefinitions;
        for (const auto& name : gameDepthSettings.conflictingDepthDefinitions)
            if (!gameDepthSettings.depthDefinitions.count(name))
                values.erase(name);
        for (const auto& [name, value] : gameDepthSettings.depthDefinitions)
            values[name] = value;
        return values;
    }

    std::set<std::string> ImGuiOverlay::effectiveDepthDefinitionConflicts() const
    {
        auto conflicts = inheritedDepthDefinitionConflicts;
        for (const auto& entry : gameDepthSettings.depthDefinitions)
            conflicts.erase(entry.first);
        conflicts.insert(gameDepthSettings.conflictingDepthDefinitions.begin(), gameDepthSettings.conflictingDepthDefinitions.end());
        return conflicts;
    }

    bool ImGuiOverlay::refreshDepthDefinitionsFromConfig()
    {
        if (!pEffectRegistry || profileDirty || paramsDirty || !pendingConfigPath.empty())
            return false;
        Config* config = pEffectRegistry->getConfig();
        if (!config)
            return false;
        auto settings                                 = ConfigSerializer::loadGameDepthSettings(activeGameName, config->getConfigFilePath());
        auto inherited                                = ConfigSerializer::loadEffectiveDepthDefinitions(config->getFallback());
        inheritedDepthDefinitions                     = std::move(inherited.values);
        inheritedDepthDefinitionConflicts             = std::move(inherited.conflicts);
        gameDepthSettings.hasDepthDefinitions         = settings.hasDepthDefinitions;
        gameDepthSettings.depthDefinitions            = std::move(settings.depthDefinitions);
        gameDepthSettings.conflictingDepthDefinitions = std::move(settings.conflictingDepthDefinitions);
        auto       values                             = effectiveDepthDefinitions();
        const bool changed                            = pEffectRegistry->getDepthDefinitions() != values;
        if (changed)
        {
            pEffectRegistry->setDepthDefinitions(std::move(values));
            shaderMetadata.clear();
        }
        return changed;
    }

    bool ImGuiOverlay::takeDepthDefinitionsChanged()
    {
        if (profileDirty || paramsDirty)
            return false;
        const bool changed      = depthDefinitionsChanged;
        depthDefinitionsChanged = false;
        return changed;
    }

    void ImGuiOverlay::toggle()
    {
        visible = !visible;
        setWaylandOverlayMouseActive(visible);
        setInputBlocked(visible);

        // Pointer confinement was removed: the overlay renders inside the game's
        // surface, so confining the pointer doesn't prevent the compositor from
        // interpreting clicks as window-move grabs.

        saveToPersistentState();
    }

    void ImGuiOverlay::saveToPersistentState()
    {
        if (!pPersistentState)
            return;

        // Only save UI preferences - effect state is in the registry, settings in settingsManager
        pPersistentState->visible = visible;
    }

    void ImGuiOverlay::updateState(OverlayState newState)
    {
        if (pendingConfigPath.empty() && techniqueSettingsPath != newState.configPath)
        {
            techniqueSettings     = ConfigSerializer::loadProfileSettings(newState.configPath);
            techniqueSettingsPath = newState.configPath;
            if (!gameDepthNeedsSave)
                gameDepthSettings = ConfigSerializer::loadGameDepthSettings(activeGameName, newState.configPath);
            shaderMetadata.clear();
        }
        else if (state.effectFiles != newState.effectFiles || state.shaderIncludePaths != newState.shaderIncludePaths)
            shaderMetadata.clear();
        {
            std::lock_guard<std::mutex> lock(depthStateMutex);
            depthState = std::move(newState.depth);
        }
        state = std::move(newState);

        if (!pEffectRegistry)
            return;

        // Registry is already initialized from config at swapchain creation
        // Just ensure any newly added effects are in the registry
        const auto& selectedEffects = pEffectRegistry->getSelectedEffects();
        for (const auto& effectName : selectedEffects)
        {
            if (!pEffectRegistry->hasEffect(effectName))
                pEffectRegistry->ensureEffect(effectName);
        }
        // No editableParams merging needed - Registry IS the source of truth

        // If Safe Anti-Cheat is active, disable any depth-using effects in the selection
        if (profileSafeAntiCheat)
            disableDepthEffects();
    }

    void ImGuiOverlay::disableDepthEffects()
    {
        if (!pEffectRegistry)
            return;

        const auto& selectedEffects = pEffectRegistry->getSelectedEffects();

        for (const auto& effectName : selectedEffects)
        {
            auto path = pEffectRegistry->getEffectFilePath(effectName);
            if (path.empty())
            {
                auto it = state.effectPaths.find(effectName);
                if (it != state.effectPaths.end())
                    path = it->second;
            }
            if (path.empty())
                continue;

            bool usesDepth = depthShaders.count(effectName) > 0;
            if (!usesDepth && !shaderTestComplete)
            {
                if (checkShaderUsesDepth(effectName, path, state.shaderIncludePaths))
                {
                    depthShaders.insert(effectName);
                    usesDepth = true;
                }
            }
            if (usesDepth && pEffectRegistry->isEffectEnabled(effectName))
            {
                pEffectRegistry->setEffectEnabled(effectName, false);
                auto& enabled = techniqueSettings.enabledTechniques;
                enabled.erase(std::remove_if(enabled.begin(),
                                             enabled.end(),
                                             [&](const std::string& entry) {
                                                 auto id = decodeTechniqueId(entry);
                                                 return id && id->owner == TechniqueOwner::Instance && id->effect == effectName;
                                             }),
                              enabled.end());
                profileDirty = true;
                Logger::info("Safe Anti-Cheat: disabled depth effect '" + effectName + "'");
            }
        }
    }

    std::vector<std::unique_ptr<EffectParam>> ImGuiOverlay::getModifiedParams()
    {
        if (!pEffectRegistry)
            return {};
        return pEffectRegistry->getAllParameters();
    }

    std::vector<std::string> ImGuiOverlay::getActiveEffects() const
    {
        std::vector<std::string> activeEffects;
        if (!pEffectRegistry)
            return activeEffects;

        for (const auto& effectName : pEffectRegistry->getSelectedEffects())
        {
            if (pEffectRegistry->isEffectEnabled(effectName))
                activeEffects.push_back(effectName);
        }
        return activeEffects;
    }

    const std::vector<std::string>& ImGuiOverlay::getSelectedEffects() const
    {
        static std::vector<std::string> empty;
        return pEffectRegistry ? pEffectRegistry->getSelectedEffects() : empty;
    }

    void ImGuiOverlay::collectSaveData(std::vector<std::string>&            effects,
                                       std::vector<std::string>&            disabledEffects,
                                       std::vector<ConfigParam>&            params,
                                       std::map<std::string, std::string>&  effectPaths,
                                       std::vector<PreprocessorDefinition>& allDefs,
                                       std::set<std::string>&               unresolvedEffects)
    {
        if (!pEffectRegistry)
            return;

        effects                = pEffectRegistry->getSelectedEffects();
        const auto oldDisabled = pEffectRegistry->getConfig()->getOption<std::vector<std::string>>("disabledEffects", {});

        for (const auto& effectName : effects)
        {
            for (auto* p : pEffectRegistry->getParametersForEffect(effectName))
            {
                if (!p->hasChanged())
                    continue;

                auto serialized = p->serialize();
                for (const auto& [suffix, value] : serialized)
                {
                    ConfigParam cp;
                    cp.effectName = p->effectName;
                    cp.paramName  = suffix.empty() ? p->name : suffix;
                    cp.value      = value;
                    params.push_back(cp);
                }
            }

            const bool registered = pEffectRegistry->hasEffect(effectName);
            bool       enabled    = pEffectRegistry->isEffectEnabled(effectName);
            if (!registered)
            {
                unresolvedEffects.insert(effectName);
                enabled = std::find(oldDisabled.begin(), oldDisabled.end(), effectName) == oldDisabled.end();
                if (techniqueSettings.hasEnabledTechniques)
                {
                    enabled = false;
                    for (const auto& entry : techniqueSettings.enabledTechniques)
                    {
                        auto id = decodeTechniqueId(entry);
                        if (id && id->owner != TechniqueOwner::File && id->effect == effectName)
                            enabled = true;
                    }
                }
            }
            if (!enabled)
                disabledEffects.push_back(effectName);

            std::string path = pEffectRegistry->getEffectFilePath(effectName);
            if (path.empty())
            {
                auto found = state.effectPaths.find(effectName);
                if (found != state.effectPaths.end())
                    path = found->second;
            }
            if (!path.empty())
                effectPaths[effectName] = path;

            const auto& defs = pEffectRegistry->getPreprocessorDefs(effectName);
            for (const auto& def : defs)
                allDefs.push_back(def);
        }
    }

    bool ImGuiOverlay::saveGameDepthIfNeeded()
    {
        if (activeGameName.empty())
            return true;
        const auto      path = ConfigSerializer::getGameDepthPath(activeGameName);
        std::error_code error;
        const bool      exists = !path.empty() && std::filesystem::exists(path, error);
        if (error || path.empty())
            return false;
        if (gameDepthNeedsSave || !exists)
        {
            if (!ConfigSerializer::saveGameDepthSettings(activeGameName, effectiveDepthDefinitions(), effectiveDepthDefinitionConflicts()))
            {
                depthDefinitionNotice = "Could not save shared depth settings.";
                return false;
            }
            gameDepthNeedsSave = false;
        }
        return true;
    }

    void ImGuiOverlay::saveCurrentConfig()
    {
        if (!pEffectRegistry)
            return;
        if (!saveGameDepthIfNeeded())
            return;

        std::vector<std::string>            effects, disabledEffects;
        std::vector<ConfigParam>            params;
        std::map<std::string, std::string>  effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        std::set<std::string>               unresolvedEffects;
        collectSaveData(effects, disabledEffects, params, effectPaths, allDefs, unresolvedEffects);

        auto savedSettings = techniqueSettings;
        if (!activeGameName.empty())
        {
            savedSettings.hasDepthDefinitions = true;
            savedSettings.depthDefinitions.clear();
            savedSettings.conflictingDepthDefinitions = effectiveDepthDefinitionConflicts();
        }
        if (ConfigSerializer::saveConfig(
                saveConfigName, effects, disabledEffects, params, effectPaths, allDefs, savedSettings, unresolvedEffects, state.configPath))
            profileDirty = false;
    }

    void ImGuiOverlay::autoSaveProfile()
    {
        if (!pEffectRegistry || activeProfilePath.empty())
            return;
        if (!saveGameDepthIfNeeded())
            return;

        std::vector<std::string>            effects, disabledEffects;
        std::vector<ConfigParam>            params;
        std::map<std::string, std::string>  effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        std::set<std::string>               unresolvedEffects;
        collectSaveData(effects, disabledEffects, params, effectPaths, allDefs, unresolvedEffects);

        ProfileSettings profileSettings     = techniqueSettings;
        profileSettings.safeAntiCheat       = profileSafeAntiCheat;
        profileSettings.depthSourceIndex    = profileDepthSourceIndex;
        profileSettings.hasDepthSourceIndex = profileDepthSourceIndexSet;
        if (!activeGameName.empty())
        {
            profileSettings.hasDepthDefinitions = true;
            profileSettings.depthDefinitions.clear();
            profileSettings.conflictingDepthDefinitions = effectiveDepthDefinitionConflicts();
        }

        if (ConfigSerializer::saveToPath(
                activeProfilePath, effects, disabledEffects, params, effectPaths, allDefs, profileSettings, unresolvedEffects))
        {
            profileDirty = false;
            if (depthDefinitionsChanged && !paramsDirty)
                applyRequested = true;
            Logger::debug("Auto-saved profile: " + activeProfilePath);
        }
    }

    void ImGuiOverlay::setSelectedEffects(const std::vector<std::string>& effects, const std::vector<std::string>& disabledEffects)
    {
        if (!pEffectRegistry)
            return;

        pEffectRegistry->setSelectedEffects(effects);

        // Build set of disabled effects for quick lookup
        std::set<std::string> disabledSet(disabledEffects.begin(), disabledEffects.end());

        // Set enabled states in registry: disabled if in disabledEffects, enabled otherwise
        for (const auto& effectName : effects)
        {
            bool enabled = (disabledSet.find(effectName) == disabledSet.end());
            pEffectRegistry->setEffectEnabled(effectName, enabled);
        }
    }

    static const char* depthFormatName(VkFormat format)
    {
        switch (format)
        {
            case VK_FORMAT_D16_UNORM: return "D16";
            case VK_FORMAT_X8_D24_UNORM_PACK32: return "D24";
            case VK_FORMAT_D32_SFLOAT: return "D32";
            case VK_FORMAT_D16_UNORM_S8_UINT: return "D16S8";
            case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
            case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32S8";
            default: return "Unknown";
        }
    }

    void ImGuiOverlay::renderDepthView()
    {
        DepthOverlayState depth;
        {
            std::lock_guard<std::mutex> lock(depthStateMutex);
            depth = depthState;
        }
        if (profileSafeAntiCheat)
        {
            ImGui::TextWrapped("Depth capture is disabled by Safe Anti-Cheat for this profile.");
            return;
        }
        if (!depth.captureEnabled)
        {
            ImGui::TextWrapped("Depth capture is off. Enable it in Settings and restart the application.");
            return;
        }

        if (ImGui::CollapsingHeader("Depth input settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::TextDisabled("Shared by all effects and profiles for this game.");
            auto depthValues        = effectiveDepthDefinitions();
            auto conflicts          = effectiveDepthDefinitionConflicts();
            auto setDepthDefinition = [this, &depthValues, &conflicts](const DepthDefinition& definition, const std::string& value) {
                const std::string name(definition.name);
                if (depthDefinitionValue(depthValues, definition) == value && !conflicts.count(name))
                    return;
                gameDepthSettings.hasDepthDefinitions    = true;
                gameDepthSettings.depthDefinitions[name] = value;
                depthValues[name]                        = value;
                gameDepthSettings.conflictingDepthDefinitions.erase(name);
                conflicts.erase(name);
                pEffectRegistry->setDepthDefinitions(depthValues);
                depthDefinitionsChanged = true;
                gameDepthNeedsSave      = true;
                depthDefinitionNotice.clear();
                paramsDirty    = true;
                profileDirty   = true;
                lastChangeTime = std::chrono::steady_clock::now();
            };

            ImGui::BeginDisabled(preprocessorEdit.pending());
            if (!conflicts.empty())
            {
                ImGui::TextWrapped("Conflicting old per-effect depth settings were kept. Choose shared values to resolve them.");
                for (const auto& name : conflicts)
                    ImGui::TextDisabled("%s", name.c_str());
                if (ImGui::Button("Use displayed values"))
                {
                    for (const auto& definition : depthDefinitions)
                    {
                        const std::string name(definition.name);
                        if (conflicts.count(name))
                            gameDepthSettings.depthDefinitions[name] = depthDefinitionValue(depthValues, definition);
                    }
                    gameDepthSettings.hasDepthDefinitions = true;
                    gameDepthSettings.conflictingDepthDefinitions.clear();
                    pEffectRegistry->setDepthDefinitions(effectiveDepthDefinitions());
                    depthDefinitionsChanged = true;
                    gameDepthNeedsSave      = true;
                    paramsDirty             = true;
                    profileDirty            = true;
                    lastChangeTime          = std::chrono::steady_clock::now();
                }
            }

            static constexpr std::array labels = {"Upside down",
                                                  "Reversed",
                                                  "Mirrored",
                                                  "Logarithmic",
                                                  "X scale",
                                                  "Y scale",
                                                  "X offset",
                                                  "Y offset",
                                                  "X pixel offset",
                                                  "Y pixel offset",
                                                  "Far plane",
                                                  "Depth multiplier"};
            static_assert(labels.size() == depthDefinitions.size());
            for (size_t i = 0; i < depthDefinitions.size(); ++i)
            {
                if (i == 4 && !ImGui::TreeNode("Advanced depth settings"))
                    break;
                if (i < 4 && i % 2 != 0)
                    ImGui::SameLine();
                const auto& definition = depthDefinitions[i];
                const auto  value      = depthDefinitionValue(depthValues, definition);
                ImGui::PushID(static_cast<int>(i));
                if (auto edited = renderDepthDefinitionInput(definition, labels[i], value, false, depthDefinitionNotice))
                    setDepthDefinition(definition, *edited);
                ImGui::PopID();
                if (i + 1 == depthDefinitions.size())
                    ImGui::TreePop();
            }
            ImGui::EndDisabled();
            if (!depthDefinitionNotice.empty())
                ImGui::TextWrapped("%s", depthDefinitionNotice.c_str());
            if (!settingsManager.getAutoApply() && paramsDirty && ImGui::Button("Apply depth settings"))
            {
                applyRequested = true;
                paramsDirty    = false;
                profileDirty   = true;
            }
        }

        const auto& candidates        = depth.candidates;
        const int   selectedIndex     = depth.sourceIndex;
        auto        selectedCandidate = resolveDepthSource(candidates,
                                                           selectedIndex,
                                                           depth.sourceFromEnvironment ? DepthState{} : depth.pinnedSource,
                                                           !depth.sourceFromEnvironment && depth.pinnedSourceDestroyed);
        if (selectedIndex == DEPTH_SOURCE_AUTO)
            for (size_t i = 0; i < candidates.size(); ++i)
                if (sameDepthSource(candidates[i].depthState, depth.activeSource))
                {
                    selectedCandidate = i;
                    break;
                }
        bool trackDrawRank = selectedIndex <= DEPTH_SOURCE_DRAW_RANK_BASE;

        auto select = [this](int index, const DepthState& source = DepthState{}) {
            profileDepthSourceIndex    = index;
            profileDepthSourceIndexSet = true;
            requestDepthSource(index, source);
            profileDirty = true;
        };

        if (ImGui::Checkbox("Copy depth before clear operations", &techniqueSettings.depthCopyBeforeClears))
        {
            requestDepthCopy();
            profileDirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Preserve depth before a selected clear. Capture starts when the game records that command buffer again.");

        if (depth.sourceFromEnvironment)
            ImGui::TextWrapped("Selection is controlled by VKSHADE_DEPTH_SOURCE_INDEX.");
        ImGui::BeginDisabled(depth.sourceFromEnvironment);
        if (ImGui::RadioButton("Automatic", selectedIndex == DEPTH_SOURCE_AUTO))
            select(DEPTH_SOURCE_AUTO);

        if (ImGui::Checkbox("Track draw rank", &trackDrawRank))
        {
            if (trackDrawRank)
            {
                const int rank = selectedCandidate ? static_cast<int>(depthSourceDrawRank(candidates, *selectedCandidate)) : 0;
                select(DEPTH_SOURCE_DRAW_RANK_BASE - rank);
            }
            else if (selectedCandidate)
                select(static_cast<int>(*selectedCandidate), candidates[*selectedCandidate].depthState);
            else
                select(DEPTH_SOURCE_AUTO);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Follow a draw-call rank instead of a specific image. The image can change as the ranking changes.");
        ImGui::EndDisabled();

        ImGui::Separator();
        if (selectedIndex != DEPTH_SOURCE_AUTO && !selectedCandidate)
        {
            if (depth.pinnedSourceDestroyed && !depth.sourceFromEnvironment)
                ImGui::TextWrapped("The selected depth image was destroyed. Select another image or choose Automatic.");
            else
                ImGui::TextWrapped("The selected depth source is not available in the current candidate list.");
        }
        if (std::none_of(candidates.begin(), candidates.end(), [](const auto& candidate) { return candidate.depthState.image != VK_NULL_HANDLE; }))
        {
            ImGui::TextWrapped("No depth buffers with submitted work observed.");
            return;
        }

        ImGui::TextDisabled("Draws in last active frame");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Indirect-count commands report their maximum possible draw count.");
        std::vector<size_t> order(candidates.size());
        std::iota(order.begin(), order.end(), size_t{0});
        std::sort(order.begin(), order.end(), [&candidates](size_t lhs, size_t rhs) {
            const auto& left  = candidates[lhs].depthState;
            const auto& right = candidates[rhs].depthState;
            if (left.extent.width != right.extent.width)
                return left.extent.width > right.extent.width;
            if (left.extent.height != right.extent.height)
                return left.extent.height > right.extent.height;
            if (left.sourceId != right.sourceId)
                return left.sourceId < right.sourceId;
            return lhs < rhs;
        });
        ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("DepthSourceTable", 7, tableFlags, ImVec2(0, 0)))
        {
            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn("Index");
            ImGui::TableSetupColumn("Image");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Format");
            ImGui::TableSetupColumn("Draws");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();

            for (size_t i : order)
            {
                const auto& candidate = candidates[i];
                if (candidate.depthState.image == VK_NULL_HANDLE)
                    continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const std::string sourceId = std::to_string(candidate.depthState.sourceId);
                ImGui::PushID(sourceId.c_str());
                ImGui::BeginDisabled(depth.sourceFromEnvironment || !candidate.depthState.capturable || (trackDrawRank && candidate.stale));
                if (ImGui::RadioButton("##select", selectedIndex != DEPTH_SOURCE_AUTO && selectedCandidate && *selectedCandidate == i))
                {
                    if (trackDrawRank)
                        select(DEPTH_SOURCE_DRAW_RANK_BASE - static_cast<int>(depthSourceDrawRank(candidates, i)));
                    else
                        select(static_cast<int>(i), candidate.depthState);
                }
                if (ImGui::IsItemHovered() && !trackDrawRank)
                    ImGui::SetTooltip(candidate.stale
                                          ? "Select this image when the game uses it again. The saved index may change after restart."
                                          : "Follow this image for this session. The saved index may refer to another image after restart.");
                ImGui::EndDisabled();
                ImGui::PopID();

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%zu", i);
                ImGui::TableSetColumnIndex(2);
                const std::string image = convertToString(candidate.depthState.image);
                ImGui::TextUnformatted(image.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("View: %s", convertToString(candidate.depthState.imageView).c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%ux%u", candidate.depthState.extent.width, candidate.depthState.extent.height);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(depthFormatName(candidate.depthState.format));
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%llu", static_cast<unsigned long long>(candidate.stats.drawcalls) + candidate.stats.drawcallsIndirect);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Vertices / indices: %llu\nDirect: %u\nIndirect: %u",
                                      static_cast<unsigned long long>(candidate.stats.vertices),
                                      candidate.stats.drawcalls,
                                      candidate.stats.drawcallsIndirect);
                ImGui::TableSetColumnIndex(6);
                const bool  active    = sameDepthSourceLayout(candidate.depthState, depth.activeSource);
                const bool  automatic = sameDepthSourceLayout(candidate.depthState, depth.automaticSource);
                const char* status    = !candidate.depthState.capturable
                                            ? (candidate.depthState.samples != VK_SAMPLE_COUNT_1_BIT ? "Multisampled" : "Unsupported")
                                        : candidate.stale     ? "Not used this frame"
                                        : active && automatic ? "Active / Candidate"
                                        : active              ? "Active"
                                        : automatic           ? "Candidate"
                                                              : "";
                ImGui::TextUnformatted(status);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(!candidate.depthState.capturable
                                          ? "This image or its view cannot be copied for depth effects."
                                          : "Active is the selected source. Candidate is the current automatic candidate.");

                if (techniqueSettings.depthCopyBeforeClears && selectedCandidate && *selectedCandidate == i)
                {
                    const auto    selectedClear = techniqueSettings.depthClearIndex == 0 && sameDepthSource(candidate.depthState, depth.activeSource)
                                                      ? depth.automaticClear
                                                      : selectDepthClear(candidate.clears, techniqueSettings.depthClearIndex);
                    const int64_t manualClear   = techniqueSettings.depthClearIndex > 0
                                                      ? static_cast<int64_t>(techniqueSettings.depthClearIndex) - 1
                                                      : static_cast<int64_t>(candidate.clears.size()) + techniqueSettings.depthClearIndex;
                    auto          selectClear   = [this](int index) {
                        techniqueSettings.depthClearIndex = index;
                        requestDepthCopy();
                        profileDirty = true;
                    };
                    ImGui::PushID(sourceId.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::RadioButton("##auto-clear", techniqueSettings.depthClearIndex == 0))
                        selectClear(0);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted("  Automatic clear");
                    for (size_t clearIndex = 0; clearIndex < candidate.clears.size(); ++clearIndex)
                    {
                        const auto& clear    = candidate.clears[clearIndex];
                        const bool  selected = selectedClear && *selectedClear == clearIndex;
                        ImGui::PushID(static_cast<int>(clearIndex));
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::BeginDisabled(!clear.copyPossible);
                        if (ImGui::RadioButton("##clear", techniqueSettings.depthClearIndex != 0 && manualClear == static_cast<int64_t>(clearIndex)))
                            selectClear(static_cast<int>(clearIndex) + 1);
                        ImGui::EndDisabled();
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("  Clear %zu", clearIndex + 1);
                        ImGui::TableSetColumnIndex(5);
                        ImGui::Text("%llu", static_cast<unsigned long long>(clear.stats.drawcalls) + clear.stats.drawcallsIndirect);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Draws since previous clear\nVertices / indices: %llu\nDirect: %u\nIndirect: %u",
                                              static_cast<unsigned long long>(clear.stats.vertices),
                                              clear.stats.drawcalls,
                                              clear.stats.drawcallsIndirect);
                        ImGui::TableSetColumnIndex(6);
                        ImGui::TextUnformatted(!clear.copyPossible ? "Unavailable" : selected ? (clear.copied ? "Copied" : "Waiting") : "Available");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(!clear.copyPossible ? "Requires a known pre-clear layout and a primary command buffer in the graphics "
                                                                    "queue family. Reused buffers require one graphics queue that also presents. "
                                                                    "Clears inside a render pass cannot be copied."
                                              : selected && !clear.copied ? "Waiting for the game to record this clear again."
                                                                          : "Statistics cover draws since the previous clear.");
                        ImGui::PopID();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    void ImGuiOverlay::initVulkanBackend(VkFormat swapchainFormat, uint32_t imageCount)
    {
        // Load Vulkan functions for ImGui using vkShade's dispatch tables
        bool loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imguiVulkanLoaderDummy, pLogicalDevice);
        if (!loaded)
        {
            Logger::err("Failed to load Vulkan functions for ImGui");
            return;
        }
        Logger::debug("ImGui Vulkan functions loaded");

        // Create descriptor pool for ImGui
        VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100}};

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType                      = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags                      = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets                    = 100;
        poolInfo.poolSizeCount              = 1;
        poolInfo.pPoolSizes                 = poolSizes;

        VkResult vr = pLogicalDevice->vkd.CreateDescriptorPool(pLogicalDevice->device, &poolInfo, nullptr, &descriptorPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui descriptor pool: " + std::to_string(vr));
            return;
        }

        // Create render pass for ImGui
        VkAttachmentDescription attachment = {};
        attachment.format                  = swapchainFormat;
        attachment.samples                 = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp                  = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp                 = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp           = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp          = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout             = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef = {};
        colorRef.attachment            = 0;
        colorRef.layout                = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        // Dependency for incoming: wait for prior color writes before overlay renders
        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass          = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass          = 0;
        dependencies[0].srcStageMask        = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstStageMask        = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask       = 0;
        dependencies[0].dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Dependency for outgoing: ensure overlay writes + layout transition complete
        // and are VISIBLE to presentation engine and DMA-BUF screen capture readers.
        // VK_ACCESS_MEMORY_READ_BIT is critical — without it, GPU caches may not be
        // flushed before PipeWire/compositor reads the image via DMA-BUF.
        dependencies[1].srcSubpass    = 0;
        dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount        = 1;
        renderPassInfo.pAttachments           = &attachment;
        renderPassInfo.subpassCount           = 1;
        renderPassInfo.pSubpasses             = &subpass;
        renderPassInfo.dependencyCount        = 2;
        renderPassInfo.pDependencies          = dependencies;

        vr = pLogicalDevice->vkd.CreateRenderPass(pLogicalDevice->device, &renderPassInfo, nullptr, &renderPass);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui render pass: " + std::to_string(vr));
            return;
        }

        // Initialize ImGui Vulkan backend
        ImGui_ImplVulkan_InitInfo initInfo   = {};
        initInfo.ApiVersion                  = VK_API_VERSION_1_3;
        initInfo.Instance                    = pLogicalDevice->instance;
        initInfo.PhysicalDevice              = pLogicalDevice->physicalDevice;
        initInfo.Device                      = pLogicalDevice->device;
        initInfo.QueueFamily                 = pLogicalDevice->queueFamilyIndex;
        initInfo.Queue                       = pLogicalDevice->queue;
        initInfo.DescriptorPool              = descriptorPool;
        initInfo.MinImageCount               = 2;
        initInfo.ImageCount                  = 2;
        initInfo.PipelineInfoMain.RenderPass = renderPass;
        initInfo.TextureUploadUserData       = this;
        initInfo.SubmitTextureUploadFn =
            [](VkCommandPool pool, VkCommandBuffer buffer, VkBuffer staging, VkDeviceMemory memory, ImTextureData* texture, void* userData) {
                return static_cast<ImGuiOverlay*>(userData)->textureUploads.submit(pool, buffer, staging, memory, texture);
            };
        initInfo.CanDestroyTextureFn = [](ImTextureData* texture, void* userData) {
            return !static_cast<ImGuiOverlay*>(userData)->textureUploads.pending(texture);
        };

        ImGui_ImplVulkan_Init(&initInfo);

        this->swapchainFormat        = swapchainFormat;
        this->imageCount             = imageCount;
        bool commandBuffersAllocated = false;

        auto rollbackBackendInit = [&]() {
            for (auto fence : commandBufferFences)
            {
                if (fence != VK_NULL_HANDLE)
                    pLogicalDevice->vkd.DestroyFence(pLogicalDevice->device, fence, nullptr);
            }
            commandBufferFences.clear();

            if (commandBuffersAllocated && !commandBuffers.empty() && commandPool != VK_NULL_HANDLE)
            {
                pLogicalDevice->vkd.FreeCommandBuffers(
                    pLogicalDevice->device, commandPool, static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
            }
            commandBuffers.clear();

            for (auto fb : framebuffers)
            {
                if (fb != VK_NULL_HANDLE)
                    pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, fb, nullptr);
            }
            framebuffers.clear();
            framebufferImageViews.clear();

            if (commandPool != VK_NULL_HANDLE)
            {
                pLogicalDevice->vkd.DestroyCommandPool(pLogicalDevice->device, commandPool, nullptr);
                commandPool = VK_NULL_HANDLE;
            }
            if (renderPass != VK_NULL_HANDLE)
            {
                pLogicalDevice->vkd.DestroyRenderPass(pLogicalDevice->device, renderPass, nullptr);
                renderPass = VK_NULL_HANDLE;
            }
            if (descriptorPool != VK_NULL_HANDLE)
            {
                pLogicalDevice->vkd.DestroyDescriptorPool(pLogicalDevice->device, descriptorPool, nullptr);
                descriptorPool = VK_NULL_HANDLE;
            }

            ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
        };

        // Create command pool
        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCreateInfo.queueFamilyIndex        = pLogicalDevice->queueFamilyIndex;
        vr = pLogicalDevice->vkd.CreateCommandPool(pLogicalDevice->device, &poolCreateInfo, nullptr, &commandPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui command pool: " + std::to_string(vr));
            rollbackBackendInit();
            return;
        }

        // Allocate command buffers
        commandBuffers.resize(imageCount);
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType                       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool                 = commandPool;
        allocInfo.level                       = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount          = imageCount;
        vr                                    = pLogicalDevice->vkd.AllocateCommandBuffers(pLogicalDevice->device, &allocInfo, commandBuffers.data());
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to allocate ImGui command buffers: " + std::to_string(vr));
            rollbackBackendInit();
            return;
        }
        commandBuffersAllocated = true;
        for (auto commandBuffer : commandBuffers)
            initializeDispatchTable(commandBuffer, pLogicalDevice->device);

        // Create fences for command buffer synchronization (signaled initially so first frame doesn't wait)
        commandBufferFences.resize(imageCount);
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags             = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < imageCount; i++)
        {
            vr = pLogicalDevice->vkd.CreateFence(pLogicalDevice->device, &fenceInfo, nullptr, &commandBufferFences[i]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create ImGui fence " + std::to_string(i) + ": " + std::to_string(vr));
                rollbackBackendInit();
                return;
            }
        }

        backendInitialized = true;
        Logger::debug("ImGui Vulkan backend initialized");
    }

    VkCommandBuffer ImGuiOverlay::recordFrame(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height)
    {
        if (!backendInitialized || !visible)
            return VK_NULL_HANDLE;

        const bool blockOverlayInput = visible && settingsManager.getOverlayBlockInput();
        setInputBlocked(blockOverlayInput);

        // Store current resolution for VRAM estimates in settings
        currentWidth  = width;
        currentHeight = height;

        // Wait for previous use of this command buffer to complete.
        // Adaptive timeout: 4x the measured frame time (generous margin for GPU
        // scheduling jitter), clamped to [2ms, 50ms].  With triple buffering the
        // fence was submitted 2-3 frames ago and is almost always already signaled.
        static float avgFrameTimeMs = 8.0f; // Seed at ~120 FPS
        VkFence      fence          = commandBufferFences[imageIndex];
        uint64_t     timeoutNs      = static_cast<uint64_t>(std::clamp(avgFrameTimeMs * 4.0f, 2.0f, 50.0f) * 1'000'000.0f);
        VkResult     fenceResult    = pLogicalDevice->vkd.WaitForFences(pLogicalDevice->device, 1, &fence, VK_TRUE, timeoutNs);
        if (fenceResult == VK_TIMEOUT)
        {
            Logger::warn("ImGui fence wait timed out for image " + std::to_string(imageIndex));
            return VK_NULL_HANDLE;
        }
        if (fenceResult != VK_SUCCESS)
        {
            Logger::err("ImGui fence wait failed: " + std::to_string(fenceResult));
            return VK_NULL_HANDLE;
        }
        const size_t  nextRenderBuffer = (renderBufferIndex + 1) % renderBufferFences.size();
        const VkFence renderFence      = renderBufferFences[nextRenderBuffer];
        if (renderFence != VK_NULL_HANDLE && renderFence != fence)
        {
            fenceResult = pLogicalDevice->vkd.WaitForFences(pLogicalDevice->device, 1, &renderFence, VK_TRUE, timeoutNs);
            if (fenceResult != VK_SUCCESS)
            {
                if (fenceResult != VK_TIMEOUT)
                    Logger::err("ImGui render buffer fence wait failed: " + std::to_string(fenceResult));
                return VK_NULL_HANDLE;
            }
        }

        const VkResult uploadResult = textureUploads.collect();
        if (uploadResult != VK_SUCCESS)
            Logger::err("ImGui texture upload fence failed: " + std::to_string(uploadResult));

        VkCommandBuffer cmd = commandBuffers[imageIndex];

        // Begin command buffer
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vr                        = pLogicalDevice->vkd.BeginCommandBuffer(cmd, &beginInfo);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to begin ImGui command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        // Ensure framebuffer exists for this image index.
        // Recreate if the image view or dimensions changed (swapchain recreation).
        if (framebuffers.size() <= imageIndex)
        {
            framebuffers.resize(imageIndex + 1, VK_NULL_HANDLE);
            framebufferImageViews.resize(imageIndex + 1, VK_NULL_HANDLE);
        }

        bool needRecreate = (framebuffers[imageIndex] == VK_NULL_HANDLE) || (framebufferImageViews[imageIndex] != imageView)
                            || (framebufferWidth != width) || (framebufferHeight != height);

        if (needRecreate)
        {
            if (framebuffers[imageIndex] != VK_NULL_HANDLE)
                pLogicalDevice->vkd.DestroyFramebuffer(pLogicalDevice->device, framebuffers[imageIndex], nullptr);

            VkFramebufferCreateInfo fbInfo = {};
            fbInfo.sType                   = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass              = renderPass;
            fbInfo.attachmentCount         = 1;
            fbInfo.pAttachments            = &imageView;
            fbInfo.width                   = width;
            fbInfo.height                  = height;
            fbInfo.layers                  = 1;
            vr = pLogicalDevice->vkd.CreateFramebuffer(pLogicalDevice->device, &fbInfo, nullptr, &framebuffers[imageIndex]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create ImGui framebuffer: " + std::to_string(vr));
                pLogicalDevice->vkd.EndCommandBuffer(cmd);
                return VK_NULL_HANDLE;
            }
            framebufferImageViews[imageIndex] = imageView;
            framebufferWidth                  = width;
            framebufferHeight                 = height;
        }

        VkFramebuffer framebuffer = framebuffers[imageIndex];

        // Set logical display size and frame timing BEFORE NewFrame
        const MouseState mouse     = getMouseState(width, height);
        ImGuiIO&         io        = ImGui::GetIO();
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(mouse.framebufferScaleX), static_cast<float>(mouse.framebufferScaleY));
        const auto logicalExtent   = [](uint32_t extent, float scale) {
            const float size = static_cast<float>(extent) / scale;
            return size * scale < static_cast<float>(extent) ? std::nextafter(size, std::numeric_limits<float>::infinity()) : size;
        };
        io.DisplaySize     = ImVec2(logicalExtent(width, io.DisplayFramebufferScale.x), logicalExtent(height, io.DisplayFramebufferScale.y));
        const int fontSize = settingsManager.getOverlayFontSize();
        ImGui::GetStyle().FontSizeBase = static_cast<float>(fontSize == AUTO_OVERLAY_FONT_SIZE ? DEFAULT_OVERLAY_FONT_SIZE : fontSize);

        // DeltaTime for ImGui's internal timing (drag thresholds, animations).
        // Uses last known good value as fallback instead of hardcoded 1/60.
        static auto  lastFrameTime = std::chrono::steady_clock::now();
        static float lastGoodDt    = 0.008f; // Seed at ~120 FPS
        auto         now           = std::chrono::steady_clock::now();
        float        dt            = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime              = now;
        if (dt > 0.0f && dt < 1.0f)
        {
            io.DeltaTime = dt;
            lastGoodDt   = dt;
            // Update adaptive fence timeout with exponential moving average
            avgFrameTimeMs = avgFrameTimeMs * 0.9f + (dt * 1000.0f) * 0.1f;
        }
        else
        {
            io.DeltaTime = lastGoodDt;
        }

        // Mouse input for interactivity
        io.MousePos = ImVec2(static_cast<float>(mouse.x / io.DisplayFramebufferScale.x), static_cast<float>(mouse.y / io.DisplayFramebufferScale.y));

        io.MouseDown[0]    = mouse.leftButton;
        io.MouseDown[1]    = mouse.rightButton;
        io.MouseDown[2]    = mouse.middleButton;
        io.MouseWheel      = mouse.scrollDelta;
        io.MouseDrawCursor = isWayland() && (isWaylandGamePointerLocked() || isWaylandGameCursorHidden());

        // Keyboard input for text fields
        // Keys are one-shot events, so we send press and release in same frame
        KeyboardState keyboard = getKeyboardState();
        for (char c : keyboard.typedChars)
            io.AddInputCharacter(c);
        if (keyboard.backspace)
        {
            io.AddKeyEvent(ImGuiKey_Backspace, true);
            io.AddKeyEvent(ImGuiKey_Backspace, false);
        }
        if (keyboard.del)
        {
            io.AddKeyEvent(ImGuiKey_Delete, true);
            io.AddKeyEvent(ImGuiKey_Delete, false);
        }
        if (keyboard.enter)
        {
            io.AddKeyEvent(ImGuiKey_Enter, true);
            io.AddKeyEvent(ImGuiKey_Enter, false);
        }
        if (keyboard.left)
        {
            io.AddKeyEvent(ImGuiKey_LeftArrow, true);
            io.AddKeyEvent(ImGuiKey_LeftArrow, false);
        }
        if (keyboard.right)
        {
            io.AddKeyEvent(ImGuiKey_RightArrow, true);
            io.AddKeyEvent(ImGuiKey_RightArrow, false);
        }
        if (keyboard.home)
        {
            io.AddKeyEvent(ImGuiKey_Home, true);
            io.AddKeyEvent(ImGuiKey_Home, false);
        }
        if (keyboard.end)
        {
            io.AddKeyEvent(ImGuiKey_End, true);
            io.AddKeyEvent(ImGuiKey_End, false);
        }

        // ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        // Clamp overlay window to screen bounds so it can never go offscreen
        ImGuiViewport* viewport  = ImGui::GetMainViewport();
        ImVec2         screenMin = viewport->WorkPos;
        ImVec2         screenMax = ImVec2(screenMin.x + viewport->WorkSize.x, screenMin.y + viewport->WorkSize.y);

        // Set default size/position on first appearance
        ImGui::SetNextWindowSizeConstraints(ImVec2(300, 200), ImVec2(screenMax.x - screenMin.x, screenMax.y - screenMin.y));
        if (resetLayoutRequested)
        {
            ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_Always);
            resetLayoutRequested = false;
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);
        }

        ImGui::Begin("vkShade Overlay", nullptr, ImGuiWindowFlags_NoCollapse);

        // Clamp position after the window is created (prevents dragging offscreen)
        ImVec2 winPos  = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        bool   clamped = false;
        if (winPos.x + winSize.x < screenMin.x + 50)
        {
            winPos.x = screenMin.x;
            clamped  = true;
        }
        if (winPos.y < screenMin.y)
        {
            winPos.y = screenMin.y;
            clamped  = true;
        }
        if (winPos.x > screenMax.x - 50)
        {
            winPos.x = screenMax.x - 50;
            clamped  = true;
        }
        if (winPos.y > screenMax.y - 30)
        {
            winPos.y = screenMax.y - 30;
            clamped  = true;
        }
        if (clamped)
            ImGui::SetWindowPos(winPos);

        // Process shader test (one per frame) regardless of active tab
        processShaderTest();

        if (ImGui::BeginTabBar("OverlayTabs"))
        {
            if (ImGui::BeginTabItem("Effects"))
            {
                if (inConfigManageMode)
                    renderConfigManagerView();
                else
                    renderMainView(keyboard);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Shaders"))
            {
                renderShaderManagerView();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings"))
            {
                renderSettingsView(keyboard);
                ImGui::EndTabItem();
            }

            if (settingsManager.isDepthCaptureActive() && ImGui::BeginTabItem("Depth"))
            {
                renderDepthView();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Diagnostics"))
            {
                renderDiagnosticsView();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End(); // vkShade Overlay

        // Debug window (separate, controlled by setting)
        renderDebugWindow();

        // Global auto-apply check (runs regardless of which tab is active)
        if (settingsManager.getAutoApply() && paramsDirty)
        {
            auto       now           = std::chrono::steady_clock::now();
            auto       elapsed       = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChangeTime).count();
            const bool leftMouseDown = ImGui::GetIO().MouseDown[0];

            // Defer auto-apply while left mouse is held (dragging/resizing/sliders).
            // Apply on the first frame after release once delay has already elapsed.
            if (elapsed >= settingsManager.getAutoApplyDelay() && !leftMouseDown)
            {
                applyRequested = true;
                paramsDirty    = false;
                profileDirty   = true; // Mark for auto-save to profile
            }
        }

        // Auto-save profile when changes are applied
        if (profileDirty && !paramsDirty && !activeProfilePath.empty())
            autoSaveProfile();

        // Focus Effects window on first frame of the session
        static bool firstFrame = true;
        if (firstFrame)
        {
            ImGui::SetWindowFocus("Effects");
            firstFrame = false;
        }

        ImGui::Render();

        // Begin render pass
        VkRenderPassBeginInfo rpBegin    = {};
        rpBegin.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass               = renderPass;
        rpBegin.framebuffer              = framebuffer;
        rpBegin.renderArea.extent.width  = width;
        rpBegin.renderArea.extent.height = height;

        pLogicalDevice->vkd.CmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        renderBufferIndex                    = nextRenderBuffer;
        renderBufferFences[nextRenderBuffer] = VK_NULL_HANDLE;
        pLogicalDevice->vkd.CmdEndRenderPass(cmd);

        vr = pLogicalDevice->vkd.EndCommandBuffer(cmd);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to end ImGui command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        VkResult resetResult = pLogicalDevice->vkd.ResetFences(pLogicalDevice->device, 1, &fence);
        if (resetResult != VK_SUCCESS)
        {
            Logger::err("Failed to reset ImGui fence: " + std::to_string(resetResult));
            return VK_NULL_HANDLE;
        }
        renderBufferFences[nextRenderBuffer] = fence;

        return cmd;
    }

} // namespace vkShade
