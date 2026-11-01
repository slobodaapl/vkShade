#ifndef IMGUI_OVERLAY_HPP_INCLUDED
#define IMGUI_OVERLAY_HPP_INCLUDED

#include <vector>
#include <array>
#include <memory>
#include <string>
#include <chrono>
#include <map>
#include <set>
#include <mutex>

#include "vulkan_include.hpp"
#include "logical_device.hpp"
#include "keyboard_input.hpp"
#include "effects/params/effect_param.hpp"
#include "config_serializer.hpp"
#include "settings_manager.hpp"
#include "shader_metadata_loader.hpp"
#include "preprocessor_edit.hpp"
#include "async_texture_upload.hpp"

namespace vkShade
{
    class Effect;
    class EffectRegistry;

    struct DepthOverlayState
    {
        std::vector<LogicalDevice::DepthSourceCandidate> candidates;
        DepthState                                       activeSource;
        DepthState                                       automaticSource;
        DepthState                                       pinnedSource;
        std::optional<size_t>                            automaticClear;
        int                                              sourceIndex           = -1;
        bool                                             sourceFromEnvironment = false;
        bool                                             pinnedSourceDestroyed = false;
        bool                                             captureEnabled        = false;
    };

    struct OverlayState
    {
        std::vector<std::string>           effectNames;          // Effects in current config
        std::vector<std::string>           disabledEffects;      // Effects that are unchecked (in list but not rendered)
        std::vector<std::string>           currentConfigEffects; // ReShade effects from current config (e.g., tunic.conf)
        std::vector<std::string>           defaultConfigEffects; // ReShade effects from default vkShade.conf (no duplicates)
        std::map<std::string, std::string> effectPaths;          // Effect name -> file path (for reshade effects)
        std::map<std::string, std::string> configuredEffectPaths;
        std::vector<std::string>           effectFiles;
        std::vector<std::string>           shaderIncludePaths;
        std::string                        configPath;
        std::string                        baseConfigPath;
        std::string                        configName; // Just the filename (e.g., "tunic.conf")
        bool                               effectsEnabled    = true;
        size_t                             maxTechniqueSlots = 0;
        DepthOverlayState                  depth;
        // Parameters now read directly from EffectRegistry
    };

    struct DepthSourceRequest
    {
        std::optional<int> index;
        DepthState         pinnedSource;
    };

    struct DepthCopyRequest
    {
        bool enabled    = false;
        int  clearIndex = 0;
    };

    // UI preferences that persist across swapchain recreation
    // Effect-related state is managed by EffectRegistry
    // Settings are managed by SettingsManager
    struct OverlayPersistentState
    {
        bool visible = false;
    };

    class ImGuiOverlay
    {
    public:
        ImGuiOverlay(LogicalDevice* device, VkFormat swapchainFormat, uint32_t imageCount, OverlayPersistentState* persistentState);
        ~ImGuiOverlay();

        void toggle();
        bool isVisible() const
        {
            return visible;
        }

        void updateState(OverlayState newState);

        // Returns modified parameters when Apply is clicked, empty otherwise
        std::vector<std::unique_ptr<EffectParam>> getModifiedParams();
        bool                                      hasModifiedParams() const
        {
            return applyRequested;
        }
        void clearApplyRequest()
        {
            applyRequested = false;
        }
        bool refreshDepthDefinitionsFromConfig();
        bool takeDepthDefinitionsChanged();
        void clearDepthDefinitionsChanged()
        {
            depthDefinitionsChanged = false;
        }

        // Config switching
        bool hasPendingConfig() const
        {
            return !pendingConfigPath.empty();
        }
        const std::string& getPendingConfigPath() const
        {
            return pendingConfigPath;
        }
        void clearPendingConfig()
        {
            pendingConfigPath.clear();
        }

        // Effects toggle (global on/off)
        bool hasToggleEffectsRequest() const
        {
            return toggleEffectsRequested;
        }
        void clearToggleEffectsRequest()
        {
            toggleEffectsRequested = false;
        }

        // Set the effect registry (single source of truth for enabled states)
        void setEffectRegistry(EffectRegistry* registry);

        // Set game/profile info for auto-save (called from vkshade.cpp after detection)
        void setGameProfile(const std::string& gameName, const std::string& profileName, const std::string& profilePath)
        {
            activeGameName    = gameName;
            activeProfileName = profileName;
            activeProfilePath = profilePath;
            gameDepthSettings = ConfigSerializer::loadGameDepthSettings(gameName, profilePath);

            // Load per-profile settings
            if (!profilePath.empty())
            {
                ProfileSettings ps         = ConfigSerializer::loadProfileSettings(profilePath);
                techniqueSettings          = ps;
                techniqueSettingsPath      = profilePath;
                profileSafeAntiCheat       = ps.safeAntiCheat;
                profileDepthSourceIndex    = ps.depthSourceIndex;
                profileDepthSourceIndexSet = ps.hasDepthSourceIndex;
                settingsManager.setSafeAntiCheat(profileSafeAntiCheat);
                if (pLogicalDevice)
                {
                    pLogicalDevice->depthSourceOverrideIndex = ps.hasDepthSourceIndex ? std::optional<int>(ps.depthSourceIndex) : std::nullopt;
                    pLogicalDevice->depthCopyBeforeClears    = ps.depthCopyBeforeClears;
                    pLogicalDevice->depthClearIndex          = ps.depthClearIndex;
                }
            }
        }

