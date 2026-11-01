#pragma once

#include <vector>
#include <cstdint>

namespace vkShade
{
    const std::vector<uint32_t> depth_resolve_frag = {
#include "depth_resolve.frag.h"
    };

    const std::vector<uint32_t> full_screen_triangle_vert = {
#include "full_screen_triangle.vert.h"
    };

} // namespace vkShade
