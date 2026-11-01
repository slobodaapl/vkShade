#ifndef LOGICAL_DEVICE_HPP_INCLUDED
#define LOGICAL_DEVICE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <chrono>
#include <unordered_map>

#include "vulkan_include.hpp"
#include "vkdispatch.hpp"

namespace vkShade
{
    struct LogicalSwapchain; // Forward declaration
    struct LogicalDevice;

    struct DepthBackup
    {
        LogicalDevice* device = nullptr;
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat       format = VK_FORMAT_UNDEFINED;
        VkExtent3D     extent = {0, 0, 1};
        ~DepthBackup();
    };

    struct DepthState
    {
        VkImageView           imageView      = VK_NULL_HANDLE;
        VkImage               image          = VK_NULL_HANDLE;
        uint64_t              sourceId       = 0;
        VkFormat              format         = VK_FORMAT_UNDEFINED;
        VkExtent3D            extent         = {0, 0, 1};
        VkImageLayout         observedLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkSampleCountFlagBits samples        = VK_SAMPLE_COUNT_1_BIT;
        bool                  capturable     = true;
    };

    struct DepthSnapshotTarget
    {
        VkSwapchainKHR    swapchain         = VK_NULL_HANDLE;
        LogicalSwapchain* pLogicalSwapchain = nullptr;
        uint32_t          imageIndex        = 0;
    };

    struct DepthDrawStats
    {
        uint64_t vertices          = 0;
        uint32_t drawcalls         = 0;
        uint32_t drawcallsIndirect = 0;
    };

    enum class DepthClearKind
    {
        RenderPassLoad,
        DynamicRenderingLoad,
        Attachment,
        Image,
    };

    struct DepthClearCandidate
    {
        DepthDrawStats               stats;
        VkCommandBuffer              commandBuffer        = VK_NULL_HANDLE;
        uint32_t                     ordinal              = 0;
        DepthClearKind               kind                 = DepthClearKind::Image;
        bool                         copyPossible         = false;
        bool                         copied               = false;
        bool                         requiresPresentQueue = false;
        std::shared_ptr<DepthBackup> backup;
    };

    struct DepthSourceCandidate
    {
        DepthState                       depthState;
        DepthDrawStats                   stats;
        std::vector<DepthClearCandidate> clears;
        bool                             stale = false;
    };

    struct AutomaticDepthSelection
    {
        uint64_t                              selectedId   = 0;
        uint64_t                              challengerId = 0;
        std::chrono::steady_clock::time_point challengerSince{};
    };

    struct DepthFrameCadence
    {
        std::chrono::steady_clock::time_point lowWorkSince{};
        bool                                  lowWorkPending = false;
    };

    class DepthFrameTracker
    {
    public:
        void                              begin(VkCommandBuffer commandBuffer);
        void                              forget(VkCommandBuffer commandBuffer);
        void                              forgetSource(uint64_t sourceId);
        void                              forgetView(VkImageView imageView);
        void                              reset();
        void                              draw(VkCommandBuffer commandBuffer, const DepthState& depth, const DepthDrawStats& stats);
        void                              layout(VkCommandBuffer commandBuffer, const DepthState& depth);
        void                              clear(VkCommandBuffer              commandBuffer,
                                                const DepthState&            depth,
                                                DepthClearKind               kind,
                                                bool                         copyPossible,
                                                std::shared_ptr<DepthBackup> backup               = {},
                                                bool                         requiresPresentQueue = false);
        uint32_t                          nextClearOrdinal(VkCommandBuffer commandBuffer, uint64_t sourceId) const;
        void                              execute(VkCommandBuffer primary, VkCommandBuffer secondary, const DepthState* inheritedDepth = nullptr);
        void                              submit(VkCommandBuffer commandBuffer);
        std::vector<DepthSourceCandidate> finishFrame();

    private:
        struct Event
        {
            DepthState          depth;
            DepthDrawStats      stats;
            DepthClearCandidate clear;
            bool                isClear  = false;
            bool                isLayout = false;
        };
        struct FrameSource
        {
            DepthSourceCandidate candidate;
            DepthDrawStats       sinceClear;
        };
        std::unordered_map<VkCommandBuffer, std::vector<Event>>                     recorded;
        std::unordered_map<VkCommandBuffer, std::unordered_map<uint64_t, uint32_t>> clearOrdinals;
        std::vector<FrameSource>                                                    frame;
        std::unordered_map<uint64_t, size_t>                                        frameIndex;
    };

    struct DepthImageMetadata
    {
        VkImageUsageFlags     usage       = 0;
        VkSampleCountFlagBits samples     = VK_SAMPLE_COUNT_1_BIT;
        VkImageTiling         tiling      = VK_IMAGE_TILING_OPTIMAL;
        uint64_t              sourceId    = 0;
        uint32_t              mipLevels   = 1;
        uint32_t              arrayLayers = 1;
        bool                  capturable  = false;
    };

    struct RenderPassMetadata
    {
        std::vector<VkImageLayout>      attachmentInitialLayouts;
        std::vector<VkImageLayout>      attachmentFinalLayouts;
        std::vector<VkAttachmentLoadOp> attachmentLoadOps;
        uint32_t                        depthAttachmentIndex = 0xFFFFFFFFu;
        std::vector<uint32_t>           subpassDepthAttachments;
        std::vector<uint32_t>           attachmentFirstUse;
    };

    struct OverlayPersistentState; // Forward declaration
    class ImGuiOverlay;            // Forward declaration

