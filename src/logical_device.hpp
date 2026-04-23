#ifndef LOGICAL_DEVICE_HPP_INCLUDED
#define LOGICAL_DEVICE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>
#include <unordered_map>

#include "vulkan_include.hpp"
#include "vkdispatch.hpp"

namespace vkShade
{
    struct LogicalSwapchain;  // Forward declaration

    struct DepthState
    {
        VkImageView imageView = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent3D extent = {0, 0, 1};
        VkImageLayout observedLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    };

    struct DepthSnapshotTarget
    {
        VkSwapchainKHR  swapchain = VK_NULL_HANDLE;
        LogicalSwapchain* pLogicalSwapchain = nullptr;
        uint32_t        imageIndex = 0;
    };

    struct DepthImageMetadata
    {
        VkImageUsageFlags     usage = 0;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageTiling         tiling = VK_IMAGE_TILING_OPTIMAL;
    };

    struct OverlayPersistentState;  // Forward declaration
    class ImGuiOverlay;  // Forward declaration

    struct LogicalDevice
    {
        struct DepthScopeTrackingState
        {
            bool     inRenderScope = false;
            DepthState depthState;
            DepthSnapshotTarget snapshotTarget;
            uint32_t  drawCount = 0;
        };

        struct DepthCandidateTrackingState
        {
            bool     valid = false;
            DepthState depthState;
            bool     hasPresentableSnapshotTarget = false;
            bool     extentMatchesPresentableTarget = false;
            uint32_t  drawCount = 0;
        };

        DeviceDispatch           vkd;
        InstanceDispatch         vki;
        VkDevice                 device;
        VkPhysicalDevice         physicalDevice;
        VkInstance               instance;
        VkQueue                  queue;
        uint32_t                 queueFamilyIndex;
        VkCommandPool            commandPool;
        bool                     supportsMutableFormat;
        bool                     isNvidiaGpu;
        bool                     gpuCrashDiagnosticsEnabled = false;
        bool                     supportsNvDiagnosticCheckpoints = false;
        bool                     supportsNvDiagnosticsConfig = false;
        bool                     supportsDeviceFaultExt = false;
        std::vector<VkImage>     depthImages;
        std::vector<VkFormat>    depthFormats;
        std::vector<VkImageView> depthImageViews;
        std::unordered_map<VkImage, VkExtent3D> depthImageExtents;
        std::unordered_map<VkImage, DepthImageMetadata> depthImageMetadata;
        std::unordered_map<VkImageView, DepthState> depthViewStates;
        std::unordered_map<VkImageView, DepthSnapshotTarget> snapshotTargetViewStates;
        std::unordered_map<VkFramebuffer, DepthState> framebufferDepthStates;
        std::unordered_map<VkFramebuffer, DepthSnapshotTarget> framebufferSnapshotTargets;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> commandBufferDepthStates;
        std::unordered_map<VkCommandBuffer, DepthScopeTrackingState> pendingTransferLinkedDepthScopes;
        std::unordered_map<VkCommandBuffer, uint32_t> commandBufferRecordedDrawCounts;
        DepthCandidateTrackingState  bestDepthCandidate;
        DepthState               activeDepthState;

        // Persistent overlay state that survives swapchain recreation
        std::unique_ptr<OverlayPersistentState> overlayPersistentState;

        // ImGui overlay - lives at device level to survive swapchain recreation
        std::unique_ptr<ImGuiOverlay> imguiOverlay;
    };
} // namespace vkShade

#endif // LOGICAL_DEVICE_HPP_INCLUDED
