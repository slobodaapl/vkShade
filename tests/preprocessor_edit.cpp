#include "overlay/preprocessor_edit.hpp"
#include "imgui/imgui.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <thread>

using namespace vkShade;

static bool check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << message << '\n';
    return condition;
}

int main(int argc, char** argv)
{
    if (argc != 3)
        return 1;
    ImGui::CreateContext();
    ImGuiIO& io                     = ImGui::GetIO();
    io.IniFilename                  = nullptr;
    io.DisplaySize                  = ImVec2(800, 600);
    io.DeltaTime                    = 1.0f / 60.0f;
    io.ConfigInputTrickleEventQueue = false;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    PreprocessorDefinition    definition{"EXTRA_TECHNIQUE", "0", "0", "Example"};
    PreprocessorEditValidator validator;
    ImVec2                    inputPosition(0, 0);
    const DepthDefinition*    sharedDefinition = nullptr;
    std::string               sharedValue;
    std::string               sharedNotice;
    bool                      sharedPending = false;
    auto                      frame         = [&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(600, 400));
        ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        auto result = sharedDefinition ? renderDepthDefinitionInput(*sharedDefinition, "Shared depth", sharedValue, sharedPending, sharedNotice)
                                       : renderPreprocessorInput(definition, validator.pending());
        if (inputPosition.x == 0)
        {
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            inputPosition        = ImVec2(minimum.x + ImGui::GetFontSize() * 3.0f, (minimum.y + maximum.y) * 0.5f);
        }
        ImGui::End();
        ImGui::Render();
        return result;
    };
    auto click = [&](ImVec2 position) {
        io.AddMousePosEvent(position.x, position.y);
        io.AddMouseButtonEvent(0, true);
        auto result = frame();
        io.AddMouseButtonEvent(0, false);
        auto released = frame();
        return result ? result : released;
    };
    auto selectAll = [&]() {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        frame();
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
    };
    auto press = [&](ImGuiKey key) {
        io.AddKeyEvent(key, true);
        auto result = frame();
        io.AddKeyEvent(key, false);
        auto released = frame();
        return result ? result : released;
    };
    auto makeEdit = [&](const std::string& value) {
        PreprocessorEdit edit;
        edit.effectName       = definition.effectName;
        edit.macroName        = definition.name;
        edit.value            = value;
        edit.originalValue    = definition.value;
        edit.defaultValue     = definition.defaultValue;
        edit.request.path     = argv[1];
        edit.request.includes = {std::filesystem::path(argv[1]).parent_path().string()};
        return edit;
    };
    auto validate = [&]() -> std::optional<PreprocessorEditResult> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto result = validator.poll())
                return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::nullopt;
    };

    bool ok = true;
    frame();
    click(inputPosition);
    selectAll();
    ok &= check(!press(ImGuiKey_Backspace) && definition.value == "0", "Deleting a draft must not change the committed value");
    const auto empty = click(ImVec2(500, 300));
    ok &= check(empty && empty->empty(), "Leaving an empty edit must submit the empty draft for validation");
    if (empty)
        ok &= check(!validator.start(makeEdit(*empty)) && definition.value == "0", "An empty value must be rejected without changing the macro");

    click(inputPosition);
    selectAll();
    io.AddInputCharactersUTF8("1");
    ok &= check(!frame() && definition.value == "0", "Typing a valid draft must not commit it");
    const auto valid = press(ImGuiKey_Enter);
    ok &= check(valid && *valid == "1", "Enter must submit the complete draft");
    if (valid)
    {
        ok &= check(validator.start(makeEdit(*valid)) && definition.value == "0", "Validation must not change the current macro early");
        auto result = validate();
        ok &= check(result && result->valid, "A valid macro must pass shader validation");
        if (result && result->valid)
            definition.value = result->edit.value;
    }

    frame();
    click(inputPosition);
    selectAll();
    io.AddInputCharactersUTF8("(");
    ok &= check(!frame() && definition.value == "1", "An invalid draft must not affect the current macro");
    const auto invalid = press(ImGuiKey_Enter);
    ok &= check(invalid && *invalid == "(", "Enter must submit the invalid draft for checking");
    if (invalid)
    {
        ok &= check(validator.start(makeEdit(*invalid)), "A nonempty draft must reach the shader checker");
        auto result = validate();
        ok &= check(result && !result->valid && definition.value == "1", "A shader compilation failure must preserve the prior value");
    }

    frame();
    click(inputPosition);
    selectAll();
    io.AddInputCharactersUTF8("0");
    ok &= check(!frame() && definition.value == "1", "A new draft must remain separate while the field is focused");
    const auto restored = click(ImVec2(500, 300));
    ok &= check(restored && *restored == "0", "Leaving the field must submit a valid draft");
    if (restored)
    {
        ok &= check(validator.start(makeEdit(*restored)), "A restored value must be checked");
        const auto result = validate();
        ok &= check(result && result->valid && result->edit.value == "0", "A valid default must be accepted after a failed edit");
    }

    PreprocessorEdit withoutTechnique;
    withoutTechnique.macroName        = "ENABLE_TECHNIQUE";
    withoutTechnique.value            = "1";
    withoutTechnique.originalValue    = "1";
    withoutTechnique.defaultValue     = "1";
    withoutTechnique.request.path     = argv[2];
    withoutTechnique.request.includes = {std::filesystem::path(argv[2]).parent_path().string()};
    ok &= check(validator.start(withoutTechnique), "The original macro must reach shader validation");
    const auto withTechnique = validate();
    ok &= check(withTechnique && withTechnique->valid, "The original shader must have a usable technique");
    withoutTechnique.value = "0";
    ok &= check(validator.start(withoutTechnique), "A nonempty macro must reach shader validation");
    const auto noTechnique = validate();
    ok &= check(noTechnique && !noTechnique->valid && noTechnique->error == "Shader has no techniques",
                "A shader with no techniques must not replace a usable value");

    const auto findDepth = [](std::string_view name) -> const DepthDefinition* {
        const auto found = std::find_if(depthDefinitions.begin(), depthDefinitions.end(), [name](const auto& item) { return item.name == name; });
        return found == depthDefinitions.end() ? nullptr : &*found;
    };
    sharedDefinition = findDepth("DEPTH_INPUT_IS_REVERSED");
    if (!sharedDefinition)
        return 1;
    sharedValue   = "0";
    inputPosition = ImVec2(0, 0);
    frame();
    const auto toggled = click(inputPosition);
    ok &= check(toggled && *toggled == "1" && sharedValue == "0", "The shared reversal checkbox must return one change without mutating its input");
    if (toggled)
        sharedValue = *toggled;
    sharedPending = true;
    frame();
    ok &= check(!click(inputPosition) && sharedValue == "1", "Shared depth controls must remain disabled while another edit is checked");

    sharedDefinition = findDepth("DEPTH_INPUT_X_SCALE");
    if (!sharedDefinition)
        return 1;
    sharedValue   = "1.0";
    sharedPending = false;
    inputPosition = ImVec2(0, 0);
    frame();
    click(inputPosition);
    selectAll();
    ok &= check(!press(ImGuiKey_Backspace) && sharedValue == "1.0", "Deleting a shared value must leave the committed value unchanged");
    ok &= check(!press(ImGuiKey_Enter) && sharedValue == "1.0" && !sharedNotice.empty(), "An empty shared value must revert with a notice");
    frame();
    click(inputPosition);
    selectAll();
    io.AddInputCharactersUTF8("2.0");
    ok &= check(!frame() && sharedValue == "1.0", "A shared numeric draft must not apply while typing");
    const auto changedScale = click(ImVec2(500, 300));
    ok &= check(changedScale && *changedScale == "2.0" && sharedValue == "1.0" && sharedNotice.empty(),
                "Leaving a valid shared numeric edit must return the complete value");
    ImGui::DestroyContext();
    return ok ? 0 : 1;
}
