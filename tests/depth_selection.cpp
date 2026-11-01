#include "config_serializer.hpp"
#include "depth_render_pass.hpp"
#include "depth_selection.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

using namespace vkShade;

template<typename T>
static T fakeHandle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}

static LogicalDevice::DepthSourceCandidate candidate(uintptr_t handle, uint64_t sourceId, uint32_t direct, uint32_t indirect, uint64_t vertices)
{
    LogicalDevice::DepthSourceCandidate result;
    result.depthState.image     = fakeHandle<VkImage>(handle);
    result.depthState.imageView = fakeHandle<VkImageView>(handle);
    result.depthState.sourceId  = sourceId;
    result.stats                = {vertices, direct, indirect};
    return result;
}

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;

    const auto                                       first      = candidate(1, 1, 3, 2, 10);
    const auto                                       second     = candidate(2, 2, 4, 1, 20);
    const auto                                       third      = candidate(3, 3, 2, 0, 100);
    std::vector<LogicalDevice::DepthSourceCandidate> candidates = {first, second, third};
    bool                                             ok         = true;

    ok &= check(resolveDepthSourcePreference(std::nullopt, 2) == 2, "An unset selection must use the config");
    ok &= check(resolveDepthSourcePreference(-1, 2) == -1, "Explicit Auto must override a configured index");
    ok &= check(!resolveDepthSource(candidates, -1), "Auto must not select a manual source");
    ok &= check(resolveDepthSource(candidates, 0) == 0, "Fixed index must select its row");
    ok &= check(resolveDepthSource(candidates, -2) == 1, "Top rank must use draw count, then vertices");
    ok &= check(resolveDepthSource(candidates, -3) == 0, "Second rank must select the next source");
    ok &= check(depthWorkloadBeats({1000, 10, 0}, {100, 20, 0}), "Direct workloads should prefer vertex count");
    ok &= check(depthWorkloadBeats({0, 0, 30}, {1000, 10, 0}), "Indirect-heavy workloads should prefer draw count");
    ok &= check(!resolveDepthSource(candidates, -5), "Unavailable rank must not select a source");
    candidates = {first, candidate(4, 4, 3, 2, 10)};
    ok &= check(resolveDepthSource(candidates, -2) == 0 && resolveDepthSource(candidates, -3) == 1, "Equal ranks must follow candidate order");

    candidates = {third, first, second};
    ok &= check(resolveDepthSource(candidates, 0, first.depthState) == 1, "A selected image must survive candidate reordering");
    auto changedView                 = first;
    changedView.depthState.imageView = fakeHandle<VkImageView>(99);
    candidates                       = {changedView};
    ok &= check(resolveDepthSource(candidates, 0, first.depthState) == 0, "A selected image must survive a change of image view");
    auto changedLayout                      = first;
    changedLayout.depthState.observedLayout = VK_IMAGE_LAYOUT_GENERAL;
    candidates                              = {changedLayout, first};
    ok &= check(resolveDepthSource(candidates, 0, first.depthState) == 1,
                "A selected image must prefer its observed layout when multiple scopes use it");
    candidates = {changedLayout};
    ok &= check(resolveDepthSource(candidates, 0, first.depthState) == 0, "A selected image must follow a single changed layout");
    auto anotherLayout                      = first;
    anotherLayout.depthState.observedLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    candidates                              = {changedLayout, anotherLayout};
    ok &= check(!resolveDepthSource(candidates, 0, first.depthState), "Multiple changed layouts must not silently select a different scope");
    candidates = {third, second};
    ok &= check(!resolveDepthSource(candidates, 0, first.depthState), "A missing image must not silently select another row");
    candidates = {candidate(1, 4, 3, 2, 10)};
    ok &= check(!resolveDepthSource(candidates, 0, first.depthState), "Reused Vulkan handles must not select a different image");
    ok &= check(!resolveDepthSource(candidates, 0, {}, true), "A destroyed selection must not fall back to its old index");

    auto previous = first;
    previous.clears.resize(1);
    previous.clears[0].stats         = {12, 4, 1};
    previous.clears[0].commandBuffer = fakeHandle<VkCommandBuffer>(123);
    previous.clears[0].copyPossible  = true;
    previous.clears[0].copied        = true;
    previous.clears[0].backup        = std::shared_ptr<DepthBackup>(fakeHandle<DepthBackup*>(1), [](DepthBackup*) {});
    candidates                       = {previous};
    for (int frame = 0; frame < 100; ++frame)
        candidates = retainDepthSourceHistory({}, std::move(candidates));
    ok &= check(candidates.size() == 1 && candidates[0].stale && candidates[0].stats.drawcalls == first.stats.drawcalls
                    && candidates[0].clears.size() == 1 && candidates[0].clears[0].stats.drawcalls == 4,
                "A live source must retain its last draw and clear statistics while gameplay is paused");
    ok &= check(!candidates[0].clears[0].backup && candidates[0].clears[0].commandBuffer == VK_NULL_HANDLE && !candidates[0].clears[0].copied,
                "Inactive history must not retain an executable command buffer or depth backup");
    ok &= check(resolveDepthSource(candidates, 0, first.depthState) == 0 && !resolveActiveDepthSource(candidates, 0, first.depthState),
                "An inactive source may remain selected but must not be sampled until it is observed again");
    candidates = retainDepthSourceHistory({second}, std::move(candidates));
    ok &= check(depthSourceDrawRank(candidates, 1) == 0, "Active sources must rank ahead of inactive history");
    candidates = retainDepthSourceHistory({first}, std::move(candidates));
    ok &= check(resolveActiveDepthSource(candidates, 0, first.depthState) == 0 && !candidates[0].stale,
                "A selected source must become usable again when new work references it");

    candidates = retainDepthSourceHistory({first, second}, {});
    for (int frameNumber = 0; frameNumber < 100; ++frameNumber)
        candidates = retainDepthSourceHistory({second, first}, std::move(candidates));
    ok &= check(candidates.size() == 2 && sameDepthSource(candidates[0].depthState, first.depthState)
                    && sameDepthSource(candidates[1].depthState, second.depthState),
                "Candidate indices must remain tied to their images when submission order changes");
    candidates[0] = {};
    ok &= check(!resolveDepthSource(candidates, 0) && resolveDepthSource(candidates, -2) == 1 && depthSourceDrawRank(candidates, 1) == 0,
                "A destroyed candidate's empty slot must not be selectable or counted in draw ranks");
    candidates = retainDepthSourceHistory({third, second}, std::move(candidates));
    ok &= check(candidates.size() == 2 && sameDepthSource(candidates[0].depthState, third.depthState)
                    && sameDepthSource(candidates[1].depthState, second.depthState),
                "A new image may reuse an empty slot without changing another live candidate's index");
    candidates[1].depthState.imageView = VK_NULL_HANDLE;
    markDepthSourceStale(candidates[1]);
    auto replacementView                 = second;
    replacementView.depthState.imageView = fakeHandle<VkImageView>(99);
    candidates                           = retainDepthSourceHistory({replacementView, third}, std::move(candidates));
    ok &= check(resolveActiveDepthSource(candidates, 1, second.depthState) == 1
                    && candidates[1].depthState.imageView == replacementView.depthState.imageView,
                "An image must retain its index and selection when its view is replaced");

    using Clock                                                  = std::chrono::steady_clock;
    const auto                                             start = Clock::time_point{};
    DepthFrameCadence                                      cadence;
    const std::vector<LogicalDevice::DepthSourceCandidate> scene = {candidate(1, 1, 400, 0, 100000)};
    const std::vector<LogicalDevice::DepthSourceCandidate> extra = {candidate(2, 2, 4, 0, 24)};
    ok &= check(!acceptDepthFrame(extra, scene, cadence, start) && !acceptDepthFrame(extra, scene, cadence, start + std::chrono::milliseconds(100))
                    && acceptDepthFrame(scene, scene, cadence, start + std::chrono::milliseconds(110)),
                "Brief low-work presentations between scene frames must not replace the scene statistics");
    ok &= check(!acceptDepthFrame({}, scene, cadence, start + std::chrono::seconds(1))
                    && acceptDepthFrame(extra, scene, cadence, start + std::chrono::seconds(2)),
                "A sustained low-work scene must eventually replace old scene statistics");
    ok &= check(acceptDepthFrame(extra, {}, cadence, start + std::chrono::seconds(3)),
                "Low-draw applications must be detected when no earlier scene exists");
    AutomaticDepthSelection automatic;
    auto                    eligible = [](const auto& source) { return source.depthState.image != VK_NULL_HANDLE && !source.stale; };
    candidates                       = {candidate(1, 1, 400, 0, 100000), candidate(2, 2, 400, 0, 99999)};
    ok &= check(selectStableAutomaticDepthSource(candidates, automatic, start, eligible) == 0,
                "Automatic selection must choose an available source immediately");
    for (int frameNumber = 1; frameNumber <= 100; ++frameNumber)
    {
        candidates[1].stats.vertices = frameNumber % 2 ? 100001 : 99999;
        ok &= check(selectStableAutomaticDepthSource(candidates, automatic, start + std::chrono::milliseconds(10 * frameNumber), eligible) == 0,
                    "Small alternating workload changes must not make automatic selection flicker");
    }
    candidates[1].stats.vertices = 200000;
    ok &= check(selectStableAutomaticDepthSource(candidates, automatic, start + std::chrono::seconds(2), eligible) == 0
                    && selectStableAutomaticDepthSource(candidates, automatic, start + std::chrono::milliseconds(2100), eligible) == 0,
                "A brief workload spike must not immediately replace the active source");
    ok &= check(selectStableAutomaticDepthSource(candidates, automatic, start + std::chrono::seconds(3), eligible) == 1,
                "A consistently stronger candidate must eventually be promoted");
    candidates[1].stale = true;
    ok &= check(selectStableAutomaticDepthSource(candidates, automatic, start + std::chrono::seconds(3), eligible) == 0,
                "An unavailable source must yield immediately to a usable candidate");
    candidates[0].stale = true;
    ok &= check(!selectStableAutomaticDepthSource(candidates, automatic, start + std::chrono::seconds(4), eligible),
                "Automatic selection must not select a stale source");
    automatic  = {};
    candidates = {candidate(1, 1, 100, 0, 100000), candidate(2, 2, 0, 500, 0)};
    ok &= check(selectStableAutomaticDepthSource(candidates, automatic, start, eligible) == 1,
                "Indirect-heavy workloads must use draw counts when vertex counts are unavailable");
    candidates = {candidate(1, 1, 100, 0, 100000)};
    automatic  = {};
    selectStableAutomaticDepthSource(candidates, automatic, start, eligible);
    candidates.push_back(candidate(2, 2, 0, 500, 0));
    selectStableAutomaticDepthSource(candidates, automatic, start + std::chrono::seconds(1), eligible);
    ok &= check(selectStableAutomaticDepthSource(candidates, automatic, start + std::chrono::seconds(2), eligible) == 1,
                "A consistently stronger indirect workload must be promoted over an older direct workload");

    std::vector<DepthClearCandidate> clears(3);
    clears[0].copyPossible = true;
    clears[0].stats        = {100, 10, 0};
    clears[1].copyPossible = false;
    clears[1].stats        = {1000, 20, 0};
    clears[2].copyPossible = true;
    clears[2].stats        = {500, 8, 0};
    ok &= check(selectDepthClear(clears, 0) == 2, "Automatic selection must choose the strongest usable clear");
    ok &= check(selectDepthClear(clears, 1) == 0 && selectDepthClear(clears, -1) == 2, "Clear indices must select from the beginning or end");
    ok &= check(!selectDepthClear(clears, 2) && !selectDepthClear(clears, 4) && !selectDepthClear(clears, -4),
                "Unavailable and out-of-range clears must not silently select a different clear");
    clears[2].stats = clears[0].stats;
    ok &= check(selectDepthClear(clears, 0) == 2, "Automatic clear selection must prefer the later clear on a tie");

    std::vector<DepthClearCandidate> autoClears(2);
    autoClears[0].copyPossible = true;
    autoClears[1].copyPossible = true;
    autoClears[0].stats        = {1000, 10, 0};
    autoClears[1].stats        = {999, 10, 0};
    AutomaticDepthSelection automaticClear;
    ok &= check(selectStableAutomaticDepthClear(autoClears, automaticClear, start) == 0, "Automatic clear selection must choose a usable clear");
    for (int frameNumber = 1; frameNumber <= 100; ++frameNumber)
    {
        autoClears[1].stats.vertices = frameNumber % 2 ? 1001 : 999;
        ok &= check(selectStableAutomaticDepthClear(autoClears, automaticClear, start + std::chrono::milliseconds(frameNumber * 10)) == 0,
                    "Alternating clear workloads must not make automatic clear selection flicker");
    }
    autoClears[1].stats.vertices = 2000;
    selectStableAutomaticDepthClear(autoClears, automaticClear, start + std::chrono::seconds(2));
    ok &= check(selectStableAutomaticDepthClear(autoClears, automaticClear, start + std::chrono::seconds(3)) == 1,
                "A consistently stronger clear must eventually be promoted");
    autoClears[1].copyPossible = false;
    ok &= check(selectStableAutomaticDepthClear(autoClears, automaticClear, start + std::chrono::seconds(3)) == 0,
                "An unavailable clear must yield immediately to a usable clear");
    automaticClear             = {};
    autoClears[1].copyPossible = true;
    autoClears[1].stats        = autoClears[0].stats;
    ok &= check(selectStableAutomaticDepthClear(autoClears, automaticClear, start) == 1,
                "Automatic clear selection must prefer the later clear on an equal workload");

    VkAttachmentDescription attachments[3]{};
    VkAttachmentReference   firstDepth{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference   laterDepth{2, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription    subpasses[3]{};
    subpasses[0].pDepthStencilAttachment = &firstDepth;
    subpasses[1].inputAttachmentCount    = 1;
    subpasses[1].pInputAttachments       = &laterDepth;
    subpasses[2].pDepthStencilAttachment = &laterDepth;
    VkRenderPassCreateInfo renderPass{};
    renderPass.attachmentCount = 3;
    renderPass.pAttachments    = attachments;
    renderPass.subpassCount    = 3;
    renderPass.pSubpasses      = subpasses;
    const auto metadata        = depthRenderPassMetadata(renderPass);
    ok &= check(metadata.subpassDepthAttachments == std::vector<uint32_t>{0, VK_ATTACHMENT_UNUSED, 2},
                "Each subpass must use its own depth attachment");
    ok &= check(metadata.attachmentFirstUse == std::vector<uint32_t>{0, VK_ATTACHMENT_UNUSED, 1},
                "Attachment clears must follow first use, including use as an input attachment");

    VkAttachmentDescription2 attachments2[2]{};
    VkAttachmentReference2   resolveReference{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    resolveReference.attachment = 1;
    VkSubpassDescriptionDepthStencilResolve resolve{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    resolve.pDepthStencilResolveAttachment = &resolveReference;
    VkSubpassDescription2 subpass2{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    subpass2.pNext = &resolve;
    VkRenderPassCreateInfo2 renderPass2{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    renderPass2.attachmentCount = 2;
    renderPass2.pAttachments    = attachments2;
    renderPass2.subpassCount    = 1;
    renderPass2.pSubpasses      = &subpass2;
    const auto metadata2        = depthRenderPassMetadata(renderPass2);
    ok &= check(metadata2.attachmentFirstUse[1] == 0, "Depth resolve attachments must count as a subpass use");

    const std::string path = argv[1];
    ProfileSettings   settings;
    ok &= check(ConfigSerializer::saveToPath(path, {}, {}, {}), "Could not save a default profile");
    auto loaded = ConfigSerializer::loadProfileSettings(path);
    ok &= check(!loaded.hasDepthSourceIndex && loaded.depthSourceIndex == -1, "A profile without a selection must retain config fallback");

    for (int selection : {-3, -2, -1, 0, 3})
    {
        settings.depthSourceIndex      = selection;
        settings.hasDepthSourceIndex   = true;
        settings.safeAntiCheat         = true;
        settings.depthCopyBeforeClears = true;
        settings.depthClearIndex       = selection;
        ok &= check(ConfigSerializer::saveToPath(path, {}, {}, {}, {}, {}, settings), "Could not save a profile selection");
        loaded = ConfigSerializer::loadProfileSettings(path);
        ok &= check(loaded.hasDepthSourceIndex && loaded.depthSourceIndex == selection && loaded.safeAntiCheat && loaded.depthCopyBeforeClears
                        && loaded.depthClearIndex == selection,
                    "Depth selection, clear copying and Safe Anti-Cheat must round-trip together");
    }
    std::remove(path.c_str());
    return ok ? 0 : 1;
}
