#include "imgui_overlay.hpp"

#include "effects/effect_registry.hpp"
#include "params/field_editor.hpp"
#include "technique_id.hpp"
#include "technique_order.hpp"
#include "technique_plan.hpp"
#include "logger.hpp"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <unordered_map>

namespace vkShade
{
    namespace
    {
        constexpr const char* reorderPayload = "VKSHADE_TECHNIQUE_REORDER";

        struct Row
        {
            TechniqueId id{};
            std::string label;
            std::string path;
            std::string tooltip;
            std::string error;
            bool        hidden = false;
            bool        forced = false;
            bool        noEdit = false;
            bool        depth  = false;
        };

        std::string canonicalPath(const std::string& path)
        {
            std::error_code ec;
            auto            canonical = std::filesystem::canonical(path, ec);
            return ec ? path : canonical.string();
        }

        bool contains(const std::vector<std::string>& items, const std::string& value)
        {
            return std::find(items.begin(), items.end(), value) != items.end();
        }

        std::string makeInstanceName(const std::string&                        proposed,
                                     const std::vector<std::string>&           selected,
                                     const std::map<std::string, std::string>& reserved,
                                     const std::string&                        claimed = "")
        {
            std::string base;
            for (unsigned char c : proposed)
                base += std::isalnum(c) || c == '_' || c == '-' || c == '.' ? static_cast<char>(c) : '_';
            if (base.empty())
                base = "shader";
            std::string result = base;
            for (size_t suffix = 2; contains(selected, result) || (reserved.count(result) && result != claimed); ++suffix)
                result = base + '.' + std::to_string(suffix);
            return result;
        }

        std::vector<TechniqueId> decodeIds(const std::vector<std::string>& entries)
        {
            std::vector<TechniqueId> result;
            for (const auto& entry : entries)
                if (auto id = decodeTechniqueId(entry))
                    result.push_back(std::move(*id));
            return result;
        }

        std::vector<std::string> encodeIds(const std::vector<TechniqueId>& entries)
        {
            std::vector<std::string> result;
            for (const auto& entry : entries)
                result.push_back(encodeTechniqueId(entry));
            return result;
        }

        bool categoryToggleEnabled(const EffectParam& param)
        {
            switch (param.getType())
            {
                case ParamType::Bool: return static_cast<const BoolParam&>(param).value;
                case ParamType::Int: return static_cast<const IntParam&>(param).value != 0;
                case ParamType::Uint: return static_cast<const UintParam&>(param).value != 0;
                case ParamType::Float: return static_cast<const FloatParam&>(param).value != 0;
                case ParamType::IntVec: return static_cast<const IntVecParam&>(param).value[0] != 0;
                case ParamType::UintVec: return static_cast<const UintVecParam&>(param).value[0] != 0;
                case ParamType::FloatVec: return static_cast<const FloatVecParam&>(param).value[0] != 0;
            }
            return false;
        }
    } // namespace