        std::optional<DepthCopyRequest> takeDepthCopyRequest()
        {
            std::lock_guard<std::mutex> lock(depthStateMutex);
            auto                        request = pendingDepthCopyRequest;
            pendingDepthCopyRequest.reset();
            return request;
        }

        std::optional<DepthSourceRequest> takeDepthSourceRequest()
        {
            std::lock_guard<std::mutex> lock(depthStateMutex);
            auto                        request = pendingDepthSourceRequest;
            pendingDepthSourceRequest.reset();
            return request;
        }

        // Trigger debounced reload (for config switch)
        void markDirty()
        {
            paramsDirty    = true;
            lastChangeTime = std::chrono::steady_clock::now();
        }

        // Settings were saved (keybindings need reload)
        bool hasSettingsSaved() const
        {
            return settingsSaved;
        }
        void clearSettingsSaved()
        {
            settingsSaved = false;
        }

        // Shader paths were changed (effect list needs refresh)
        bool hasShaderPathsChanged() const
        {
            return shaderPathsChanged;
        }
        void clearShaderPathsChanged()
        {
            shaderPathsChanged = false;
        }

        // Returns list of effects that should be active (enabled, for reloading)
        std::vector<std::string> getActiveEffects() const;

        // Returns all selected effects (enabled + disabled, for parameter collection)
        const std::vector<std::string>& getSelectedEffects() const;
        const ProfileSettings&          getTechniqueSettings() const
        {
            return techniqueSettings;
        }
        const std::string& getTechniqueSettingsPath() const
        {
            return techniqueSettingsPath;
        }
        void setTechniqueNotice(const std::string& notice)
        {
            techniqueNotice = notice;
        }
        void invalidateShaderMetadata()
        {
            shaderMetadata.clear();
        }

        // Set effects list (when loading a different config)
        // disabledEffects: effects that should be unchecked (in list but not rendered)
        void setSelectedEffects(const std::vector<std::string>& effects, const std::vector<std::string>& disabledEffects = {});

        VkCommandBuffer recordFrame(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height);
        void            pollPreprocessorEdit();

        // Get fence for command buffer synchronization (used by vkshade.cpp submit)
        VkFence getCommandBufferFence(uint32_t imageIndex) const
        {
            return (imageIndex < commandBufferFences.size()) ? commandBufferFences[imageIndex] : VK_NULL_HANDLE;
        }

    private:
        bool                  saveGameDepthIfNeeded();
        DepthDefinitionValues effectiveDepthDefinitions() const;
        std::set<std::string> effectiveDepthDefinitionConflicts() const;
        void                  initVulkanBackend(VkFormat swapchainFormat, uint32_t imageCount);
        void                  saveToPersistentState();
        void                  saveCurrentConfig();

        // View rendering methods (implemented in separate files)
        void renderConfigManagerView();
        void renderSettingsView(const KeyboardState& keyboard);
        void renderShaderManagerView();
        void renderShaderTestSection(); // Shader test UI (part of shader manager)
        void startShaderTest();         // Initialize shader test queue and start
        void processShaderTest();       // Process one shader per frame (runs every frame)
        void renderMainView(const KeyboardState& keyboard);
        void renderTechniqueList();
        void renderEffectDetails(const std::string& effectName);
        void queuePreprocessorEdit(const std::string& effectName, const PreprocessorDefinition& definition, const std::string& value);
        void renderDiagnosticsView();
        void renderDepthView();
        void renderDebugWindow(); // Debug window with effect registry and log data

        LogicalDevice*               pLogicalDevice;
        OverlayPersistentState*      pPersistentState;
        AsyncTextureUploads          textureUploads;
        EffectRegistry*              pEffectRegistry = nullptr; // Single source of truth for enabled states
        VkDescriptorPool             descriptorPool  = VK_NULL_HANDLE;
        VkRenderPass                 renderPass      = VK_NULL_HANDLE;
        VkCommandPool                commandPool     = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkFence>         commandBufferFences; // Fences to track command buffer completion
        std::array<VkFence, 2>       renderBufferFences{};
        size_t                       renderBufferIndex = 0;
        std::vector<VkFramebuffer>   framebuffers;          // Pre-created per swapchain image
        std::vector<VkImageView>     framebufferImageViews; // Track which image views framebuffers were created for
        VkFormat                     swapchainFormat   = VK_FORMAT_UNDEFINED;
        uint32_t                     imageCount        = 0;
        uint32_t                     framebufferWidth  = 0; // Dimensions used to create framebuffers
        uint32_t                     framebufferHeight = 0;
        OverlayState                 state;
        ShaderMetadataLoader         shaderMetadata;
        PreprocessorEditValidator    preprocessorEdit;
        ProfileSettings              techniqueSettings;
        ProfileSettings              gameDepthSettings;
        DepthDefinitionValues        inheritedDepthDefinitions;
        std::set<std::string>        inheritedDepthDefinitionConflicts;
        std::string                  techniqueSettingsPath;
        std::string                  techniqueNotice;
        DepthOverlayState            depthState;
        std::mutex                   depthStateMutex;
        char                         techniqueSearch[64] = "";
        bool                         inConfigManageMode  = false;
        int                          currentTab          = 0; // 0=Effects, 1=Shaders, 2=Settings, 3=Diagnostics
        std::vector<std::string>     configList;

