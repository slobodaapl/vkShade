#include "technique_id.hpp"

#include <utility>

namespace vkShade
{
    namespace
    {
        bool isSafe(unsigned char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'
                   || c == '~';
        }

        std::string encodeField(std::string_view value)
        {
            constexpr char hex[] = "0123456789ABCDEF";
            std::string    result;
            for (unsigned char c : value)
            {
                if (isSafe(c))
                    result.push_back(static_cast<char>(c));
                else
                {
                    result.push_back('%');
                    result.push_back(hex[c >> 4]);
                    result.push_back(hex[c & 15]);
                }
            }
            return result;
        }

        int hexValue(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            return -1;
        }

        std::optional<std::string> decodeField(std::string_view value)
        {
            std::string result;
            for (size_t i = 0; i < value.size(); ++i)
            {
                unsigned char c = static_cast<unsigned char>(value[i]);
                if (c == '%')
                {
                    if (i + 2 >= value.size())
                        return std::nullopt;
                    const int high = hexValue(value[i + 1]);
                    const int low  = hexValue(value[i + 2]);
                    if (high < 0 || low < 0)
                        return std::nullopt;
                    c = static_cast<unsigned char>((high << 4) | low);
                    i += 2;
                }
                else if (!isSafe(c))
                    return std::nullopt;
                if (c == 0)
                    return std::nullopt;
                result.push_back(static_cast<char>(c));
            }
            return result;
        }
    } // namespace

    std::string encodeTechniqueId(const TechniqueId& id)
    {
        char owner;
        switch (id.owner)
        {
            case TechniqueOwner::BuiltIn: owner = 'B'; break;
            case TechniqueOwner::File: owner = 'F'; break;
            case TechniqueOwner::Instance: owner = 'I'; break;
            default: return {};
        }
        return std::string(1, owner) + '@' + encodeField(id.effect) + '@' + encodeField(id.technique);
    }

    std::optional<TechniqueId> decodeTechniqueId(std::string_view encoded)
    {
        if (encoded.size() < 3 || encoded[1] != '@')
            return std::nullopt;
        TechniqueOwner owner;
        switch (encoded[0])
        {
            case 'B': owner = TechniqueOwner::BuiltIn; break;
            case 'F': owner = TechniqueOwner::File; break;
            case 'I': owner = TechniqueOwner::Instance; break;
            default: return std::nullopt;
        }
        const auto separator = encoded.find('@', 2);
        if (separator == std::string_view::npos || encoded.find('@', separator + 1) != std::string_view::npos)
            return std::nullopt;
        auto effect    = decodeField(encoded.substr(2, separator - 2));
        auto technique = decodeField(encoded.substr(separator + 1));
        if (!effect || effect->empty() || !technique)
            return std::nullopt;
        return TechniqueId{owner, std::move(*effect), std::move(*technique)};
    }
} // namespace vkShade
