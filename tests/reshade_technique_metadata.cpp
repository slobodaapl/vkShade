#include "reshade_parser.hpp"
#include "depth_definitions.hpp"

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <string>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;

    const std::string path   = argv[1];
    auto              result = testShaderCompilation("Multiple", path, {}, {});
    bool              ok     = check(result.success, "The multi-technique shader must compile");
    ok &= check(result.techniques.size() == 3, "Techniques must include the active declarations in source order");
    if (result.techniques.size() == 3)
    {
        const auto& first  = result.techniques[0];
        const auto& second = result.techniques[1];
        const auto& hidden = result.techniques[2];
        ok &= check(first.name == "First" && first.label == "First look" && first.tooltip == "First tooltip",
                    "Technique labels and tooltips must be available");
        ok &= check(second.name == "Second" && second.label == "Second" && second.forcedEnabled && second.noEdit,
                    "Technique flags and default labels must be available");
        ok &= check(hidden.name == "Hidden" && hidden.hidden, "Hidden technique annotations must be available");
    }

    result = testShaderCompilation("Multiple", path, {}, {{"EXTRA_TECHNIQUE", "1"}});
    ok &= check(result.success && result.techniques.size() == 4 && result.techniques.back().name == "Extra",
                "Technique discovery must respect preprocessing definitions");

    result = testShaderCompilation("Missing", path + ".missing", {}, {});
    ok &= check(!result.success && result.techniques.empty(), "Failed shaders must not report techniques");

    const auto localDir    = std::filesystem::path(path).parent_path() / "local-include";
    const auto localShader = (localDir / "nested" / "Local.fx").string();
    Config     config(path + ".missing");
    config.setOverride("reshadeIncludePath", localDir.string());
    auto localParams = parseReshadeEffect("Local", localShader, &config);
    ok &= check(!localParams.empty() && localParams[0]->name == "LocalAmount", "Local include paths must be used when parsing shader parameters");
    result = testShaderCompilation("Local", localShader, {localDir.string()});
    ok &= check(result.success && result.techniques.size() == 1 && result.techniques[0].name == "Local",
                "Local include paths must be used when discovering techniques");
    const auto categoryShader = (std::filesystem::path(path).parent_path() / "category_toggle.fx").string();
    auto       categoryParams = parseReshadeEffect("Category", categoryShader, &config);
    bool       foundToggle    = false;
    bool       foundChild     = false;
    for (const auto& param : categoryParams)
    {
        if (param->name == "ShowPreview")
        {
            foundToggle = param->category == "Preview" && param->categoryToggle && param->clone()->categoryToggle;
        }
        if (param->name == "FlipPreview")
            foundChild = param->category == "Preview" && !param->categoryToggle;
    }
    ok &= check(foundToggle && foundChild, "Category toggles and their child controls must preserve shader annotations");

    const auto depthShader = (std::filesystem::path(path).parent_path() / "depth_alias.fx").string();
    result                 = testShaderCompilation("Depth", depthShader, {}, {});
    ok &= check(result.success && result.techniques.size() == 1 && result.techniques[0].name == "Forward",
                "Native and ReShade depth names must have the same default");
    result = testShaderCompilation("Depth", depthShader, {}, {{"DEPTH_INPUT_IS_REVERSED", "1"}});
    ok &= check(result.success && result.techniques.size() == 1 && result.techniques[0].name == "Reversed",
                "A native depth definition must also set its ReShade alias");
    result = testShaderCompilation("Depth", depthShader, {}, {{"RESHADE_DEPTH_INPUT_IS_REVERSED", "1"}});
    ok &= check(result.success && result.techniques.size() == 1 && result.techniques[0].name == "Reversed",
                "A ReShade depth definition must also set its native alias");
    result = testShaderCompilation("Depth",
                                   depthShader,
                                   {},
                                   {{"TEST_NUMERIC_DEPTH_ALIASES", "1"},
                                    {"DEPTH_INPUT_X_SCALE", "2"},
                                    {"RESHADE_DEPTH_INPUT_Y_SCALE", "3"},
                                    {"DEPTH_INPUT_X_OFFSET", "4"},
                                    {"RESHADE_DEPTH_INPUT_Y_OFFSET", "5"},
                                    {"DEPTH_INPUT_X_PIXEL_OFFSET", "6"},
                                    {"RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", "7"},
                                    {"DEPTH_LINEARIZATION_FAR_PLANE", "8"},
                                    {"RESHADE_DEPTH_MULTIPLIER", "9"}});
    ok &= check(result.success, "Numeric depth settings must match through both naming conventions");
    const auto depthDefs   = extractPreprocessorDefinitions("Depth", depthShader, &config);
    bool       foundLocal  = false;
    bool       foundGlobal = false;
    for (const auto& def : depthDefs)
    {
        foundLocal |= def.name == "LOCAL_OPTION";
        foundGlobal |= def.name == "DEPTH_INPUT_IS_REVERSED" || def.name == "RESHADE_DEPTH_INPUT_IS_REVERSED" || def.name == "DEPTH_INPUT_X_SCALE"
                       || def.name == "RESHADE_DEPTH_INPUT_X_SCALE";
    }
    ok &= check(foundLocal && !foundGlobal, "Shared depth definitions must not appear as per-effect options");
    const auto findDepth = [](std::string_view name) -> const DepthDefinition* {
        const auto found =
            std::find_if(depthDefinitions.begin(), depthDefinitions.end(), [name](const auto& definition) { return definition.name == name; });
        return found == depthDefinitions.end() ? nullptr : &*found;
    };
    const auto* scale    = findDepth("DEPTH_INPUT_X_SCALE");
    const auto* offset   = findDepth("DEPTH_INPUT_X_PIXEL_OFFSET");
    const auto* farPlane = findDepth("DEPTH_LINEARIZATION_FAR_PLANE");
    ok &= check(scale && validDepthDefinitionValue(*scale, "-1.25") && !validDepthDefinitionValue(*scale, "0")
                    && !validDepthDefinitionValue(*scale, "nan") && !validDepthDefinitionValue(*scale, "1e100")
                    && !validDepthDefinitionValue(*scale, ""),
                "Scale edits must accept finite nonzero values and reject invalid or empty values");
    ok &= check(offset && farPlane && validDepthDefinitionValue(*offset, "-3") && !validDepthDefinitionValue(*offset, "1.5")
                    && validDepthDefinitionValue(*farPlane, "1000") && !validDepthDefinitionValue(*farPlane, "0"),
                "Pixel offsets must be integers and the far plane must be positive");
    return ok ? 0 : 1;
}
