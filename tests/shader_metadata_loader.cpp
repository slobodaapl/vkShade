#include "overlay/shader_metadata_loader.hpp"
#include "shader_metadata.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace vkShade;

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    const std::string     path = argv[1];
    ShaderMetadataRequest request{path, {std::filesystem::path(path).parent_path().string()}, {{"EXTRA_TECHNIQUE", "1"}}};
    ShaderMetadataRequest missing{path + ".missing", request.includes, {}};
    std::string           tempPattern = (std::filesystem::current_path() / "shader-metadata-XXXXXX").string();
    std::vector<char>     tempName(tempPattern.begin(), tempPattern.end());
    tempName.push_back('\0');
    const char* tempDir = mkdtemp(tempName.data());
    if (!tempDir || setenv("XDG_CONFIG_HOME", tempDir, 1) != 0)
        return 1;
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::filesystem::remove_all(path);
        }
    } cleanup{tempDir};
    const auto configPath = std::filesystem::path(tempDir) / "profile.conf";
    std::ofstream(configPath) << "Alias@EXTRA_TECHNIQUE = 1\nDEPTH_INPUT_IS_REVERSED = 1\n";
    ShaderMetadataRequest configured{path, request.includes, {}, "Alias", configPath.string(), ""};
    const auto            depthPath = (std::filesystem::path(path).parent_path() / "depth_alias.fx").string();
    ShaderMetadataRequest globalDepth{depthPath, request.includes, {}, "Depth", configPath.string(), ""};
    ShaderMetadataRequest overrideDepth{depthPath, request.includes, {{"RESHADE_DEPTH_INPUT_IS_REVERSED", "0"}}, "Depth", configPath.string(), ""};
    const auto            gameDepthDir = std::filesystem::path(tempDir) / "vkShade/configs/.depth/command/game";
    std::filesystem::create_directories(gameDepthDir);
    std::ofstream(gameDepthDir / "settings.conf") << "DEPTH_INPUT_IS_REVERSED = 0\n";
    ShaderMetadataRequest gameDepth{depthPath, request.includes, {}, "Depth", configPath.string(), "", "command/game"};
    ShaderMetadataRequest gameOverride{
        depthPath, request.includes, {{"DEPTH_INPUT_IS_REVERSED", "1"}}, "Depth", configPath.string(), "", "command/game"};
    ShaderMetadataLoader loader;
    loader.request(request);
    loader.request(request);
    loader.request(missing);
    loader.request(configured);
    loader.request(globalDepth);
    loader.request(overrideDepth);
    loader.request(gameDepth);
    loader.request(gameOverride);
    if (loader.pending() != 7)
        return 2;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (loader.pending() && std::chrono::steady_clock::now() < deadline)
    {
        loader.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto* result           = loader.result(request);
    const auto* failure          = loader.result(missing);
    const auto* withConfig       = loader.result(configured);
    const auto* withDepth        = loader.result(globalDepth);
    const auto* withOverride     = loader.result(overrideDepth);
    const auto* withGame         = loader.result(gameDepth);
    const auto* withGameOverride = loader.result(gameOverride);
    if (!result || !result->success || result->techniques.size() != 4 || result->techniques[0].name != "First"
        || result->techniques[3].name != "Extra" || !result->techniques[1].forcedEnabled || !result->techniques[2].hidden || !failure
        || failure->success || failure->errorMessage.empty() || !withConfig || !withConfig->success || withConfig->techniques.size() != 4
        || withConfig->techniques.back().name != "Extra" || !withDepth || !withDepth->success || withDepth->techniques.size() != 1
        || withDepth->techniques[0].name != "Reversed" || !withOverride || !withOverride->success || withOverride->techniques.size() != 1
        || withOverride->techniques[0].name != "Forward" || !withGame || !withGame->success || withGame->techniques.size() != 1
        || withGame->techniques[0].name != "Forward" || !withGameOverride || !withGameOverride->success || withGameOverride->techniques.size() != 1
        || withGameOverride->techniques[0].name != "Reversed")
    {
        std::cerr << "Shader helper did not return the expected techniques and failure diagnostic\n";
        return 3;
    }
    const auto encoded = encodeShaderMetadata(*result);
    if (!decodeShaderMetadata(encoded) || decodeShaderMetadata(encoded.substr(0, encoded.size() - 1)) || decodeShaderMetadata(encoded + 'x'))
        return 4;
    loader.clear();
    if (loader.result(request) || loader.pending())
        return 5;
    return 0;
}
