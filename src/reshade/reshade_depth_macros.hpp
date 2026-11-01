#ifndef VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED
#define VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED

#include "depth_definitions.hpp"
#include "reshade/effect_preprocessor.hpp"

namespace vkShade
{
    inline void applyDepthDefinition(reshadefx::preprocessor& pp, std::string_view name, const std::string& value)
    {
        const auto canonical = canonicalDepthDefinition(name);
        if (canonical.empty())
            return;
        pp.add_shared_macro_definition(std::string(canonical), value);
        pp.add_shared_macro_definition("RESHADE_" + std::string(canonical), value);
    }

    inline void addReshadeDepthMacros(reshadefx::preprocessor& pp, const DepthDefinitionValues& values = {})
    {
        for (const auto& definition : depthDefinitions)
            applyDepthDefinition(pp, definition.name, std::string(definition.defaultValue));
        for (const auto& [name, value] : values)
            applyDepthDefinition(pp, name, value);
        pp.add_macro_definition("RESHADE_MIX_STAGE_DEPTH_MAP", "0");
    }
} // namespace vkShade

#endif // VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED
