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
#include <unordered_map>

#include "vulkan_include.hpp"
#include "vkdispatch.hpp"

namespace vkShade
{
    struct LogicalSwapchain; // Forward declaration

    struct DepthState
    {
        VkImageView   imageView      = VK_NULL_HANDLE;
        VkImage       image          = VK_NULL_HANDLE;
        uint64_t      sourceId       = 0;
        VkFormat      format         = VK_FORMAT_UNDEFINED;
        VkExtent3D    extent         = {0, 0, 1};
        VkImageLayout observedLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    };

    struct DepthSnapshotTarget
    {
        VkSwapchainKHR    swapchain         = VK_NULL_HANDLE;
        LogicalSwapchain* pLogicalSwapchain = nullptr;
        uint32_t          imageIndex        = 0;
    };

    struct DepthImageMetadata
    {
        VkImageUsageFlags     usage    = 0;
        VkSampleCountFlagBits samples  = VK_SAMPLE_COUNT_1_BIT;
        VkImageTiling         tiling   = VK_IMAGE_TILING_OPTIMAL;
        uint64_t              sourceId = 0;
    };

    struct RenderPassMetadata
    {
        std::vector<VkImageLayout> attachmentFinalLayouts;
        uint32_t                   depthAttachmentIndex = 0xFFFFFFFFu;
    };

    struct OverlayPersistentState; // Forward declaration
    class ImGuiOverlay;            // Forward declaration

    struct LogicalDevice
    {
        // Per-command-buffer running totals of draws / vertices attributed
        // to the currently-active dynamic-rendering or render-pass scope.
        struct DrawStats
        {
            uint64_t vertices          = 0;
            uint32_t drawcalls         = 0;
            uint32_t drawcallsIndirect = 0;
        };

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

        struct DepthSourceCandidate
        {
            DepthState depthState;
            DrawStats  stats;
        };

        DeviceDispatch                                               vkd;
        InstanceDispatch                                             vki;
        VkDevice                                                     device;
        VkPhysicalDevice                                             physicalDevice;
        VkInstance                                                   instance;
        VkQueue                                                      queue;
        uint32_t                                                     queueFamilyIndex = 0;
        VkCommandPool                                                commandPool;
        bool                                                         supportsMutableFormat           = false;
        bool                                                         isNvidiaGpu                     = false;
        bool                                                         gpuCrashDiagnosticsEnabled      = false;
        bool                                                         supportsNvDiagnosticCheckpoints = false;
        bool                                                         supportsNvDiagnosticsConfig     = false;
        bool                                                         supportsDeviceFaultExt          = false;
        std::vector<VkImage>                                         depthImages;
        std::vector<VkFormat>                                        depthFormats;
        std::vector<VkImageView>                                     depthImageViews;
        std::unordered_map<VkImage, VkExtent3D>                      depthImageExtents;
        std::unordered_map<VkImage, DepthImageMetadata>              depthImageMetadata;
        uint64_t                                                     nextDepthSourceId = 0;
        std::unordered_map<VkImageView, DepthState>                  depthViewStates;
        std::unordered_map<VkRenderPass, RenderPassMetadata>         renderPassMetadata;
        std::unordered_map<VkImageView, DepthSnapshotTarget>         snapshotTargetViewStates;
        std::unordered_map<VkFramebuffer, DepthState>                framebufferDepthStates;
        std::unordered_map<VkFramebuffer, DepthSnapshotTarget>       framebufferSnapshotTargets;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> commandBufferDepthStates;
        std::unordered_map<VkCommandBuffer, DepthSnapshotTarget>     commandBufferColorOnlySnapshotTargets;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> pendingTransferLinkedDepthScopes;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> completedSecondaryDepthScopes;
        std::unordered_map<VkCommandBuffer, DrawStats>               commandBufferRecordedDraws;
        std::unordered_map<VkCommandBuffer, VkCommandBufferLevel>    commandBufferLevels;
        std::vector<DepthCandidateTrackingState>                     frameDepthCandidates;
        std::vector<DepthSourceCandidate>                            depthSourceCandidates;
        DepthCandidateTrackingState                                  bestDepthCandidate;
        DepthState                                                   activeDepthState;
        bool                                                         activeDepthStatePresentTimeFallback = false;
        std::optional<int>                                           depthSourceOverrideIndex;
        DepthState                                                   depthSourceOverrideImage;
        int                                                          depthSourcePinnedIndex            = -1;
        bool                                                         depthSourceOverrideImageDestroyed = false;

        // Persistent overlay state that survives swapchain recreation
        std::unique_ptr<OverlayPersistentState> overlayPersistentState;

        // ImGui overlay - lives at device level to survive swapchain recreation
        std::unique_ptr<ImGuiOverlay> imguiOverlay;
    };
} // namespace vkShade

#endif // LOGICAL_DEVICE_HPP_INCLUDED