        // Shader Manager state
        std::vector<std::string> shaderMgrParentDirs;
        std::vector<std::string> shaderMgrShaderPaths;
        std::vector<std::string> shaderMgrTexturePaths;
        bool                     shaderMgrInitialized = false;

        // Shader test state
        bool                                             shaderTestRunning        = false;
        bool                                             shaderTestComplete       = false;
        size_t                                           shaderTestCurrentIndex   = 0;
        int                                              shaderTestDuplicateCount = 0; // Number of duplicate shaders skipped
        std::vector<std::pair<std::string, std::string>> shaderTestQueue;              // {effectName, filePath}
        std::vector<std::string>                         shaderTestIncludePaths;       // Cached include paths (avoid re-reading config per shader)
        std::vector<std::tuple<std::string, std::string, bool, std::string>> shaderTestResults; // {name, path, success, error}
        std::set<std::string> depthShaders;   // Effect names that use depth buffer (populated by shader test)
        std::set<std::string> checkedShaders; // Effects already checked for depth (avoids recompiling)

        // UI state for settings view
        int    listeningForKey    = 0;     // 0=none, 1=toggle, 2=reload, 3=overlay
        bool   settingsSaved      = false; // True when settings saved, cleared by vkshade.cpp
        bool   shaderPathsChanged = false; // True when shader manager saved, cleared by vkshade.cpp
        size_t maxEffects         = 10;    // Cached from settingsManager for VRAM estimates

        // UI state for debug window
        int                                   debugWindowTab          = 0;                                // 0=Registry, 1=Log
        bool                                  debugLogFilters[5]      = {false, false, true, true, true}; // Trace, Debug, Info, Warn, Error
        char                                  debugLogSearch[128]     = "";                               // Search filter for log tab
        bool                                  applyRequested          = false;
        bool                                  toggleEffectsRequested  = false;
        bool                                  paramsDirty             = false; // True when params changed, waiting for debounce
        bool                                  depthDefinitionsChanged = false;
        bool                                  gameDepthNeedsSave      = false;
        std::string                           depthDefinitionNotice;
        std::chrono::steady_clock::time_point lastChangeTime;
        bool                                  visible              = false;
        bool                                  initialized          = false;
        bool                                  backendInitialized   = false;
        bool                                  resetLayoutRequested = false; // When true, reset window position/size next frame
        uint32_t                              currentWidth         = 1920;  // Current swapchain resolution for VRAM estimates
        uint32_t                              currentHeight        = 1080;
        char                                  saveConfigName[64]   = "";
        std::string                           pendingConfigPath;

        // Per-app profile system
        std::string                       activeGameName;                     // Detected game identity
        std::string                       activeProfileName;                  // Active profile ("default", "performance", etc.)
        std::string                       activeProfilePath;                  // Full path to active profile file
        bool                              profileDirty               = false; // True when changes need saving
        bool                              profileSafeAntiCheat       = false; // Per-profile: force-disable depth capture
        int                               profileDepthSourceIndex    = -1;
        bool                              profileDepthSourceIndexSet = false;
        std::optional<DepthSourceRequest> pendingDepthSourceRequest;
        std::optional<DepthCopyRequest>   pendingDepthCopyRequest;

        void requestDepthSource(std::optional<int> index, const DepthState& pinnedSource = {})
        {
            std::lock_guard<std::mutex> lock(depthStateMutex);
            pendingDepthSourceRequest = DepthSourceRequest{index, pinnedSource};
        }

        void requestDepthCopy()
        {
            std::lock_guard<std::mutex> lock(depthStateMutex);
            pendingDepthCopyRequest = DepthCopyRequest{techniqueSettings.depthCopyBeforeClears, techniqueSettings.depthClearIndex};
        }

        void disableDepthEffects(); // Disable effects that use depth buffer
        void autoSaveProfile();     // Save current state to active profile
        void collectSaveData(       // Shared helper for save operations
            std::vector<std::string>&            effects,
            std::vector<std::string>&            disabledEffects,
            std::vector<ConfigParam>&            params,
            std::map<std::string, std::string>&  effectPaths,
            std::vector<PreprocessorDefinition>& allDefs,
            std::set<std::string>&               unresolvedEffects);
    };

} // namespace vkShade

#endif // IMGUI_OVERLAY_HPP_INCLUDED
