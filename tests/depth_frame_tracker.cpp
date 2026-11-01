#include "logical_device.hpp"

#include <cstdint>
#include <iostream>
#include <type_traits>

using namespace vkShade;

template<typename T>
static T handle(uintptr_t value)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<T>(value);
    else
        return static_cast<T>(value);
}

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main()
{
    const auto first   = handle<VkCommandBuffer>(1);
    const auto second  = handle<VkCommandBuffer>(2);
    const auto primary = handle<VkCommandBuffer>(3);
    DepthState depth;
    depth.image     = handle<VkImage>(10);
    depth.imageView = handle<VkImageView>(11);
    depth.sourceId  = 12;

    DepthFrameTracker tracker;
    tracker.begin(first);
    tracker.draw(first, depth, {30, 3, 0});
    tracker.clear(first, depth, DepthClearKind::Image, true);
    tracker.draw(first, depth, {70, 7, 0});
    auto finalLayout           = depth;
    finalLayout.observedLayout = VK_IMAGE_LAYOUT_GENERAL;
    tracker.layout(first, finalLayout);
    tracker.begin(second);
    auto anotherView      = depth;
    anotherView.imageView = handle<VkImageView>(13);
    tracker.draw(second, anotherView, {20, 1, 1});
    tracker.submit(first);
    tracker.submit(first);
    tracker.submit(second);
    auto frame = tracker.finishFrame();
    bool ok    = check(frame.size() == 1, "Views of one image must appear as one depth source");
    if (frame.size() == 1)
    {
        ok &= check(frame[0].stats.vertices == 220 && frame[0].stats.drawcalls == 21 && frame[0].stats.drawcallsIndirect == 1,
                    "Every command-buffer submission must contribute its recorded work");
        ok &= check(frame[0].clears.size() == 2 && frame[0].clears[0].stats.drawcalls == 3 && frame[0].clears[1].stats.drawcalls == 10,
                    "Clear workloads must span submissions in execution order");
    }
    ok &= check(tracker.finishFrame().empty(), "A new frame must not inherit old draw counts");
    tracker.submit(first);
    frame = tracker.finishFrame();
    ok &= check(frame.size() == 1 && frame[0].stats.drawcalls == 10, "Replaying a recorded buffer must count its work again");
    ok &= check(frame.size() == 1 && frame[0].depthState.observedLayout == VK_IMAGE_LAYOUT_GENERAL,
                "A submitted layout transition must update the depth source's final layout");
    auto owner  = std::make_shared<int>(0);
    auto backup = std::shared_ptr<DepthBackup>(owner, reinterpret_cast<DepthBackup*>(owner.get()));
    tracker.begin(first);
    tracker.clear(first, depth, DepthClearKind::Image, true, backup);
    tracker.submit(first);
    tracker.submit(first);
    frame = tracker.finishFrame();
    ok &= check(frame.size() == 1 && frame[0].clears.size() == 2 && !frame[0].clears[0].copyPossible && !frame[0].clears[0].copied
                    && frame[0].clears[1].copied,
                "Repeated submissions can only retain the last snapshot written to the same backup");
    tracker.begin(first);
    tracker.draw(first, depth, {5, 1, 0});
    tracker.begin(primary);
    tracker.execute(primary, first);
    tracker.submit(primary);
    frame = tracker.finishFrame();
    ok &= check(frame.size() == 1 && frame[0].stats.drawcalls == 1 && frame[0].stats.vertices == 5,
                "Re-recording and secondary execution must replace old commands");
    tracker.begin(second);
    tracker.draw(second, {}, {8, 2, 0});
    tracker.clear(second, {}, DepthClearKind::Attachment, false);
    tracker.draw(second, {}, {12, 3, 0});
    tracker.begin(primary);
    tracker.draw(primary, depth, {4, 1, 0});
    tracker.execute(primary, second, &depth);
    tracker.submit(primary);
    frame = tracker.finishFrame();
    ok &= check(frame.size() == 1 && frame[0].stats.drawcalls == 6 && frame[0].clears.size() == 1 && frame[0].clears[0].stats.drawcalls == 3,
                "Secondary draws and clears must inherit the primary depth attachment in order");
    tracker.forgetSource(depth.sourceId);
    tracker.submit(primary);
    ok &= check(tracker.finishFrame().empty(), "Destroyed sources must not reappear through recorded command buffers");
    tracker.reset();
    tracker.begin(first);
    tracker.draw(first, anotherView, {8, 2, 0});
    tracker.begin(second);
    tracker.draw(second, depth, {12, 3, 0});
    tracker.submit(first);
    tracker.submit(second);
    tracker.forgetView(depth.imageView);
    ok &= check(tracker.finishFrame().empty(), "A view destroyed before presentation must not become a depth candidate");
    tracker.submit(first);
    frame = tracker.finishFrame();
    ok &= check(frame.size() == 1 && frame[0].depthState.imageView == anotherView.imageView && frame[0].stats.drawcalls == 2,
                "Destroying one view must preserve recorded work using another live view");
    tracker.reset();
    auto otherDepth      = depth;
    otherDepth.image     = handle<VkImage>(20);
    otherDepth.imageView = handle<VkImageView>(21);
    otherDepth.sourceId  = 22;
    tracker.begin(first);
    tracker.clear(first, depth, DepthClearKind::Image, true);
    tracker.clear(first, otherDepth, DepthClearKind::Image, true);
    tracker.clear(first, depth, DepthClearKind::Image, true);
    tracker.submit(first);
    frame = tracker.finishFrame();
    ok &= check(frame.size() == 2 && frame[0].clears.size() == 2 && frame[0].clears[0].ordinal == 0 && frame[0].clears[1].ordinal == 1
                    && frame[1].clears.size() == 1 && frame[1].clears[0].ordinal == 0,
                "Clear ordinals must be independent for each depth image in a command buffer");
    tracker.forget(first);
    return ok ? 0 : 1;
}
