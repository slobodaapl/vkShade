#ifndef VKSHADE_DEPTH_DEFINITIONS_HPP_INCLUDED
#define VKSHADE_DEPTH_DEFINITIONS_HPP_INCLUDED

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace vkShade
{
    struct DepthDefinition
    {
        std::string_view name;
        std::string_view defaultValue;
        enum class Type
        {
            Flag,
            Real,
            NonzeroReal,
            PositiveReal,
            Integer
        } type = Type::Real;
    };

    using DepthDefinitionType = DepthDefinition::Type;
    inline constexpr std::array depthDefinitions{
        DepthDefinition{"DEPTH_INPUT_IS_UPSIDE_DOWN", "0", DepthDefinitionType::Flag},
        DepthDefinition{"DEPTH_INPUT_IS_REVERSED", "0", DepthDefinitionType::Flag},
        DepthDefinition{"DEPTH_INPUT_IS_MIRRORED", "0", DepthDefinitionType::Flag},
        DepthDefinition{"DEPTH_INPUT_IS_LOGARITHMIC", "0", DepthDefinitionType::Flag},
        DepthDefinition{"DEPTH_INPUT_X_SCALE", "1.0", DepthDefinitionType::NonzeroReal},
        DepthDefinition{"DEPTH_INPUT_Y_SCALE", "1.0", DepthDefinitionType::NonzeroReal},
        DepthDefinition{"DEPTH_INPUT_X_OFFSET", "0.0", DepthDefinitionType::Real},
        DepthDefinition{"DEPTH_INPUT_Y_OFFSET", "0.0", DepthDefinitionType::Real},
        DepthDefinition{"DEPTH_INPUT_X_PIXEL_OFFSET", "0", DepthDefinitionType::Integer},
        DepthDefinition{"DEPTH_INPUT_Y_PIXEL_OFFSET", "0", DepthDefinitionType::Integer},
        DepthDefinition{"DEPTH_LINEARIZATION_FAR_PLANE", "1000.0", DepthDefinitionType::PositiveReal},
        DepthDefinition{"DEPTH_MULTIPLIER", "1.0", DepthDefinitionType::Real},
    };

    using DepthDefinitionValues = std::map<std::string, std::string>;

    struct EffectiveDepthDefinitions
    {
        DepthDefinitionValues values;
        std::set<std::string> conflicts;
    };

    inline std::string_view canonicalDepthDefinition(std::string_view name)
    {
        if (name.starts_with("RESHADE_"))
            name.remove_prefix(8);
        for (const auto& definition : depthDefinitions)
            if (definition.name == name)
                return definition.name;
        return {};
    }

    inline std::string depthDefinitionValue(const DepthDefinitionValues& values, const DepthDefinition& definition)
    {
        const auto found = values.find(std::string(definition.name));
        return found == values.end() ? std::string(definition.defaultValue) : found->second;
    }

    inline bool validDepthDefinitionValue(const DepthDefinition& definition, std::string_view value)
    {
        if (value.empty())
            return false;
        if (definition.type == DepthDefinitionType::Flag)
            return value == "0" || value == "1";
        if (definition.type == DepthDefinitionType::Integer)
        {
            std::int32_t result{};
            const auto   parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
        }
        float      result{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(result))
            return false;
        if (definition.type == DepthDefinitionType::NonzeroReal)
            return result != 0.0f;
        if (definition.type == DepthDefinitionType::PositiveReal)
            return result > 0.0f;
        return true;
    }
} // namespace vkShade

#endif // VKSHADE_DEPTH_DEFINITIONS_HPP_INCLUDED
