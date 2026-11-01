#ifndef SHADER_METADATA_HPP_INCLUDED
#define SHADER_METADATA_HPP_INCLUDED

#include "reshade_parser.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace vkShade
{
    std::string                     encodeShaderMetadata(const ShaderTestResult& result);
    std::optional<ShaderTestResult> decodeShaderMetadata(std::string_view data);
} // namespace vkShade

#endif
