#pragma once

#include <type_traits>

#include "logical_device.hpp"

namespace vkShade
{
    template<typename CreateInfo>
    RenderPassMetadata depthRenderPassMetadata(const CreateInfo& info)
    {
        RenderPassMetadata metadata;
        metadata.attachmentFirstUse.assign(info.attachmentCount, VK_ATTACHMENT_UNUSED);
        for (uint32_t i = 0; i < info.attachmentCount; ++i)
        {
            metadata.attachmentInitialLayouts.push_back(info.pAttachments[i].initialLayout);
            metadata.attachmentFinalLayouts.push_back(info.pAttachments[i].finalLayout);
            metadata.attachmentLoadOps.push_back(info.pAttachments[i].loadOp);
        }
        for (uint32_t subpassIndex = 0; subpassIndex < info.subpassCount; ++subpassIndex)
        {
            const auto& subpass  = info.pSubpasses[subpassIndex];
            const auto  markUsed = [&](uint32_t attachment) {
                if (attachment < metadata.attachmentFirstUse.size() && metadata.attachmentFirstUse[attachment] == VK_ATTACHMENT_UNUSED)
                    metadata.attachmentFirstUse[attachment] = subpassIndex;
            };
            for (uint32_t i = 0; i < subpass.inputAttachmentCount; ++i)
                markUsed(subpass.pInputAttachments[i].attachment);
            for (uint32_t i = 0; i < subpass.colorAttachmentCount; ++i)
            {
                markUsed(subpass.pColorAttachments[i].attachment);
                if (subpass.pResolveAttachments)
                    markUsed(subpass.pResolveAttachments[i].attachment);
            }
            const uint32_t depth = subpass.pDepthStencilAttachment ? subpass.pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED;
            markUsed(depth);
            metadata.subpassDepthAttachments.push_back(depth);
            if (metadata.depthAttachmentIndex == VK_ATTACHMENT_UNUSED && depth != VK_ATTACHMENT_UNUSED)
                metadata.depthAttachmentIndex = depth;
            if constexpr (std::is_same_v<CreateInfo, VkRenderPassCreateInfo2>)
                for (auto* next = static_cast<const VkBaseInStructure*>(subpass.pNext); next; next = next->pNext)
                    if (next->sType == VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE)
                    {
                        const auto* resolve = reinterpret_cast<const VkSubpassDescriptionDepthStencilResolve*>(next);
                        if (resolve->pDepthStencilResolveAttachment)
                            markUsed(resolve->pDepthStencilResolveAttachment->attachment);
                    }
        }
        return metadata;
    }
} // namespace vkShade
