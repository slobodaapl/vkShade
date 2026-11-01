#pragma once

#include "reshade/effect_module.hpp"

#include <optional>
#include <string>
#include <vector>

namespace vkShade
{
    std::optional<std::vector<reshadefx::pass_info>> selectTechniquePasses(const reshadefx::module&          module,
                                                                           const std::optional<std::string>& technique);
} // namespace vkShade
