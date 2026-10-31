#include "config_serializer.hpp"
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
    ok &= check(!resolveDepthSource(candidates, -5), "Unavailable rank must not select a source");
    candidates = {first, candidate(4, 4, 3, 2, 10)};
    ok &= check(resolveDepthSource(candidates, -2) == 0 && resolveDepthSource(candidates, -3) == 1, "Equal ranks must follow candidate order");

    candidates = {third, first, second};
    ok &= check(resolveDepthSource(candidates, 0, first.depthState) == 1, "A selected image must survive candidate reordering");
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

    const std::string path = argv[1];
    ProfileSettings   settings;
    ok &= check(ConfigSerializer::saveToPath(path, {}, {}, {}), "Could not save a default profile");
    auto loaded = ConfigSerializer::loadProfileSettings(path);
    ok &= check(!loaded.hasDepthSourceIndex && loaded.depthSourceIndex == -1, "A profile without a selection must retain config fallback");

    for (int selection : {-3, -2, -1, 0, 3})
    {
        settings.depthSourceIndex    = selection;
        settings.hasDepthSourceIndex = true;
        settings.safeAntiCheat       = true;
        ok &= check(ConfigSerializer::saveToPath(path, {}, {}, {}, {}, {}, settings), "Could not save a profile selection");
        loaded = ConfigSerializer::loadProfileSettings(path);
        ok &= check(loaded.hasDepthSourceIndex && loaded.depthSourceIndex == selection && loaded.safeAntiCheat,
                    "Profile selection and Safe Anti-Cheat must round-trip together");
    }
    std::remove(path.c_str());
    return ok ? 0 : 1;
}
