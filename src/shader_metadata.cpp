#include "shader_metadata.hpp"

#include <cstdint>
#include <limits>

namespace vkShade
{
    namespace
    {
        void writeNumber(std::string& out, uint32_t value)
        {
            for (unsigned shift = 0; shift != 32; shift += 8)
                out.push_back(static_cast<char>((value >> shift) & 0xff));
        }

        void writeString(std::string& out, const std::string& value)
        {
            writeNumber(out, static_cast<uint32_t>(value.size()));
            out += value;
        }

        bool readNumber(std::string_view& data, uint32_t& value)
        {
            if (data.size() < 4)
                return false;
            value = 0;
            for (unsigned i = 0; i != 4; ++i)
                value |= static_cast<uint32_t>(static_cast<unsigned char>(data[i])) << (8 * i);
            data.remove_prefix(4);
            return true;
        }

        bool readString(std::string_view& data, std::string& value)
        {
            uint32_t length;
            if (!readNumber(data, length) || length > data.size())
                return false;
            value.assign(data.data(), length);
            data.remove_prefix(length);
            return true;
        }
    } // namespace

    std::string encodeShaderMetadata(const ShaderTestResult& result)
    {
        std::string out = "VKSM1";
        writeNumber(out, (result.success ? 1u : 0u) | (result.usesDepth ? 2u : 0u));
        writeString(out, result.errorMessage);
        writeNumber(out, static_cast<uint32_t>(result.techniques.size()));
        for (const auto& technique : result.techniques)
        {
            writeString(out, technique.name);
            writeString(out, technique.label);
            writeString(out, technique.tooltip);
            writeNumber(out, (technique.hidden ? 1u : 0u) | (technique.forcedEnabled ? 2u : 0u) | (technique.noEdit ? 4u : 0u));
        }
        return out;
    }

    std::optional<ShaderTestResult> decodeShaderMetadata(std::string_view data)
    {
        if (!data.starts_with("VKSM1"))
            return std::nullopt;
        data.remove_prefix(5);
        uint32_t         flags, count;
        ShaderTestResult result;
        if (!readNumber(data, flags) || flags > 3 || !readString(data, result.errorMessage) || !readNumber(data, count) || count > data.size() / 16)
            return std::nullopt;
        result.success   = (flags & 1) != 0;
        result.usesDepth = (flags & 2) != 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            ShaderTechniqueInfo technique;
            if (!readString(data, technique.name) || technique.name.empty() || !readString(data, technique.label)
                || !readString(data, technique.tooltip) || !readNumber(data, flags) || flags > 7)
                return std::nullopt;
            technique.hidden        = (flags & 1) != 0;
            technique.forcedEnabled = (flags & 2) != 0;
            technique.noEdit        = (flags & 4) != 0;
            result.techniques.push_back(std::move(technique));
        }
        return data.empty() ? std::optional<ShaderTestResult>(std::move(result)) : std::nullopt;
    }
} // namespace vkShade
