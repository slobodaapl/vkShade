#include "config_serializer.hpp"
#include "config.hpp"
#include "config_paths.hpp"
#include "game_identity.hpp"
#include "logger.hpp"

#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <array>
#include <unistd.h>
#include <climits>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>

namespace vkShade
{
    namespace
    {
        constexpr int MIN_MAX_EFFECTS = 1;
        constexpr int MAX_MAX_EFFECTS = 200;

        class FileLock
        {
        public:
            explicit FileLock(const std::string& path) : fd(open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600))
            {
                if (fd < 0)
                    return;
                while (flock(fd, LOCK_EX) != 0)
                {
                    if (errno == EINTR)
                        continue;
                    close(fd);
                    fd = -1;
                    break;
                }
            }
            ~FileLock()
            {
                if (fd >= 0)
                    close(fd);
            }
            explicit operator bool() const
            {
                return fd >= 0;
            }
            FileLock(const FileLock&)            = delete;
            FileLock& operator=(const FileLock&) = delete;

        private:
            int fd;
        };

        std::string makeTempFile(const std::string& path)
        {
            const auto        pattern = path + ".tmp.XXXXXX";
            std::vector<char> name(pattern.begin(), pattern.end());
            name.push_back('\0');
            const int fd = mkstemp(name.data());
            if (fd < 0)
                return {};
            struct stat original;
            if (stat(path.c_str(), &original) == 0 && fchmod(fd, original.st_mode & 07777) != 0)
            {
                close(fd);
                std::remove(name.data());
                return {};
            }
            if (close(fd) != 0)
            {
                std::remove(name.data());
                return {};
            }
            return name.data();
        }
    } // namespace

    std::string ConfigSerializer::getBaseConfigDir()
    {
        const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
        if (xdgConfig)
            return std::string(xdgConfig) + "/vkShade";

        const char* home = std::getenv("HOME");
        if (home)
            return std::string(home) + "/.config/vkShade";

        return "";
    }

    std::string ConfigSerializer::getConfigsDir()
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
            return "";
        return baseDir + "/configs";
    }

    std::vector<std::string> ConfigSerializer::listConfigs()
    {
        std::vector<std::string> configs;
        std::string              dir = getConfigsDir();

        DIR* d = opendir(dir.c_str());
        if (!d)
            return configs;

        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr)
        {
            std::string name = entry->d_name;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".conf")
                configs.push_back(name.substr(0, name.size() - 5));
        }
        closedir(d);

        std::sort(configs.begin(), configs.end());
        return configs;
    }

    static std::string joinEffects(const std::vector<std::string>& effects)
    {
        std::string result;
        for (size_t i = 0; i < effects.size(); i++)
        {
            if (i > 0)
                result += ":";
            result += effects[i];
        }
        return result;
    }

    static std::string quotePath(const std::string& path)
    {
        std::string result = "\"";
        for (char c : path)
        {
            if (c == '\\' || c == '"')
                result += '\\';
            result += c;
        }
        return result + '"';
    }

    bool ConfigSerializer::saveConfig(const std::string&                         configName,
                                      const std::vector<std::string>&            effects,
                                      const std::vector<std::string>&            disabledEffects,
                                      const std::vector<ConfigParam>&            params,
                                      const std::map<std::string, std::string>&  effectPaths,
                                      const std::vector<PreprocessorDefinition>& preprocessorDefs,
                                      const ProfileSettings&                     profileSettings,
                                      const std::set<std::string>&               unresolvedEffects,
                                      const std::string&                         sourcePath)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
        {
            Logger::err("Could not determine configs directory");
            return false;
        }

        mkdir(configsDir.c_str(), 0755);

        std::string filePath = configsDir + "/" + configName + ".conf";
        bool        result =
            saveToPath(filePath, effects, disabledEffects, params, effectPaths, preprocessorDefs, profileSettings, unresolvedEffects, sourcePath);
        if (result)
            Logger::info("Saved config to: " + filePath);
        return result;
    }

    bool ConfigSerializer::deleteConfig(const std::string& configName)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return false;

        std::string filePath = configsDir + "/" + configName + ".conf";
        if (std::remove(filePath.c_str()) == 0)
        {
            Logger::info("Deleted config: " + filePath);
            return true;
        }
        Logger::err("Failed to delete config: " + filePath);
        return false;
    }

    std::string ConfigSerializer::getDefaultConfigPath()
    {
        const char* home = std::getenv("HOME");
        if (home)
            return std::string(home) + "/.config/vkShade/default_config";
        return "";
    }

    bool ConfigSerializer::setDefaultConfig(const std::string& configName)
    {
        std::string path = getDefaultConfigPath();
        if (path.empty())
            return false;

        std::ofstream file(path);
        if (!file.is_open())
        {
            Logger::err("Could not write default config file: " + path);
            return false;
        }

        file << configName;
        file.close();
        Logger::info("Set default config: " + configName);
        return true;
    }

    std::string ConfigSerializer::getDefaultConfig()
    {
        std::string path = getDefaultConfigPath();
        if (path.empty())
            return "";

        std::ifstream file(path);
        if (!file.is_open())
            return "";

        std::string configName;
        std::getline(file, configName);
        return configName;
    }

    VkShadeSettings ConfigSerializer::loadSettings()
    {
        VkShadeSettings settings;

        // Check user config first, then fall back to system paths (same order as Config)
        std::string userConfig = getBaseConfigDir() + "/vkShade.conf";

        const std::array<std::string, 3> configPaths = {
            userConfig,
            std::string(SYSCONFDIR) + "/vkShade/vkShade.conf",
            std::string(SYSCONFDIR) + "/vkShade.conf",
        };

        std::string configPath;
        for (const auto& path : configPaths)
        {
            std::ifstream test(path);
            if (test.is_open())
            {
                configPath = path;
                break;
            }
        }

        if (configPath.empty())
            return settings;

        Logger::info("SettingsManager loading from: " + configPath);

        std::ifstream file(configPath);
        if (!file.is_open())
            return settings;

        std::string line;
        while (std::getline(file, line))
        {
            // Skip comments and empty lines
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key   = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // Trim whitespace
            auto trimWs = [](std::string& s) {
                size_t start = s.find_first_not_of(" \t");
                size_t end   = s.find_last_not_of(" \t");
                s            = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
            };
            trimWs(key);
            trimWs(value);

            if (key == "maxEffects")
            {
                try
                {
                    int parsed = std::stoi(value);
                    if (parsed < 0)
                    {
                        Logger::err("invalid maxEffects value (negative): " + value);
                    }
                    else
                    {
                        settings.maxEffects = std::clamp(parsed, MIN_MAX_EFFECTS, MAX_MAX_EFFECTS);
                        if (settings.maxEffects != parsed)
                        {
                            Logger::warn("clamped maxEffects from " + std::to_string(parsed) + " to " + std::to_string(settings.maxEffects));
                        }
                    }
                }
                catch (...)
                {
                    Logger::err("invalid maxEffects value: " + value);
                }
            }
            else if (key == "overlayBlockInput")
                settings.overlayBlockInput = (value == "true" || value == "1");
            else if (key == "overlayFontSize")
            {
                try
                {
                    const int parsed = std::stoi(value);
                    if (parsed < 0)
                        Logger::err("invalid overlayFontSize value: " + value);
                    else
                        settings.overlayFontSize = parsed == AUTO_OVERLAY_FONT_SIZE
                                                       ? AUTO_OVERLAY_FONT_SIZE
                                                       : std::clamp(parsed, MIN_OVERLAY_FONT_SIZE, MAX_OVERLAY_FONT_SIZE);
                }
                catch (...)
                {
                    Logger::err("invalid overlayFontSize value: " + value);
                }
            }
            else if (key == "toggleKey")
                settings.toggleKey = value;
            else if (key == "reloadKey")
                settings.reloadKey = value;
            else if (key == "overlayKey")
                settings.overlayKey = value;
            else if (key == "enableOnLaunch")
                settings.enableOnLaunch = (value == "true" || value == "1");
            else if (key == "depthCapture")
                settings.depthCapture = (value == "on");
            else if (key == "autoApply")
                settings.autoApply = (value == "true" || value == "1");
            else if (key == "autoApplyDelay")
            {
                try
                {
                    settings.autoApplyDelay = std::stoi(value);
                }
                catch (...)
                {
                    Logger::err("invalid autoApplyDelay value: " + value);
                }
            }
            else if (key == "showDebugWindow")
                settings.showDebugWindow = (value == "true" || value == "1");
        }

        return settings;
    }

    bool ConfigSerializer::saveSettings(const VkShadeSettings& settings)
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
        {
            Logger::err("Could not determine config directory");
            return false;
        }

        mkdir(baseDir.c_str(), 0755);

        std::string   configPath = baseDir + "/vkShade.conf";
        std::ofstream file(configPath);
        if (!file.is_open())
        {
            Logger::err("Could not open vkShade.conf for writing: " + configPath);
            return false;
        }

        // Write settings with comments
        file << "# vkShade configuration\n\n";

        file << "# Overlay settings\n";
        const int clampedMaxEffects = std::clamp(settings.maxEffects, MIN_MAX_EFFECTS, MAX_MAX_EFFECTS);

        file << "overlayBlockInput = " << (settings.overlayBlockInput ? "true" : "false") << "\n";
        file << "overlayFontSize = "
             << (settings.overlayFontSize <= AUTO_OVERLAY_FONT_SIZE
                     ? AUTO_OVERLAY_FONT_SIZE
                     : std::clamp(settings.overlayFontSize, MIN_OVERLAY_FONT_SIZE, MAX_OVERLAY_FONT_SIZE))
             << "\n";
        file << "maxEffects = " << clampedMaxEffects << "\n";
        file << "autoApply = " << (settings.autoApply ? "true" : "false") << "\n";
        file << "autoApplyDelay = " << settings.autoApplyDelay << "\n";

        file << "\n# Key bindings\n";
        file << "toggleKey = " << settings.toggleKey << "\n";
        file << "reloadKey = " << settings.reloadKey << "\n";
        file << "overlayKey = " << settings.overlayKey << "\n";

        file << "\n# Startup behavior\n";
        file << "enableOnLaunch = " << (settings.enableOnLaunch ? "true" : "false") << "\n";
        file << "depthCapture = " << (settings.depthCapture ? "on" : "off") << "\n";

        file << "\n# Debug\n";
        file << "showDebugWindow = " << (settings.showDebugWindow ? "true" : "false") << "\n";

        file.close();
        Logger::info("Saved settings to: " + configPath);
        return true;
    }

    void ConfigSerializer::ensureConfigExists()
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
            return;

        // Create directory if needed
        mkdir(baseDir.c_str(), 0755);

        std::string configPath = baseDir + "/vkShade.conf";

        // Only create if no user config exists
        struct stat st;
        if (stat(configPath.c_str(), &st) == 0)
            return;

        VkShadeSettings defaults;
        saveSettings(defaults);
        Logger::info("Created default vkShade.conf");
    }

    std::string ConfigSerializer::detectExecutableName()
    {
        char    buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0)
            return "";

        buf[len] = '\0';
        std::string exePath(buf);

        // Extract basename
        size_t lastSlash = exePath.rfind('/');
        if (lastSlash == std::string::npos)
            return exePath;

        return exePath.substr(lastSlash + 1);
    }

    std::string ConfigSerializer::detectGameName()
    {
        for (const char* variable : {"SteamAppId", "SteamGameId"})
        {
            const char* value = std::getenv(variable);
            if (value)
            {
                auto identity = gameIdentity({}, value, {}, {});
                if (!identity.empty())
                    return identity;
            }
        }

        const char*              command   = std::getenv("VKSHADE_WRAPPED_COMMAND");
        const char*              directory = std::getenv("VKSHADE_WRAPPED_CWD");
        std::error_code          error;
        const auto               workingDirectory = directory && *directory ? std::filesystem::path(directory) : std::filesystem::current_path(error);
        std::vector<std::string> args;
        std::ifstream            cmdline("/proc/self/cmdline", std::ios::binary);
        for (std::string arg; std::getline(cmdline, arg, '\0');)
            args.push_back(std::move(arg));
        auto identity = gameIdentity(command ? command : "", "", args, workingDirectory);
        return identity.empty() ? detectExecutableName() : identity;
    }

    std::string ConfigSerializer::autoDetectConfig()
    {
        std::string gameName = detectGameName();
        if (gameName.empty())
            return "";

        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return "";

        std::string configPath = getProfilePath(gameName);
        struct stat st;
        if (stat(configPath.c_str(), &st) != 0)
            return "";

        Logger::info("Auto-loaded config for: " + gameName);
        return gameName;
    }

    // Case-insensitive string comparison helper
    static bool equalsIgnoreCaseLocal(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    // Scan a directory recursively for Shaders/ and Textures/ subdirectories
    static void scanDirectoryForShaders(const std::string& dir, std::vector<std::string>& shaderPaths, std::vector<std::string>& texturePaths)
    {
        try
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied))
            {
                if (!entry.is_directory())
                    continue;

                std::string dirName = entry.path().filename().string();
                if (equalsIgnoreCaseLocal(dirName, "Shaders"))
                    shaderPaths.push_back(entry.path().string());
                else if (equalsIgnoreCaseLocal(dirName, "Textures"))
                    texturePaths.push_back(entry.path().string());
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            Logger::err("Error scanning directory " + dir + ": " + e.what());
        }
    }

    ShaderManagerConfig ConfigSerializer::loadShaderManagerConfig()
    {
        ShaderManagerConfig config;
        std::string         configPath = getBaseConfigDir() + "/shader_manager.conf";

        std::ifstream file(configPath);
        if (!file.is_open())
        {
            // Config file doesn't exist - set up defaults
            std::string defaultReshadeDir = getBaseConfigDir() + "/reshade";

            // Create directories if they don't exist
            mkdir(defaultReshadeDir.c_str(), 0755);
            mkdir((defaultReshadeDir + "/Shaders").c_str(), 0755);
            mkdir((defaultReshadeDir + "/Textures").c_str(), 0755);

            config.parentDirectories.push_back(defaultReshadeDir);

            // Auto-scan to discover paths
            scanDirectoryForShaders(defaultReshadeDir, config.discoveredShaderPaths, config.discoveredTexturePaths);

            // Save the config so it persists
            saveShaderManagerConfig(config);
            Logger::info("Created default shader manager config with reshade directory");
            return config;
        }

        // File exists - parse it (respect user's choices, even if empty)
        std::string line;
        while (std::getline(file, line))
        {
            // Skip comments and empty lines
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key   = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // Trim whitespace
            auto trimWs = [](std::string& s) {
                size_t start = s.find_first_not_of(" \t");
                size_t end   = s.find_last_not_of(" \t");
                s            = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
            };
            trimWs(key);
            trimWs(value);

            if (key == "parentDir" && !value.empty())
                config.parentDirectories.push_back(value);
            else if (key == "shaderPath" && !value.empty())
                config.discoveredShaderPaths.push_back(value);
            else if (key == "texturePath" && !value.empty())
                config.discoveredTexturePaths.push_back(value);
        }

        return config;
    }

    bool ConfigSerializer::saveShaderManagerConfig(const ShaderManagerConfig& config)
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
        {
            Logger::err("Could not determine config directory");
            return false;
        }

        mkdir(baseDir.c_str(), 0755);

        std::string   configPath = baseDir + "/shader_manager.conf";
        std::ofstream file(configPath);
        if (!file.is_open())
        {
            Logger::err("Could not open shader_manager.conf for writing: " + configPath);
            return false;
        }

        file << "# Shader Manager configuration\n";
        file << "# Parent directories are scanned recursively for Shaders/ and Textures/ subdirs\n\n";

        file << "# Parent directories (user-added)\n";
        for (const auto& dir : config.parentDirectories)
            file << "parentDir = " << dir << "\n";

        file << "\n# Discovered shader paths (auto-generated on scan)\n";
        for (const auto& path : config.discoveredShaderPaths)
            file << "shaderPath = " << path << "\n";

        file << "\n# Discovered texture paths (auto-generated on scan)\n";
        for (const auto& path : config.discoveredTexturePaths)
            file << "texturePath = " << path << "\n";

        file.close();
        Logger::info("Saved shader manager config to: " + configPath);
        return true;
    }

    // --- Per-app profile system ---

    std::string ConfigSerializer::getProfilePath(const std::string& gameName, const std::string& profileName)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty() || gameName.empty())
            return "";

        if (profileName.find_first_of("/\n\r") != std::string::npos)
            return "";
        if (usesGameIdentityDirectory(gameName))
        {
            const auto path = std::filesystem::path(gameName).lexically_normal();
            if (path.is_absolute() || path.string().starts_with("../"))
                return "";
            const auto filename = profileName.empty() || profileName == "default" ? "default.conf" : "@" + profileName + ".conf";
            return (std::filesystem::path(configsDir) / path / filename).string();
        }
        if (profileName.empty() || profileName == "default")
            return configsDir + "/" + gameName + ".conf";

        return configsDir + "/" + gameName + "@" + profileName + ".conf";
    }

    std::string ConfigSerializer::getGameDepthPath(const std::string& gameName)
    {
        const auto configsDir = getConfigsDir();
        if (configsDir.empty() || gameName.empty())
            return {};
        const auto path = std::filesystem::path(gameName).lexically_normal();
        if (path.is_absolute() || path == "." || path == ".." || path.string().starts_with("../"))
            return {};
        return (std::filesystem::path(configsDir) / ".depth" / path / "settings.conf").string();
    }

    std::string ConfigSerializer::ensureGameProfile(const std::string& gameName)
    {
        if (gameName.empty())
            return "";

        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return "";

        std::string profilePath = getProfilePath(gameName);
        if (profilePath.empty())
            return "";
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(profilePath).parent_path(), error);
        if (error)
        {
            Logger::err("Could not create profile directory: " + error.message());
            return "";
        }

        const int fd = open(profilePath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0)
        {
            if (errno == EEXIST)
                return profilePath;
            Logger::err("Could not create profile for " + gameName + ": " + profilePath);
            return "";
        }
        const std::string contents = "# vkShade profile for " + gameName + "\n# Auto-created on first launch\n\neffects = \n";
        size_t            written  = 0;
        while (written < contents.size())
        {
            const ssize_t count = write(fd, contents.data() + written, contents.size() - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            written += static_cast<size_t>(count);
        }
        const bool closed = close(fd) == 0;
        if (written != contents.size() || !closed)
        {
            Logger::err("Could not write profile for " + gameName + ": " + profilePath);
            return "";
        }
        Logger::info("Created default profile for " + gameName + ": " + profilePath);

        return profilePath;
    }

    void ConfigSerializer::migrateGameProfiles(const std::string& gameName, const std::string& legacyGameName)
    {
        if (!usesGameIdentityDirectory(gameName) || legacyGameName.empty())
            return;
        const auto targetDefault = getProfilePath(gameName);
        if (targetDefault.empty())
            return;
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(targetDefault).parent_path(), error);
        if (error)
        {
            Logger::warn("Could not create migrated profile directory: " + error.message());
            return;
        }
        FileLock lock((std::filesystem::path(targetDefault).parent_path() / ".migration.lock").string());
        if (!lock)
        {
            Logger::warn("Could not lock game profile migration");
            return;
        }
        if (!listProfilesForGame(gameName).empty())
            return;
        const auto profiles = listProfilesForGame(legacyGameName);
        if (profiles.empty())
            return;
        for (const auto& profile : profiles)
        {
            const auto source = getProfilePath(legacyGameName, profile);
            const auto target = getProfilePath(gameName, profile);
            if (source.empty() || target.empty())
                continue;
            std::filesystem::copy_file(source, target, std::filesystem::copy_options::skip_existing, error);
            if (error)
            {
                Logger::warn("Could not migrate game profile: " + error.message());
                return;
            }
        }
        const auto active = getActiveProfile(legacyGameName);
        if (active != "default" && std::filesystem::exists(getProfilePath(gameName, active), error))
            setActiveProfile(gameName, active);
    }

    std::vector<std::string> ConfigSerializer::listProfilesForGame(const std::string& gameName)
    {
        std::vector<std::string> profiles;
        if (gameName.empty())
            return profiles;

        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return profiles;

        // Check for default profile: <gameName>.conf
        struct stat st;
        std::string defaultPath = getProfilePath(gameName);
        if (stat(defaultPath.c_str(), &st) == 0)
            profiles.push_back("default");

        // Check for named profiles: <gameName>@<name>.conf
        const bool  separateDirectory = usesGameIdentityDirectory(gameName);
        std::string prefix            = separateDirectory ? "@" : gameName + "@";
        std::string searchDir         = separateDirectory ? configsDir + "/" + gameName : configsDir;
        DIR*        d                 = opendir(searchDir.c_str());
        if (!d)
            return profiles;

        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr)
        {
            std::string name = entry->d_name;
            if (name.size() <= 5)
                continue;
            if (name.substr(name.size() - 5) != ".conf")
                continue;
            if (name.substr(0, prefix.size()) != prefix)
                continue;

            // Extract profile name: <gameName>@<profileName>.conf
            std::string profileName = name.substr(prefix.size(), name.size() - prefix.size() - 5);
            if (!profileName.empty())
                profiles.push_back(profileName);
        }
        closedir(d);

        if (profiles.size() > 1)
            std::sort(profiles.begin() + 1, profiles.end());
        return profiles;
    }

    std::string ConfigSerializer::getActiveProfile(const std::string& gameName)
    {
        if (gameName.empty())
            return "default";

        std::string   activePath = getConfigsDir() + "/.active_profiles";
        std::ifstream file(activePath);
        if (!file.is_open())
            return "default";

        std::string line;
        while (std::getline(file, line))
        {
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = line.substr(0, eq);
            if (key == gameName)
                return line.substr(eq + 1);
        }

        return "default";
    }

    void ConfigSerializer::setActiveProfile(const std::string& gameName, const std::string& profileName)
    {
        if (gameName.empty())
            return;

        const auto configsDir = getConfigsDir();
        if (configsDir.empty())
            return;
        std::string activePath = configsDir + "/.active_profiles";
        FileLock    lock(activePath + ".lock");
        if (!lock)
        {
            Logger::warn("Could not lock active profiles");
            return;
        }

        // Read existing entries
        std::map<std::string, std::string> entries;
        {
            std::ifstream file(activePath);
            if (file.is_open())
            {
                std::string line;
                while (std::getline(file, line))
                {
                    size_t eq = line.find('=');
                    if (eq != std::string::npos)
                        entries[line.substr(0, eq)] = line.substr(eq + 1);
                }
            }
        }

        entries[gameName] = profileName;

        // Write back atomically
        std::string tmpPath = makeTempFile(activePath);
        if (tmpPath.empty())
            return;
        std::ofstream file(tmpPath);
        if (!file.is_open())
        {
            std::remove(tmpPath.c_str());
            return;
        }

        for (const auto& [key, value] : entries)
            file << key << "=" << value << "\n";

        file.close();
        if (file.fail() || std::rename(tmpPath.c_str(), activePath.c_str()) != 0)
            std::remove(tmpPath.c_str());
    }

    bool ConfigSerializer::createProfile(const std::string& gameName, const std::string& profileName, const std::string& copyFromProfile)
    {
        if (gameName.empty() || profileName.empty() || profileName == "default")
            return false;

        std::string newPath = getProfilePath(gameName, profileName);
        if (newPath.empty())
            return false;

        // Check if already exists
        struct stat st;
        if (stat(newPath.c_str(), &st) == 0)
            return false; // Already exists

        // Copy from source profile or create empty
        if (!copyFromProfile.empty())
        {
            std::string   srcPath = getProfilePath(gameName, copyFromProfile);
            std::ifstream src(srcPath, std::ios::binary);
            if (src.is_open())
            {
                std::ofstream dst(newPath, std::ios::binary);
                dst << src.rdbuf();
                Logger::info("Created profile " + profileName + " for " + gameName + " (copied from " + copyFromProfile + ")");
                return true;
            }
        }

        // Create empty profile
        std::ofstream file(newPath);
        if (!file.is_open())
            return false;

        file << "# vkShade profile '" << profileName << "' for " << gameName << "\n\n";
        file << "effects = \n";
        file.close();

        Logger::info("Created profile " + profileName + " for " + gameName);
        return true;
    }

    bool ConfigSerializer::deleteProfile(const std::string& gameName, const std::string& profileName)
    {
        // Prevent deleting default profile
        if (profileName.empty() || profileName == "default")
            return false;

        std::string path = getProfilePath(gameName, profileName);
        if (path.empty())
            return false;

        if (std::remove(path.c_str()) == 0)
        {
            Logger::info("Deleted profile " + profileName + " for " + gameName);

            // If this was the active profile, switch back to default
            if (getActiveProfile(gameName) == profileName)
                setActiveProfile(gameName, "default");

            return true;
        }

        return false;
    }

    ProfileSettings ConfigSerializer::loadProfileSettings(const std::string& filePath)
    {
        ProfileSettings                              settings;
        DepthDefinitionValues                        canonicalDepthValues;
        DepthDefinitionValues                        reshadeDepthValues;
        std::map<std::string, std::set<std::string>> legacyDepthValues;
        std::ifstream                                file(filePath);
        if (!file.is_open())
            return settings;

        std::string line;
        while (std::getline(file, line))
        {
            // Skip comments and empty lines
            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line[start] == '#')
                continue;

            // Parse key = value
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key   = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // Trim whitespace
            auto trim = [](std::string& s) {
                s.erase(0, s.find_first_not_of(" \t"));
                s.erase(s.find_last_not_of(" \t") + 1);
            };
            trim(key);
            trim(value);

            const auto separator = key.rfind('@');
            const auto macroName = separator == std::string::npos ? std::string_view(key) : std::string_view(key).substr(separator + 1);
            const auto canonical = canonicalDepthDefinition(macroName);
            if (!canonical.empty() && !value.empty())
            {
                const std::string name(canonical);
                settings.hasDepthDefinitions = true;
                if (separator != std::string::npos)
                    legacyDepthValues[name].insert(value);
                else if (macroName.starts_with("RESHADE_"))
                    reshadeDepthValues[name] = value;
                else
                    canonicalDepthValues[name] = value;
                continue;
            }

            if (key == "safeAntiCheat")
                settings.safeAntiCheat = (value == "true" || value == "on" || value == "1");
            else if (key == "depthCopyBeforeClears")
                settings.depthCopyBeforeClears = (value == "true" || value == "on" || value == "1");
            else if (key == "depthClearIndex")
            {
                try
                {
                    settings.depthClearIndex = std::stoi(value);
                }
                catch (...)
                {
                    Logger::warn("invalid depthClearIndex value: " + value);
                }
            }
            else if (key == "techniqueOrder" || key == "enabledTechniques")
            {
                auto& entries = key == "techniqueOrder" ? settings.techniqueOrder : settings.enabledTechniques;
                entries.clear();
                size_t start = 0;
                while (start < value.size())
                {
                    const size_t end = value.find(':', start);
                    entries.push_back(value.substr(start, end == std::string::npos ? end : end - start));
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                if (key == "techniqueOrder")
                    settings.hasTechniqueOrder = true;
                else
                    settings.hasEnabledTechniques = true;
            }
            else if (key == "depthSourceIndex")
            {
                try
                {
                    settings.depthSourceIndex    = std::stoi(value);
                    settings.hasDepthSourceIndex = true;
                }
                catch (...)
                {
                    Logger::warn("invalid depthSourceIndex value: " + value);
                }
            }
        }

        for (const auto& definition : depthDefinitions)
        {
            const std::string name(definition.name);
            const auto        canonical = canonicalDepthValues.find(name);
            const auto        reshade   = reshadeDepthValues.find(name);
            if (canonical != canonicalDepthValues.end())
            {
                settings.depthDefinitions[name] = canonical->second;
                if (reshade != reshadeDepthValues.end() && reshade->second != canonical->second)
                    settings.conflictingDepthDefinitions.insert(name);
            }
            else if (reshade != reshadeDepthValues.end())
                settings.depthDefinitions[name] = reshade->second;
            else if (auto legacy = legacyDepthValues.find(name); legacy != legacyDepthValues.end())
            {
                if (legacy->second.size() == 1)
                    settings.depthDefinitions[name] = *legacy->second.begin();
                else if (!legacy->second.empty())
                    settings.conflictingDepthDefinitions.insert(name);
            }
        }

        return settings;
    }

    ProfileSettings ConfigSerializer::loadGameDepthSettings(const std::string& gameName, const std::string& activeProfilePath)
    {
        ProfileSettings result;
        if (gameName.empty())
            return loadProfileSettings(activeProfilePath);

        const auto shared = loadProfileSettings(getGameDepthPath(gameName));
        if (shared.depthDefinitions.size() == depthDefinitions.size() && shared.conflictingDepthDefinitions.empty())
            return shared;

        std::map<std::string, std::set<std::string>> legacyValues;
        std::set<std::string>                        legacyConflicts;
        std::vector<std::string>                     paths;
        for (const auto& profile : listProfilesForGame(gameName))
            paths.push_back(getProfilePath(gameName, profile));
        if (!activeProfilePath.empty() && std::find(paths.begin(), paths.end(), activeProfilePath) == paths.end())
            paths.push_back(activeProfilePath);
        for (const auto& path : paths)
        {
            const auto settings = loadProfileSettings(path);
            for (const auto& [name, value] : settings.depthDefinitions)
                legacyValues[name].insert(value);
            legacyConflicts.insert(settings.conflictingDepthDefinitions.begin(), settings.conflictingDepthDefinitions.end());
        }
        for (const auto& definition : depthDefinitions)
        {
            const std::string name(definition.name);
            if (auto value = shared.depthDefinitions.find(name); value != shared.depthDefinitions.end())
                result.depthDefinitions[name] = value->second;
            else if (auto legacy = legacyValues.find(name);
                     legacy != legacyValues.end() && legacy->second.size() == 1 && !legacyConflicts.count(name))
                result.depthDefinitions[name] = *legacy->second.begin();
            if (shared.conflictingDepthDefinitions.count(name))
                result.conflictingDepthDefinitions.insert(name);
            else if (!shared.depthDefinitions.count(name)
                     && (legacyConflicts.count(name) || (legacyValues.count(name) && legacyValues[name].size() > 1)))
                result.conflictingDepthDefinitions.insert(name);
        }
        result.hasDepthDefinitions = !result.depthDefinitions.empty() || !result.conflictingDepthDefinitions.empty();
        return result;
    }

    bool
    ConfigSerializer::saveGameDepthSettings(const std::string& gameName, const DepthDefinitionValues& values, const std::set<std::string>& conflicts)
    {
        const auto path = getGameDepthPath(gameName);
        if (path.empty())
            return false;
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        if (error)
            return false;
        FileLock lock(path + ".lock");
        if (!lock)
            return false;
        std::vector<std::string> preserved;
        std::ifstream            previous(path);
        for (std::string line; std::getline(previous, line);)
        {
            const auto equals = line.find('=');
            if (equals == std::string::npos)
            {
                preserved.push_back(line);
                continue;
            }
            auto key = line.substr(0, equals);
            key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return c == ' ' || c == '\t'; }), key.end());
            const auto canonical = canonicalDepthDefinition(key);
            if (canonical.empty() || conflicts.count(std::string(canonical)))
                preserved.push_back(line);
        }
        const auto temporary = makeTempFile(path);
        if (temporary.empty())
            return false;
        std::ofstream file(temporary);
        if (!file.is_open())
        {
            std::remove(temporary.c_str());
            return false;
        }
        for (const auto& definition : depthDefinitions)
            if (!conflicts.count(std::string(definition.name)))
                file << definition.name << " = " << depthDefinitionValue(values, definition) << '\n';
        for (const auto& line : preserved)
            file << line << '\n';
        file.close();
        if (file.fail() || std::rename(temporary.c_str(), path.c_str()) != 0)
        {
            std::remove(temporary.c_str());
            return false;
        }
        return true;
    }

    EffectiveDepthDefinitions ConfigSerializer::loadEffectiveDepthDefinitions(Config* config)
    {
        if (!config)
            return {};
        auto       result   = loadEffectiveDepthDefinitions(config->getFallback());
        const auto settings = config->getGameName().empty() ? loadProfileSettings(config->getConfigFilePath())
                                                            : loadGameDepthSettings(config->getGameName(), config->getConfigFilePath());
        for (const auto& [name, value] : settings.depthDefinitions)
        {
            result.values[name] = value;
            result.conflicts.erase(name);
        }
        for (const auto& name : settings.conflictingDepthDefinitions)
        {
            result.conflicts.insert(name);
            if (!settings.depthDefinitions.count(name))
                result.values.erase(name);
        }
        return result;
    }

    DepthDefinitionValues ConfigSerializer::loadDepthDefinitions(Config* config)
    {
        return loadEffectiveDepthDefinitions(config).values;
    }

    bool ConfigSerializer::saveToPath(const std::string&                         filePath,
                                      const std::vector<std::string>&            effects,
                                      const std::vector<std::string>&            disabledEffects,
                                      const std::vector<ConfigParam>&            params,
                                      const std::map<std::string, std::string>&  effectPaths,
                                      const std::vector<PreprocessorDefinition>& preprocessorDefs,
                                      const ProfileSettings&                     profileSettings,
                                      const std::set<std::string>&               unresolvedEffects,
                                      const std::string&                         sourcePath)
    {
        std::vector<std::string> preservedLines;
        const auto&              previousPath = sourcePath.empty() ? filePath : sourcePath;
        ProfileSettings          savedProfile = profileSettings;
        if (!savedProfile.hasDepthDefinitions)
        {
            const auto previousSettings              = loadProfileSettings(previousPath);
            savedProfile.hasDepthDefinitions         = previousSettings.hasDepthDefinitions;
            savedProfile.depthDefinitions            = previousSettings.depthDefinitions;
            savedProfile.conflictingDepthDefinitions = previousSettings.conflictingDepthDefinitions;
        }
        std::ifstream previous(previousPath);
        if (previous)
        {
            Config                   oldConfig(previousPath);
            const auto               definitions = oldConfig.getEffectDefinitions();
            const auto               oldEffects  = oldConfig.getOption<std::vector<std::string>>("effects", {});
            std::vector<std::string> owners      = oldEffects;
            owners.insert(owners.end(), effects.begin(), effects.end());
            for (const auto& [name, _] : definitions)
                owners.push_back(name);
            std::sort(owners.begin(), owners.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
            for (std::string line; std::getline(previous, line);)
            {
                const auto equals  = line.find('=');
                const auto comment = line.find('#');
                if (equals == std::string::npos || (comment != std::string::npos && comment < equals))
                    continue;
                auto key = line.substr(0, equals);
                key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return c == ' ' || c == '\t' || c == '"'; }), key.end());
                const auto separator = key.rfind('@');
                const auto macroName = separator == std::string::npos ? std::string_view(key) : std::string_view(key).substr(separator + 1);
                if (const auto canonical = canonicalDepthDefinition(macroName); !canonical.empty())
                {
                    if (savedProfile.conflictingDepthDefinitions.count(std::string(canonical)))
                        preservedLines.push_back(line);
                    continue;
                }
                for (const auto& owner : owners)
                {
                    if (key != owner && !key.starts_with(owner + '.') && !key.starts_with(owner + '@'))
                        continue;
                    const bool unused     = definitions.count(owner) && std::find(oldEffects.begin(), oldEffects.end(), owner) == oldEffects.end()
                                            && std::find(effects.begin(), effects.end(), owner) == effects.end();
                    const bool unresolved = unresolvedEffects.count(owner) && (key != owner || !effectPaths.count(owner));
                    if (unused || unresolved)
                        preservedLines.push_back(line);
                    break;
                }
            }
        }

        // Atomic write: write to temp file then rename to prevent corruption
        std::string tmpPath = makeTempFile(filePath);
        if (tmpPath.empty())
        {
            Logger::err("Could not create temporary config for: " + filePath);
            return false;
        }
        std::ofstream file(tmpPath);
        if (!file.is_open())
        {
            Logger::err("Could not open for writing: " + tmpPath);
            std::remove(tmpPath.c_str());
            return false;
        }

        // Write per-profile settings
        if (profileSettings.safeAntiCheat)
            file << "safeAntiCheat = true\n\n";
        if (profileSettings.hasDepthSourceIndex || profileSettings.depthSourceIndex != -1)
            file << "depthSourceIndex = " << profileSettings.depthSourceIndex << "\n\n";
        if (profileSettings.depthCopyBeforeClears)
            file << "depthCopyBeforeClears = true\n";
        if (profileSettings.depthClearIndex != 0)
            file << "depthClearIndex = " << profileSettings.depthClearIndex << "\n";
        if (profileSettings.hasTechniqueOrder)
            file << "techniqueOrder = " << joinEffects(profileSettings.techniqueOrder) << "\n";
        if (profileSettings.hasEnabledTechniques)
            file << "enabledTechniques = " << joinEffects(profileSettings.enabledTechniques) << "\n";
        for (const auto& [name, value] : savedProfile.depthDefinitions)
            if (!savedProfile.conflictingDepthDefinitions.count(name))
                file << name << " = " << value << "\n";
        for (const auto& line : preservedLines)
            file << line << '\n';

        // Group params by effect
        std::map<std::string, std::vector<const ConfigParam*>> paramsByEffect;
        for (const auto& param : params)
            paramsByEffect[param.effectName].push_back(&param);

        // Group preprocessor defs by effect
        std::map<std::string, std::vector<const PreprocessorDefinition*>> defsByEffect;
        for (const auto& def : preprocessorDefs)
            defsByEffect[def.effectName].push_back(&def);

        // Write params grouped by effect
        for (const auto& [effectName, effectParams] : paramsByEffect)
        {
            file << "# " << effectName << "\n";
            auto pathIt = effectPaths.find(effectName);
            if (pathIt != effectPaths.end() && !pathIt->second.empty())
                file << effectName << " = " << quotePath(pathIt->second) << "\n";
            for (const auto* param : effectParams)
                file << param->effectName << "." << param->paramName << " = " << param->value << "\n";
            auto defsIt = defsByEffect.find(effectName);
            if (defsIt != defsByEffect.end())
            {
                // Only persist user-changed definitions. Unchanged defaults must
                // not round-trip through the config: the value parser strips
                // spaces and quote characters, which corrupts string macros
                // (e.g. SuperDepth3D's G_Note) a little more on every session.
                for (const auto* def : defsIt->second)
                    if (def->value != def->defaultValue)
                        file << def->effectName << "@" << def->name << " = " << def->value << "\n";
            }
            file << "\n";
        }

        // Write preprocessor defs for effects that have defs but no params
        for (const auto& [effectName, effectDefs] : defsByEffect)
        {
            if (paramsByEffect.find(effectName) != paramsByEffect.end())
                continue;
            file << "# " << effectName << "\n";
            auto pathIt = effectPaths.find(effectName);
            if (pathIt != effectPaths.end() && !pathIt->second.empty())
                file << effectName << " = " << quotePath(pathIt->second) << "\n";
            for (const auto* def : effectDefs)
                if (def->value != def->defaultValue) // see comment above
                    file << def->effectName << "@" << def->name << " = " << def->value << "\n";
            file << "\n";
        }

        // Write paths for effects that have no params or defs
        for (const auto& [effectName, path] : effectPaths)
        {
            if (!path.empty() && paramsByEffect.find(effectName) == paramsByEffect.end() && defsByEffect.find(effectName) == defsByEffect.end())
            {
                file << "# " << effectName << "\n";
                file << effectName << " = " << quotePath(path) << "\n\n";
            }
        }

        // Write effects list
        file << "effects = " << joinEffects(effects) << "\n";

        file << "disabledEffects = " << joinEffects(disabledEffects) << "\n";

        file.close();

        if (file.fail())
        {
            Logger::err("Failed to write config to: " + tmpPath);
            std::remove(tmpPath.c_str());
            return false;
        }

        // Atomic rename — if this fails, the original file is untouched
        if (std::rename(tmpPath.c_str(), filePath.c_str()) != 0)
        {
            Logger::err("Failed to rename temp config to: " + filePath);
            std::remove(tmpPath.c_str());
            return false;
        }

        return true;
    }

} // namespace vkShade