    void ImGuiOverlay::queuePreprocessorEdit(const std::string& effectName, const PreprocessorDefinition& definition, const std::string& value)
    {
        if (value == definition.value)
            return;
        if (std::none_of(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }))
        {
            techniqueNotice = "The preprocessor value is empty; the previous value was kept.";
            return;
        }
        PreprocessorEdit edit;
        edit.effectName         = effectName;
        edit.macroName          = definition.name;
        edit.value              = value;
        edit.originalValue      = definition.value;
        edit.defaultValue       = definition.defaultValue;
        edit.profilePath        = state.configPath;
        edit.request.path       = pEffectRegistry->getEffectFilePath(effectName);
        edit.request.effectName = effectName;
        edit.request.includes   = state.shaderIncludePaths;
        edit.request.defines    = {{"BUFFER_WIDTH", std::to_string(currentWidth)},
                                   {"BUFFER_HEIGHT", std::to_string(currentHeight)},
                                   {"BUFFER_COLOR_DEPTH", swapchainFormat == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? "10" : "8"}};
        for (const auto& [name, globalValue] : effectiveDepthDefinitions())
            edit.request.defines.emplace_back(name, globalValue);
        for (const auto& existing : pEffectRegistry->getPreprocessorDefs(effectName))
            if (existing.name != definition.name && existing.value != existing.defaultValue)
                edit.request.defines.emplace_back(existing.name, existing.value);
        if (!preprocessorEdit.start(std::move(edit)))
            techniqueNotice = "Could not check the preprocessor value; the previous value was kept.";
        else
            techniqueNotice.clear();
    }

    void ImGuiOverlay::pollPreprocessorEdit()
    {
        if (!preprocessorEdit.pending())
            return;
        if (!pendingConfigPath.empty())
        {
            preprocessorEdit.cancel();
            return;
        }
        auto checked = preprocessorEdit.poll();
        if (!checked || !pEffectRegistry || checked->edit.profilePath != state.configPath
            || pEffectRegistry->getEffectFilePath(checked->edit.effectName) != checked->edit.request.path)
            return;
        const auto& definitions = pEffectRegistry->getPreprocessorDefs(checked->edit.effectName);
        const auto  definition  = std::find_if(definitions.begin(), definitions.end(), [&](const auto& def) {
            return def.name == checked->edit.macroName && def.value == checked->edit.originalValue;
        });
        if (definition == definitions.end())
            return;
        if (!checked->valid)
        {
            techniqueNotice = "Invalid value for " + checked->edit.macroName + "; the previous value was kept.";
            Logger::warn("Preprocessor validation failed for " + checked->edit.effectName + ": " + checked->error);
            return;
        }
        pEffectRegistry->setPreprocessorDefValue(checked->edit.effectName, checked->edit.macroName, checked->edit.value);
        techniqueNotice.clear();
        paramsDirty    = true;
        profileDirty   = true;
        lastChangeTime = std::chrono::steady_clock::now();
        if (!visible && settingsManager.getAutoApply())
        {
            paramsDirty    = false;
            applyRequested = true;
            autoSaveProfile();
        }
    }

    void ImGuiOverlay::renderTechniqueList()
    {
        auto       selected    = pEffectRegistry->getSelectedEffects();
        const auto depthValues = effectiveDepthDefinitions();
        auto       requestFor  = [&](const std::string& path, const std::string& instance, bool configured = false) {
            ShaderMetadataRequest request;
            request.path     = path;
            request.gameName = activeGameName;
            request.includes = state.shaderIncludePaths;
            request.defines  = {{"BUFFER_WIDTH", std::to_string(currentWidth)},
                                {"BUFFER_HEIGHT", std::to_string(currentHeight)},
                                {"BUFFER_COLOR_DEPTH", swapchainFormat == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? "10" : "8"}};
            for (const auto& [name, value] : depthValues)
                request.defines.emplace_back(name, value);
            if (configured)
            {
                request.effectName    = instance;
                request.profileConfig = state.configPath;
                request.baseConfig    = state.baseConfigPath;
            }
            else if (!instance.empty())
                for (const auto& def : pEffectRegistry->getPreprocessorDefs(instance))
                    if (def.value != def.defaultValue)
                        request.defines.emplace_back(def.name, def.value);
            return request;
        };

        std::set<std::string> selectedPaths;
        std::set<std::string> configuredPaths;
        for (const auto& instance : selected)
        {
            std::string path = pEffectRegistry->getEffectFilePath(instance);
            if (path.empty())
            {
                auto found = state.effectPaths.find(instance);
                if (found != state.effectPaths.end())
                    path = found->second;
            }
            if (!path.empty())
            {
                path = canonicalPath(path);
                selectedPaths.insert(path);
                shaderMetadata.request(requestFor(path, instance), true);
            }
        }
        for (const auto& [name, path] : state.configuredEffectPaths)
        {
            if (contains(selected, name) || path.empty())
                continue;
            const auto canonical = canonicalPath(path);
            configuredPaths.insert(canonical);
            shaderMetadata.request(requestFor(canonical, name, true));
        }
        for (const auto& path : state.effectFiles)
            if (!selectedPaths.count(path) && !configuredPaths.count(path))
                shaderMetadata.request(requestFor(path, ""));
        shaderMetadata.poll();

        std::vector<Row>              rows;
        bool                          selectedReady = true;
        std::map<std::string, size_t> filenameCounts;
        for (const auto& file : state.effectFiles)
            ++filenameCounts[std::filesystem::path(file).filename().string()];
        auto appendFile = [&](const TechniqueOwner owner,
                              const std::string&   identity,
                              const std::string&   path,
                              const std::string&   instance,
                              bool                 selectedFile,
                              bool                 configured) {
            const auto* metadata = path.empty() ? nullptr : shaderMetadata.result(requestFor(path, instance, configured));
            if (!metadata && !path.empty())
            {
                if (selectedFile)
                    selectedReady = false;
                return;
            }
            if (!metadata || !metadata->success || metadata->techniques.empty())
            {
                Row row{{owner, identity, ""}, instance.empty() ? std::filesystem::path(path).filename().string() : instance, path};
                row.error = !metadata ? "Shader file not found" : (metadata->success ? "No techniques found" : metadata->errorMessage);
                rows.push_back(std::move(row));
                return;
            }
            for (const auto& technique : metadata->techniques)
            {
                Row row;
                row.id    = {owner, identity, technique.name};
                row.label = technique.label + " [" + std::filesystem::path(path).filename().string();
                if (!instance.empty() && instance != std::filesystem::path(path).stem().string())
                    row.label += ", " + instance;
                row.label += ']';
                if (filenameCounts[std::filesystem::path(path).filename().string()] > 1)
                    row.label += " (" + std::filesystem::path(path).parent_path().string() + ')';
                row.path    = path;
                row.tooltip = technique.tooltip;
                row.hidden  = technique.hidden;
                row.forced  = technique.forcedEnabled;
                row.noEdit  = technique.noEdit;
                row.depth   = metadata->usesDepth;
                rows.push_back(std::move(row));
            }
        };

        for (const auto& instance : selected)
        {
            auto path = pEffectRegistry->getEffectFilePath(instance);
            if (path.empty())
            {
                auto found = state.effectPaths.find(instance);
                if (found != state.effectPaths.end())
                    path = found->second;
            }
            appendFile(TechniqueOwner::Instance, instance, path.empty() ? path : canonicalPath(path), instance, true, false);
        }
        for (const auto& [name, path] : state.configuredEffectPaths)
            if (!contains(selected, name) && !path.empty())
                appendFile(TechniqueOwner::Instance, name, canonicalPath(path), name, false, true);
        for (const auto& path : state.effectFiles)
            if (!selectedPaths.count(path) && !configuredPaths.count(path))
                appendFile(TechniqueOwner::File, path, path, "", false, false);
        for (auto& row : rows)
            if (row.id.owner != TechniqueOwner::File && pEffectRegistry->hasEffectFailed(row.id.effect))
                row.error = pEffectRegistry->getEffectError(row.id.effect);
            else if (row.id.owner == TechniqueOwner::Instance && row.error.empty())
                row.error = pEffectRegistry->getTechniqueError(row.id.effect, row.id.technique);

        if (!techniqueSettings.hasEnabledTechniques)
        {
            if (!selectedReady)
            {
                ImGui::TextDisabled("Loading selected techniques...");
                return;
            }
            techniqueSettings.hasEnabledTechniques = true;
            techniqueSettings.hasTechniqueOrder    = true;
            const auto oldDisabled                 = pEffectRegistry->getConfig()->getOption<std::vector<std::string>>("disabledEffects", {});
            for (const auto& row : rows)
            {
                if (!contains(selected, row.id.effect) || row.id.owner == TechniqueOwner::File)
                    continue;
                const auto id = encodeTechniqueId(row.id);
                techniqueSettings.techniqueOrder.push_back(id);
                const bool enabled = pEffectRegistry->hasEffect(row.id.effect) ? pEffectRegistry->isEffectEnabled(row.id.effect)
                                                                               : !contains(oldDisabled, row.id.effect);
                if (enabled)
                    techniqueSettings.enabledTechniques.push_back(id);
            }
        }

        // A missing legacy shader was retained as a whole-file stage. Expand it once its techniques are available.
        for (const auto& instance : selected)
        {
            const auto placeholder = encodeTechniqueId({TechniqueOwner::Instance, instance, ""});
            auto       position    = std::find(techniqueSettings.techniqueOrder.begin(), techniqueSettings.techniqueOrder.end(), placeholder);
            auto&      entries     = techniqueSettings.enabledTechniques;
            if (position == techniqueSettings.techniqueOrder.end() && !contains(entries, placeholder))
                continue;
            std::vector<std::string> replacements;
            for (const auto& row : rows)
                if (row.id.owner == TechniqueOwner::Instance && row.id.effect == instance && !row.id.technique.empty())
                    replacements.push_back(encodeTechniqueId(row.id));
            if (replacements.empty())
                continue;
            if (position != techniqueSettings.techniqueOrder.end())
            {
                const auto offset = std::distance(techniqueSettings.techniqueOrder.begin(), position);
                techniqueSettings.techniqueOrder.erase(position);
                techniqueSettings.techniqueOrder.insert(techniqueSettings.techniqueOrder.begin() + offset, replacements.begin(), replacements.end());
            }
            if (contains(entries, placeholder))
            {
                entries.erase(std::remove(entries.begin(), entries.end(), placeholder), entries.end());
                for (const auto& replacement : replacements)
                    if (!contains(entries, replacement))
                        entries.push_back(replacement);
                paramsDirty    = true;
                lastChangeTime = std::chrono::steady_clock::now();
            }
            profileDirty = true;
        }

        std::vector<TechniqueId> available;
        for (const auto& row : rows)
            available.push_back(row.id);
        auto order =
            mergeProfileTechniqueOrder(available,
                                       techniqueSettings.hasTechniqueOrder ? decodeIds(techniqueSettings.techniqueOrder) : std::vector<TechniqueId>{},
                                       decodeIds(techniqueSettings.enabledTechniques));
        techniqueSettings.techniqueOrder    = encodeIds(order);
        techniqueSettings.hasTechniqueOrder = true;

        auto markChanged = [&]() {
            paramsDirty    = true;
            profileDirty   = true;
            lastChangeTime = std::chrono::steady_clock::now();
        };
        auto enabled    = [&](const TechniqueId& id) { return contains(techniqueSettings.enabledTechniques, encodeTechniqueId(id)); };
        auto setEnabled = [&](const TechniqueId& id, bool value) {
            const auto encoded = encodeTechniqueId(id);
            auto&      entries = techniqueSettings.enabledTechniques;
            entries.erase(std::remove(entries.begin(), entries.end(), encoded), entries.end());
            if (value)
                entries.push_back(encoded);
        };
        auto syncInstance = [&](const std::string& name) {
            bool any = false;
            for (const auto& id : decodeIds(techniqueSettings.enabledTechniques))
                any |= id.owner == TechniqueOwner::Instance && id.effect == name;
            pEffectRegistry->setEffectEnabled(name, any);
        };

        for (const auto& row : rows)
            if (row.id.owner == TechniqueOwner::Instance && row.forced && contains(selected, row.id.effect)
                && pEffectRegistry->isEffectEnabled(row.id.effect) && !enabled(row.id))
            {
                setEnabled(row.id, true);
                markChanged();
            }

        if (shaderMetadata.pending())
            ImGui::TextDisabled("Loading shaders: %zu remaining", shaderMetadata.pending());
        else if (rows.empty())
            ImGui::TextDisabled("No shaders found. Add a shader pack in the Shaders tab.");
        if (!techniqueNotice.empty())
            ImGui::TextWrapped("%s", techniqueNotice.c_str());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##technique_search", "Search techniques", techniqueSearch, sizeof(techniqueSearch));

        std::unordered_map<std::string, const Row*> byId;
        for (const auto& row : rows)
            byId.emplace(encodeTechniqueId(row.id), &row);

        std::string filter = techniqueSearch;
        std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        for (size_t rowIndex = 0; rowIndex < order.size(); ++rowIndex)
        {
            const TechniqueId id      = order[rowIndex];
            const auto        encoded = encodeTechniqueId(id);
            auto              found   = byId.find(encoded);
            if (found == byId.end() || found->second->hidden)
                continue;
            const Row&  row        = *found->second;
            std::string searchable = row.label + ' ' + row.path;
            std::transform(
                searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!filter.empty() && searchable.find(filter) == std::string::npos)
                continue;

            ImGui::PushID(encoded.c_str());
            const ImVec2 rowMin   = ImGui::GetCursorScreenPos();
            const float  rowWidth = ImGui::GetContentRegionAvail().x;
            const float  side     = ImGui::GetFrameHeight();
            ImGui::InvisibleButton("##drag", ImVec2(side, side));
            const auto min  = ImGui::GetItemRectMin();
            const auto max  = ImGui::GetItemRectMax();
            auto*      draw = ImGui::GetWindowDrawList();
            for (int bar = -1; bar <= 1; ++bar)
            {
                const float y = (min.y + max.y) * 0.5f + bar * 3.0f;
                draw->AddLine(ImVec2(min.x + 4, y), ImVec2(max.x - 4, y), IM_COL32(150, 150, 150, 220));
            }
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(reorderPayload, encoded.c_str(), encoded.size() + 1);
                ImGui::TextUnformatted(row.label.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::SameLine();

            const bool isSelected = id.owner == TechniqueOwner::Instance && contains(selected, id.effect);
            bool isEnabled = enabled(id) && (!isSelected || !pEffectRegistry->hasEffect(id.effect) || pEffectRegistry->isEffectEnabled(id.effect));
            const bool blocked = (!row.error.empty() && (!isSelected || !pEffectRegistry->hasEffect(id.effect))) || row.noEdit
                                 || (row.forced && isEnabled) || (profileSafeAntiCheat && row.depth);
            ImGui::BeginDisabled(blocked);
            if (ImGui::Checkbox("##enabled", &isEnabled))
            {
                if (isEnabled)
                {
                    size_t added = enabled(id) ? 0 : 1;
                    for (const auto& other : rows)
                        if (other.forced && other.id.owner == id.owner && other.id.effect == id.effect && other.id != id && !enabled(other.id))
                            ++added;
                    const auto   activeCount = buildTechniquePlan(getActiveEffects(), techniqueSettings).size();
                    const size_t limit = state.maxTechniqueSlots ? state.maxTechniqueSlots : static_cast<size_t>(settingsManager.getMaxEffects());
                    if (activeCount + added > limit)
                    {
                        techniqueNotice = "Technique limit reached. Increase Max Effects in Settings and restart the game.";
                        ImGui::EndDisabled();
                        ImGui::PopID();
                        break;
                    }
                }
                techniqueNotice.clear();
                if (isEnabled && isSelected)
                    pEffectRegistry->clearEffectError(id.effect);
                TechniqueId target   = id;
                std::string instance = id.effect;
                if (!isSelected)
                {
                    const auto proposed = id.owner == TechniqueOwner::File ? std::filesystem::path(row.path).stem().string() : id.effect;
                    instance =
                        makeInstanceName(proposed, selected, state.configuredEffectPaths, id.owner == TechniqueOwner::Instance ? id.effect : "");
                    pEffectRegistry->ensureEffect(instance, row.path);
                    if (pEffectRegistry->hasEffect(instance))
                    {
                        selected.push_back(instance);
                        pEffectRegistry->setSelectedEffects(selected);
                        target.owner  = TechniqueOwner::Instance;
                        target.effect = instance;
                        if (id.owner == TechniqueOwner::File || instance != id.effect)
                        {
                            for (auto& item : order)
                                if (item.owner == id.owner && item.effect == id.effect)
                                {
                                    item.owner  = target.owner;
                                    item.effect = instance;
                                }
                            techniqueSettings.techniqueOrder = encodeIds(order);
                        }
                    }
                    else
                    {
                        ImGui::EndDisabled();
                        ImGui::PopID();
                        break;
                    }
                }
                setEnabled(target, isEnabled);
                if (isEnabled && !row.path.empty())
                    for (const auto& other : rows)
                        if (other.path == row.path && other.forced && other.id.effect == id.effect && other.id.owner == id.owner)
                            setEnabled({TechniqueOwner::Instance, instance, other.id.technique}, true);
                syncInstance(instance);
                markChanged();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();

            if (!row.error.empty())
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            bool open = false;
            if (isSelected && row.error.empty())
                open = ImGui::TreeNode("##details", "%s", row.label.c_str());
            else
                ImGui::TextUnformatted(row.label.c_str());
            if (!row.error.empty())
                ImGui::PopStyleColor();
            bool stopRows = false;
            if (isSelected && ImGui::BeginPopupContextItem("##technique_menu"))
            {
                if (ImGui::MenuItem("Reset effect parameters"))
                {
                    for (auto* param : pEffectRegistry->getParametersForEffect(id.effect))
                        if (auto* editor = FieldEditorFactory::instance().getEditor(param->getType()))
                            editor->resetToDefault(*param);
                    markChanged();
                }
                if (ImGui::MenuItem("Add another instance", nullptr, false, row.error.empty()))
                {
                    size_t added = 1;
                    for (const auto& other : rows)
                        if (other.id.owner == id.owner && other.id.effect == id.effect && other.forced && other.id != id)
                            ++added;
                    const auto   activeCount = buildTechniquePlan(getActiveEffects(), techniqueSettings).size();
                    const size_t limit = state.maxTechniqueSlots ? state.maxTechniqueSlots : static_cast<size_t>(settingsManager.getMaxEffects());
                    if (activeCount + added > limit)
                        techniqueNotice = "Technique limit reached. Increase Max Effects in Settings and restart the game.";
                    else
                    {
                        const std::string proposed = std::filesystem::path(row.path).stem().string();
                        const std::string instance = makeInstanceName(proposed, selected, state.configuredEffectPaths);
                        pEffectRegistry->ensureEffect(instance, row.path);
                        if (pEffectRegistry->hasEffect(instance))
                        {
                            selected.push_back(instance);
                            pEffectRegistry->setSelectedEffects(selected);
                            for (const auto& other : rows)
                                if (other.id.owner == id.owner && other.id.effect == id.effect)
                                {
                                    auto duplicate   = other.id;
                                    duplicate.effect = instance;
                                    order.push_back(duplicate);
                                    if (other.id == id || other.forced)
                                        setEnabled(duplicate, true);
                                }
                            techniqueSettings.techniqueOrder = encodeIds(order);
                            syncInstance(instance);
                            techniqueNotice.clear();
                            markChanged();
                            stopRows = true;
                        }
                    }
                }
                if (ImGui::MenuItem("Remove instance"))
                {
                    const std::string path = row.path;
                    selected.erase(std::remove(selected.begin(), selected.end(), id.effect), selected.end());
                    bool another = false;
                    for (const auto& name : selected)
                        another |= canonicalPath(pEffectRegistry->getEffectFilePath(name)) == path;
                    pEffectRegistry->setSelectedEffects(selected);
                    pEffectRegistry->removeEffect(id.effect);
                    for (auto it = order.begin(); it != order.end();)
                    {
                        if (it->owner != id.owner || it->effect != id.effect)
                        {
                            ++it;
                            continue;
                        }
                        if (another || path.empty())
                            it = order.erase(it);
                        else
                        {
                            it->owner  = TechniqueOwner::File;
                            it->effect = path;
                            ++it;
                        }
                    }
                    auto& entries = techniqueSettings.enabledTechniques;
                    entries.erase(std::remove_if(entries.begin(),
                                                 entries.end(),
                                                 [&](const auto& entry) {
                                                     auto removed = decodeTechniqueId(entry);
                                                     return removed && removed->owner == id.owner && removed->effect == id.effect;
                                                 }),
                                  entries.end());
                    techniqueSettings.techniqueOrder = encodeIds(order);
                    markChanged();
                    stopRows = true;
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                if (!row.error.empty())
                    ImGui::SetTooltip("%s", row.error.c_str());
                else if (profileSafeAntiCheat && row.depth)
                    ImGui::SetTooltip("Depth effects are disabled by Safe Anti-Cheat");
                else if (!row.tooltip.empty() && !row.path.empty())
                    ImGui::SetTooltip("%s\n%s", row.tooltip.c_str(), row.path.c_str());
                else if (!row.tooltip.empty() || !row.path.empty())
                    ImGui::SetTooltip("%s", row.tooltip.empty() ? row.path.c_str() : row.tooltip.c_str());
            }

            const ImRect drop(rowMin, ImVec2(rowMin.x + rowWidth, rowMin.y + ImGui::GetFrameHeightWithSpacing()));
            if (!stopRows && ImGui::BeginDragDropTargetCustom(drop, ImGui::GetID("##drop")))
            {
                if (const auto* payload = ImGui::AcceptDragDropPayload(reorderPayload))
                    if (auto source = decodeTechniqueId(static_cast<const char*>(payload->Data)); source && moveTechnique(order, *source, id))
                    {
                        techniqueSettings.techniqueOrder = encodeIds(order);
                        markChanged();
                    }
                ImGui::EndDragDropTarget();
            }
            if (open)
            {
                if (!stopRows)
                    renderEffectDetails(id.effect);
                ImGui::TreePop();
            }
            ImGui::PopID();
            if (stopRows)
                break;
        }
    }
    void ImGuiOverlay::renderEffectDetails(const std::string& effectName)
    {
        // Show preprocessor definitions first (ReShade effects only)
        if (pEffectRegistry)
        {
            auto& defs = pEffectRegistry->getPreprocessorDefs(effectName);
            if (!defs.empty())
            {
                // Draw background rect behind preprocessor section using channels
                ImVec2      startPos     = ImGui::GetCursorScreenPos();
                float       contentWidth = ImGui::GetContentRegionAvail().x;
                ImDrawList* drawList     = ImGui::GetWindowDrawList();
                drawList->ChannelsSplit(2);
                drawList->ChannelsSetCurrent(1); // Foreground for content

                if (ImGui::TreeNode("preprocessor", "Preprocessor (%zu)", defs.size()))
                {
                    ImGui::TextWrapped("%s",
                                       preprocessorEdit.pending()       ? "Checking preprocessor value..."
                                       : settingsManager.getAutoApply() ? "Values are checked when editing ends."
                                                                        : "Values are checked when editing ends; click Apply to recompile.");

                    for (size_t defIdx = 0; defIdx < defs.size(); defIdx++)
                    {
                        ImGui::PushID(static_cast<int>(defIdx + 1000));
                        if (const auto value = renderPreprocessorInput(defs[defIdx], preprocessorEdit.pending()))
                            queuePreprocessorEdit(effectName, defs[defIdx], *value);
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }

                // Draw background rect on channel 0 (behind content)
                ImVec2 endPos = ImGui::GetCursorScreenPos();
                drawList->ChannelsSetCurrent(0); // Background
                drawList->AddRectFilled(startPos,
                                        ImVec2(startPos.x + contentWidth, endPos.y),
                                        IM_COL32(0, 0, 0, 128), // 50% opacity black
                                        0.0f);
                drawList->ChannelsMerge();
            }
        }

        // Show parameters for this effect, grouped under ui_category headers
        // (declaration order preserved; a header spans its contiguous run of
        // params, matching ReShade). Params with no category render directly.
        auto        effectParams = pEffectRegistry ? pEffectRegistry->getParametersForEffect(effectName) : std::vector<EffectParam*>{};
        std::string activeCategory;          // category of the header currently open
        bool        haveCategory    = false; // currently inside a category header?
        bool        categoryVisible = true;  // is that header expanded?
        bool        categoryEnabled = true;
        for (size_t paramIdx = 0; paramIdx < effectParams.size(); paramIdx++)
        {
            EffectParam*       param = effectParams[paramIdx];
            const std::string& cat   = param->category;

            if (!(haveCategory && cat == activeCategory))
            {
                // Category boundary: close the previous header's indent first.
                if (haveCategory && categoryVisible)
                    ImGui::Unindent();

                haveCategory   = !cat.empty();
                activeCategory = cat;

                if (haveCategory)
                {
                    categoryEnabled = !param->categoryToggle || categoryToggleEnabled(*param);
                    categoryVisible = false;
                    if (categoryEnabled)
                    {
                        ImGuiTreeNodeFlags flags  = param->categoryClosed ? 0 : ImGuiTreeNodeFlags_DefaultOpen;
                        std::string        header = cat + "##cat_" + effectName + "_" + std::to_string(paramIdx);
                        categoryVisible           = ImGui::CollapsingHeader(header.c_str(), flags);
                    }
                    if (categoryVisible)
                        ImGui::Indent();
                }
                else
                {
                    categoryVisible = true; // uncategorized params always show
                    categoryEnabled = true;
                }
            }

            if (haveCategory && !categoryVisible && (categoryEnabled || !param->categoryToggle))
                continue; // header collapsed: skip its controls

            ImGui::PushID(static_cast<int>(paramIdx));
            if (renderFieldEditor(*param))
            {
                paramsDirty    = true;
                lastChangeTime = std::chrono::steady_clock::now();
            }
            ImGui::PopID();
        }
        if (haveCategory && categoryVisible)
            ImGui::Unindent(); // balance the final open category's indent
    }
} // namespace vkShade
