#include "technique_passes.hpp"

namespace vkShade
{
    std::optional<std::vector<reshadefx::pass_info>> selectTechniquePasses(const reshadefx::module&          module,
                                                                           const std::optional<std::string>& technique)
    {
        std::vector<reshadefx::pass_info> passes;
        bool                              found = !technique.has_value();
        for (const auto& entry : module.techniques)
        {
            if (technique && entry.name != *technique)
                continue;
            found = true;
            passes.insert(passes.end(), entry.passes.begin(), entry.passes.end());
        }
        if (!found)
            return std::nullopt;
        return passes;
    }
} // namespace vkShade