    struct LogicalDevice
    {
        // Per-command-buffer running totals of draws / vertices attributed
        // to the currently-active dynamic-rendering or render-pass scope.
        using DrawStats            = DepthDrawStats;
        using DepthSourceCandidate = vkShade::DepthSourceCandidate;

        struct DepthScopeTrackingState
        {
            bool                inRenderScope = false;
            DepthState          depthState;
            DepthSnapshotTarget snapshotTarget;
            DrawStats           stats;
        };

        struct DepthCandidateTrackingState
        {
            bool                valid = false;
            DepthState          depthState;
            DepthSnapshotTarget snapshotTarget;
            bool                hasPresentableSnapshotTarget   = false;
            bool                extentMatchesPresentableTarget = false;
            DrawStats           stats;
        };

        struct RenderPassTrackingState
        {
            VkRenderPass            renderPass = VK_NULL_HANDLE;
            uint32_t                subpass    = 0;
            std::vector<DepthState> attachments;
            DepthSnapshotTarget     target;
        };

        struct CommandPoolTrackingState
        {
            uint32_t                 family = 0;
            VkCommandPoolCreateFlags flags  = 0;
        };

        DeviceDispatch                                                  vkd;
        InstanceDispatch                                                vki;
        VkDevice                                                        device;
        VkPhysicalDevice                                                physicalDevice;
        VkPhysicalDeviceFeatures                                        enabledFeatures{};
        VkInstance                                                      instance;
        VkQueue                                                         queue;
        uint32_t                                                        queueFamilyIndex         = 0;
        bool                                                            singleGraphicsQueue      = false;
        bool                                                            presentUsesGraphicsQueue = false;
        VkCommandPool                                                   commandPool;
        bool                                                            supportsMutableFormat           = false;
        bool                                                            isNvidiaGpu                     = false;
        bool                                                            gpuCrashDiagnosticsEnabled      = false;
        bool                                                            supportsNvDiagnosticCheckpoints = false;
        bool                                                            supportsNvDiagnosticsConfig     = false;
        bool                                                            supportsDeviceFaultExt          = false;
        std::vector<VkImage>                                            depthImages;
        std::vector<VkFormat>                                           depthFormats;
        std::vector<VkImageView>                                        depthImageViews;
        std::unordered_map<VkImage, VkExtent3D>                         depthImageExtents;
        std::unordered_map<VkImage, DepthImageMetadata>                 depthImageMetadata;
        uint64_t                                                        nextDepthSourceId = 0;
        std::unordered_map<VkImageView, DepthState>                     depthViewStates;
        std::unordered_map<VkRenderPass, RenderPassMetadata>            renderPassMetadata;
        std::unordered_map<VkImageView, DepthSnapshotTarget>            snapshotTargetViewStates;
        std::unordered_map<VkFramebuffer, DepthState>                   framebufferDepthStates;
        std::unordered_map<VkFramebuffer, std::vector<VkImageView>>     framebufferAttachments;
        std::unordered_map<VkFramebuffer, DepthSnapshotTarget>          framebufferSnapshotTargets;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState>    commandBufferDepthStates;
        std::unordered_map<VkCommandBuffer, RenderPassTrackingState>    commandBufferRenderPasses;
        std::unordered_map<VkCommandBuffer, DepthSnapshotTarget>        commandBufferColorOnlySnapshotTargets;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState>    pendingTransferLinkedDepthScopes;
        std::unordered_map<VkCommandBuffer, DrawStats>                  commandBufferRecordedDraws;
        std::unordered_map<VkCommandBuffer, VkCommandBufferLevel>       commandBufferLevels;
        std::unordered_map<VkCommandPool, std::vector<VkCommandBuffer>> commandPoolBuffers;
        std::unordered_map<VkCommandPool, CommandPoolTrackingState>     commandPoolFamilies;
        std::unordered_map<VkCommandBuffer, VkCommandPool>              commandBufferPools;
        std::unordered_map<VkCommandBuffer, VkCommandBufferUsageFlags>  commandBufferUsage;
        std::vector<DepthCandidateTrackingState>                        frameDepthCandidates;
        DepthFrameTracker                                               depthFrameTracker;
        std::vector<DepthSourceCandidate>                               depthSourceCandidates;
        AutomaticDepthSelection                                         automaticDepthSelection;
        AutomaticDepthSelection                                         automaticClearSelection;
        DepthFrameCadence                                               depthFrameCadence;

        DepthCandidateTrackingState bestDepthCandidate;
        DepthState                  activeDepthState;
        bool                        activeDepthStatePresentTimeFallback = false;
        std::optional<int>          depthSourceOverrideIndex;
        DepthState                  depthSourceOverrideImage;
        int                         depthSourcePinnedIndex            = -1;
        bool                        depthSourceOverrideImageDestroyed = false;
        bool                        depthCopyBeforeClears             = false;
        int                         depthClearIndex                   = 0;
        uint64_t                    depthCopySourceId                 = 0;
        struct DepthCopyTarget
        {
            uint64_t sourceId = 0;
            uint32_t ordinal  = 0;
        };
        std::unordered_map<VkCommandBuffer, DepthCopyTarget> depthCopyTargets;
        DepthCopyTarget                                      depthCopyFallbackTarget;
        std::vector<std::shared_ptr<DepthBackup>>            depthBackupPool;

        // Persistent overlay state that survives swapchain recreation
        std::unique_ptr<OverlayPersistentState> overlayPersistentState;

        // ImGui overlay - lives at device level to survive swapchain recreation
        std::unique_ptr<ImGuiOverlay> imguiOverlay;
    };
} // namespace vkShade

#endif // LOGICAL_DEVICE_HPP_INCLUDED
