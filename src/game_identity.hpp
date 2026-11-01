#ifndef VKSHADE_GAME_IDENTITY_HPP_INCLUDED
#define VKSHADE_GAME_IDENTITY_HPP_INCLUDED

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vkShade
{
    inline std::string encodeGameIdentityPart(std::string_view value)
    {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string    result;
        for (const unsigned char c : value)
        {
            if (c < 32 || c == 127 || c == '%' || c == '=' || c == '/')
            {
                result += '%';
                result += digits[c >> 4];
                result += digits[c & 15];
            }
            else
                result += static_cast<char>(c);
        }
        return result;
    }

    inline std::string gameIdentityPath(std::string_view prefix, const std::filesystem::path& path, const std::filesystem::path& workingDirectory)
    {
        if (path.empty())
            return {};
        auto absolute = path.is_absolute() ? path.lexically_normal() : (workingDirectory / path).lexically_normal();
        if (!absolute.is_absolute())
            return {};
        std::string identity(prefix);
        for (const auto& part : absolute.relative_path())
        {
            const auto component = part.string();
            if (component.empty() || component == "." || component == "..")
                continue;
            identity += '/';
            identity += encodeGameIdentityPart(component);
        }
        return identity == prefix ? std::string{} : identity;
    }

    inline std::string javaGameIdentity(const std::vector<std::string>& args, const std::filesystem::path& workingDirectory)
    {
        if (args.empty())
            return {};
        const auto executable = std::filesystem::path(args[0]).filename().string();
        if (executable != "java" && executable != "javaw")
            return {};
        static constexpr std::string_view optionsWithValue[] = {"-cp",
                                                                "-classpath",
                                                                "--class-path",
                                                                "-p",
                                                                "--module-path",
                                                                "--upgrade-module-path",
                                                                "--add-modules",
                                                                "--limit-modules",
                                                                "--add-reads",
                                                                "--add-exports",
                                                                "--add-opens",
                                                                "--patch-module",
                                                                "--enable-native-access",
                                                                "--source"};
        for (size_t i = 1; i < args.size(); ++i)
        {
            const auto& arg = args[i];
            if (arg == "-jar")
                return i + 1 < args.size() ? gameIdentityPath("java-jar", args[i + 1], workingDirectory) : std::string{};
            if (arg == "-m" || arg == "--module")
                return i + 1 < args.size() ? "java-module/" + encodeGameIdentityPart(args[i + 1]) : std::string{};
            if (arg.starts_with("--module="))
                return "java-module/" + encodeGameIdentityPart(std::string_view(arg).substr(9));
            if (std::find(std::begin(optionsWithValue), std::end(optionsWithValue), arg) != std::end(optionsWithValue))
            {
                if (++i == args.size())
                    return {};
                continue;
            }
            if (arg.empty() || arg[0] == '@')
                return {};
            if (arg[0] == '-')
                continue;
            if (arg.find('.') == std::string::npos
                || arg.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.$") != std::string::npos)
                return {};
            return "java-main/" + encodeGameIdentityPart(arg);
        }
        return {};
    }

    inline std::string gameIdentity(std::string_view                command,
                                    std::string_view                steamAppId,
                                    const std::vector<std::string>& processArgs,
                                    const std::filesystem::path&    workingDirectory)
    {
        if (!steamAppId.empty() && steamAppId.size() <= 20
            && std::all_of(steamAppId.begin(), steamAppId.end(), [](unsigned char c) { return std::isdigit(c) != 0; })
            && steamAppId.find_first_not_of('0') != std::string_view::npos)
            return "steam/" + std::string(steamAppId);
        if (const auto java = javaGameIdentity(processArgs, workingDirectory); !java.empty())
            return java;
        return gameIdentityPath("command", std::filesystem::path(command), workingDirectory);
    }

    inline bool usesGameIdentityDirectory(std::string_view gameName)
    {
        return gameName.starts_with("command/") || gameName.starts_with("steam/") || gameName.starts_with("java-main/")
               || gameName.starts_with("java-module/") || gameName.starts_with("java-jar/");
    }
} // namespace vkShade

#endif // VKSHADE_GAME_IDENTITY_HPP_INCLUDED
