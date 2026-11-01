#include "preprocessor_edit.hpp"

#include "imgui/imgui.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

namespace vkShade
{
    std::optional<std::string> renderPreprocessorInput(const PreprocessorDefinition& definition, bool pending)
    {
        std::vector<char> value(std::max<size_t>(256, definition.value.size() + 1), '\0');
        std::memcpy(value.data(), definition.value.data(), definition.value.size());
        ImGui::BeginDisabled(pending);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        const bool entered  = ImGui::InputText(definition.name.c_str(), value.data(), value.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        const bool finished = entered || ImGui::IsItemDeactivatedAfterEdit();
        const bool hovered  = ImGui::IsItemHovered();
        bool       reset    = false;
        if (ImGui::BeginPopupContextItem("##preproc_reset"))
        {
            reset = ImGui::MenuItem("Reset to default");
            ImGui::EndPopup();
        }
        ImGui::EndDisabled();
        if (hovered)
            ImGui::SetTooltip("Default: %s\nRight-click to reset", definition.defaultValue.c_str());
        if (definition.value != definition.defaultValue)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(modified)");
        }
        if (reset && definition.value != definition.defaultValue)
            return definition.defaultValue;
        if (!pending && finished && definition.value != value.data())
            return std::string(value.data());
        return std::nullopt;
    }

    std::optional<std::string>
    renderDepthDefinitionInput(const DepthDefinition& definition, const char* label, const std::string& value, bool pending, std::string& notice)
    {
        if (definition.type == DepthDefinitionType::Flag && (value == "0" || value == "1"))
        {
            bool enabled = value == "1";
            ImGui::BeginDisabled(pending);
            const bool changed = ImGui::Checkbox(label, &enabled);
            ImGui::EndDisabled();
            if (changed)
            {
                notice.clear();
                return enabled ? "1" : "0";
            }
            return std::nullopt;
        }
        PreprocessorDefinition field;
        field.name         = label;
        field.value        = value;
        field.defaultValue = std::string(definition.defaultValue);
        if (auto edited = renderPreprocessorInput(field, pending))
        {
            const auto first   = edited->find_first_not_of(" \t");
            const auto last    = edited->find_last_not_of(" \t");
            const auto trimmed = first == std::string::npos ? std::string{} : edited->substr(first, last - first + 1);
            if (validDepthDefinitionValue(definition, trimmed))
            {
                notice.clear();
                return trimmed;
            }
            notice = std::string("Invalid value for ") + label + "; the previous value was kept.";
        }
        return std::nullopt;
    }

    bool PreprocessorEditValidator::start(PreprocessorEdit edit)
    {
        if (current || edit.request.path.empty() || !edit.request.profileConfig.empty() || !edit.request.baseConfig.empty()
            || std::none_of(edit.value.begin(), edit.value.end(), [](unsigned char c) { return !std::isspace(c); }))
            return false;
        auto& definitions = edit.request.defines;
        definitions.erase(std::remove_if(definitions.begin(), definitions.end(), [&](const auto& item) { return item.first == edit.macroName; }),
                          definitions.end());
        if (edit.value != edit.defaultValue)
            definitions.emplace_back(edit.macroName, edit.value);
        current = std::move(edit);
        loader.request(current->request, true);
        return true;
    }

    std::optional<PreprocessorEditResult> PreprocessorEditValidator::poll()
    {
        if (!current)
            return std::nullopt;
        loader.poll();
        const auto* checked = loader.result(current->request);
        if (!checked)
            return std::nullopt;
        const bool             valid = checked->success && !checked->techniques.empty();
        const auto             error = checked->success && checked->techniques.empty() ? "Shader has no techniques" : checked->errorMessage;
        PreprocessorEditResult result{std::move(*current), valid, error};
        current.reset();
        loader.clear();
        return result;
    }

    void PreprocessorEditValidator::cancel()
    {
        current.reset();
        loader.clear();
    }
} // namespace vkShade
