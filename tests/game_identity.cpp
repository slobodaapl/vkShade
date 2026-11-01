#include "config_serializer.hpp"
#include "config.hpp"
#include "game_identity.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main(int argc, char** argv)
{
    if (std::getenv("VKSHADE_TEST_JAVA_IDENTITY"))
        return ConfigSerializer::detectGameName() == "java-main/net.minecraft.client.main.Main" ? 0 : 1;
    if (argc != 2)
        return 1;
    std::string       directory = std::string(argv[1]) + "/game-identity-XXXXXX";
    std::vector<char> buffer(directory.begin(), directory.end());
    buffer.push_back('\0');
    const char* root = mkdtemp(buffer.data());
    if (!root || setenv("XDG_CONFIG_HOME", root, 1) != 0)
        return 1;
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::filesystem::remove_all(path);
        }
    } cleanup{root};
    unsetenv("SteamAppId");
    unsetenv("SteamGameId");

    const std::vector<std::string> javaArgs = {"/usr/bin/java",
                                               "-Xmx4G",
                                               "--add-opens",
                                               "java.base/java.lang=ALL-UNNAMED",
                                               "-cp",
                                               "classes",
                                               "net.minecraft.client.main.Main",
                                               "--accessToken",
                                               "private-token"};
    const auto                     java     = gameIdentity("/usr/bin/java", "", javaArgs, "/games");
    bool                           ok = check(java == "java-main/net.minecraft.client.main.Main" && java.find("private-token") == std::string::npos,
                                              "Java games must use their main class without recording game arguments");
    const auto                     otherJava = gameIdentity("/usr/bin/java", "", {"java", "-cp", "classes", "org.example.OtherGame"}, "/games");
    ok &= check(otherJava != java, "Different Java games must not share settings just because they use the same JVM");
    const auto jarOne = gameIdentity("/usr/bin/java", "", {"java", "-jar", "one/game.jar", "--token", "secret"}, "/games");
    const auto jarTwo = gameIdentity("/usr/bin/java", "", {"java", "-jar", "two/game.jar"}, "/games");
    ok &=
        check(jarOne != jarTwo && jarOne.find("secret") == std::string::npos, "Java archives must be identified by their paths, not game arguments");
    const auto first  = gameIdentity("/games/first/game", "", {"/games/first/game"}, "/games");
    const auto second = gameIdentity("/games/second/game", "", {"/games/second/game"}, "/games");
    ok &= check(!first.empty() && first != second, "Commands with the same basename in different directories must have separate settings");
    ok &= check(gameIdentity("/compat/proton", "12345", javaArgs, "/games") == "steam/12345", "Steam app identity must survive launcher chains");
    ok &= check(gameIdentity("/games/first/game", "invalid", {}, "/games") == first, "Invalid Steam IDs must fall back to the wrapped command");

    const auto legacyDefault = ConfigSerializer::ensureGameProfile("java");
    ok &= check(!legacyDefault.empty(), "The legacy Java profile must be created");
    std::ofstream(legacyDefault) << "effects = legacy\n";
    ok &= check(ConfigSerializer::createProfile("java", "quality", "default"), "The legacy named profile must be created");
    const auto legacyNamed = ConfigSerializer::getProfilePath("java", "quality");
    std::ofstream(legacyNamed, std::ios::app) << "DEPTH_INPUT_IS_REVERSED = 1\n";
    ConfigSerializer::setActiveProfile("java", "quality");
    ConfigSerializer::migrateGameProfiles(java, "java");
    const auto migrated = ConfigSerializer::listProfilesForGame(java);
    ok &= check(migrated == std::vector<std::string>({"default", "quality"}) && ConfigSerializer::getActiveProfile(java) == "quality",
                "Existing profiles and their active selection must migrate automatically");
    auto settings = ConfigSerializer::loadProfileSettings(ConfigSerializer::getProfilePath(java, "quality"));
    ok &= check(settings.depthDefinitions["DEPTH_INPUT_IS_REVERSED"] == "1" && std::filesystem::exists(legacyNamed),
                "Migration must preserve settings and leave the old profile intact");
    std::ofstream(ConfigSerializer::getProfilePath(java, "quality"), std::ios::app) << "DEPTH_INPUT_IS_UPSIDE_DOWN = 1\n";
    ConfigSerializer::migrateGameProfiles(java, "java");
    settings = ConfigSerializer::loadProfileSettings(ConfigSerializer::getProfilePath(java, "quality"));
    ok &= check(settings.depthDefinitions["DEPTH_INPUT_IS_UPSIDE_DOWN"] == "1", "Later launches must not overwrite an existing game profile");

    auto gameDepth = ConfigSerializer::loadGameDepthSettings(java);
    ok &= check(gameDepth.depthDefinitions["DEPTH_INPUT_IS_REVERSED"] == "1" && gameDepth.depthDefinitions["DEPTH_INPUT_IS_UPSIDE_DOWN"] == "1",
                "Legacy depth values must migrate into the game's shared settings");
    ok &= check(ConfigSerializer::saveGameDepthSettings(java, gameDepth.depthDefinitions, gameDepth.conflictingDepthDefinitions),
                "The game's shared depth settings must save");
    std::ofstream(ConfigSerializer::getGameDepthPath(java), std::ios::app) << "FUTURE_GLOBAL = 7\n";
    Config defaultGame(ConfigSerializer::getProfilePath(java, "default"));
    Config namedGame(ConfigSerializer::getProfilePath(java, "quality"));
    defaultGame.setGameName(java);
    namedGame.setGameName(java);
    gameDepth.depthDefinitions["DEPTH_INPUT_IS_REVERSED"] = "0";
    ok &= check(ConfigSerializer::saveGameDepthSettings(java, gameDepth.depthDefinitions, {}), "The game's depth setting must update");
    Config sharedConfig(ConfigSerializer::getGameDepthPath(java));
    ok &= check(sharedConfig.getOption<std::string>("FUTURE_GLOBAL") == "7", "Saving depth settings must preserve unrelated entries");
    ok &= check(defaultGame.hasConfigChanged(), "Changes to shared game settings must trigger a reload");
    const auto defaultDepth = ConfigSerializer::loadDepthDefinitions(&defaultGame);
    const auto namedDepth   = ConfigSerializer::loadDepthDefinitions(&namedGame);
    ok &= check(defaultDepth.count("DEPTH_INPUT_IS_REVERSED") && namedDepth.count("DEPTH_INPUT_IS_REVERSED")
                    && defaultDepth.at("DEPTH_INPUT_IS_REVERSED") == "0" && namedDepth.at("DEPTH_INPUT_IS_REVERSED") == "0",
                "Every effect profile for a game must use its shared depth setting");
    ok &= check(ConfigSerializer::getGameDepthPath(java) != ConfigSerializer::getGameDepthPath(otherJava),
                "Different games must not share their depth settings file");

    const auto conflictGame    = gameIdentity("/games/conflict/game", "", {}, "/games");
    const auto conflictDefault = ConfigSerializer::ensureGameProfile(conflictGame);
    std::ofstream(conflictDefault) << "A@RESHADE_DEPTH_INPUT_IS_REVERSED = 0\n";
    ok &= check(ConfigSerializer::createProfile(conflictGame, "other"), "A second effect profile must be created");
    std::ofstream(ConfigSerializer::getProfilePath(conflictGame, "other")) << "B@RESHADE_DEPTH_INPUT_IS_REVERSED = 1\n";
    auto gameConflict = ConfigSerializer::loadGameDepthSettings(conflictGame);
    ok &= check(gameConflict.conflictingDepthDefinitions.count("DEPTH_INPUT_IS_REVERSED") == 1,
                "Conflicting values across profiles must require a shared choice");
    ok &= check(ConfigSerializer::saveGameDepthSettings(conflictGame, gameConflict.depthDefinitions, gameConflict.conflictingDepthDefinitions),
                "Unresolved game settings must save without discarding conflicts");
    gameConflict = ConfigSerializer::loadGameDepthSettings(conflictGame);
    ok &= check(gameConflict.conflictingDepthDefinitions.count("DEPTH_INPUT_IS_REVERSED") == 1,
                "Conflicting profile values must remain available until resolved");
    gameConflict.depthDefinitions["DEPTH_INPUT_IS_REVERSED"] = "1";
    gameConflict.conflictingDepthDefinitions.clear();
    ok &= check(ConfigSerializer::saveGameDepthSettings(conflictGame, gameConflict.depthDefinitions, gameConflict.conflictingDepthDefinitions),
                "A shared choice must save");
    gameConflict = ConfigSerializer::loadGameDepthSettings(conflictGame);
    ok &= check(gameConflict.conflictingDepthDefinitions.empty() && gameConflict.depthDefinitions["DEPTH_INPUT_IS_REVERSED"] == "1",
                "A resolved game value must override all legacy per-profile values");

    int start[2];
    if (pipe(start) != 0)
        return 1;
    std::vector<pid_t> children;
    for (int i = 0; i < 8; ++i)
    {
        const pid_t child = fork();
        if (child == 0)
        {
            close(start[1]);
            char ready;
            if (read(start[0], &ready, 1) != 1)
                _exit(1);
            ConfigSerializer::setActiveProfile("steam/" + std::to_string(i + 1), "quality");
            _exit(0);
        }
        if (child < 0)
        {
            ok = false;
            break;
        }
        children.push_back(child);
    }
    close(start[0]);
    for (size_t i = 0; i < children.size(); ++i)
        ok &= check(write(start[1], "x", 1) == 1, "Could not start a profile writer");
    close(start[1]);
    for (const auto child : children)
    {
        int status = 0;
        ok &= check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "A concurrent profile writer failed");
    }
    for (size_t i = 0; i < children.size(); ++i)
        ok &= check(ConfigSerializer::getActiveProfile("steam/" + std::to_string(i + 1)) == "quality",
                    "Concurrent games must retain each other's active-profile choices");

    setenv("VKSHADE_WRAPPED_COMMAND", "/games/first/game", 1);
    setenv("VKSHADE_WRAPPED_CWD", "/games", 1);
    ok &= check(ConfigSerializer::detectGameName() == first, "The wrapped command must select its game profile automatically");
    const pid_t javaProbe = fork();
    if (javaProbe == 0)
    {
        setenv("VKSHADE_TEST_JAVA_IDENTITY", "1", 1);
        setenv("VKSHADE_WRAPPED_COMMAND", "/usr/bin/java", 1);
        execl("/proc/self/exe", "java", "-cp", "classes", "net.minecraft.client.main.Main", "--accessToken", "private-token", nullptr);
        _exit(1);
    }
    int javaStatus = 0;
    ok &= check(javaProbe > 0 && waitpid(javaProbe, &javaStatus, 0) == javaProbe && WIFEXITED(javaStatus) && WEXITSTATUS(javaStatus) == 0,
                "The running Java command must automatically select its game identity");
    setenv("SteamAppId", "12345", 1);
    ok &= check(ConfigSerializer::detectGameName() == "steam/12345", "Steam app ID must take precedence over intermediary launchers");
    return ok ? 0 : 1;
}
