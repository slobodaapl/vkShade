#include "effect_registry.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <sstream>

#include "reshade_parser.hpp"
#include "config_serializer.hpp"
#include "logger.hpp"

namespace vkShade
{

    namespace
    {
        std::string toLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool hasFxExtension(const std::filesystem::path& path)
        {
            return toLower(path.extension().string()) == ".fx";
        }

        std::string tryResolveDirectEffectPath(const std::string& value)
        {
            if (value.empty())
                return "";

            std::error_code       ec;
            std::filesystem::path candidate(value);
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec))
            {
                if (hasFxExtension(candidate))
                    return candidate.string();
            }

            if (!candidate.has_extension())
            {
                std::filesystem::path withFx = candidate;
                withFx.replace_extension(".fx");
                if (std::filesystem::exists(withFx, ec) && std::filesystem::is_regular_file(withFx, ec))
                    return withFx.string();
            }

            return "";
        }

        // Search a list of directories for an effect file by name
        std::string searchDirsForEffect(const std::string& name, const std::vector<std::string>& dirs)
        {
            const std::filesystem::path queryPath(name);
            const std::string           queryFilenameLower = toLower(queryPath.filename().string());
            const std::string           queryStemLower     = toLower(queryPath.stem().string());

            for (const auto& dir : dirs)
            {
                std::error_code       ec;
                std::filesystem::path base(dir);

                std::filesystem::path path = base / name;
                if (std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec))
                {
                    if (hasFxExtension(path))
                        return path.string();
                }

                if (!queryPath.has_extension())
                {
                    std::filesystem::path withFx = base / (name + ".fx");
                    if (std::filesystem::exists(withFx, ec) && std::filesystem::is_regular_file(withFx, ec))
                        return withFx.string();
                }

                if (!std::filesystem::exists(base, ec) || !std::filesystem::is_directory(base, ec))
                    continue;

                try
                {
                    for (const auto& entry :
                         std::filesystem::recursive_directory_iterator(base, std::filesystem::directory_options::skip_permission_denied))
                    {
                        if (!entry.is_regular_file())
                            continue;

                        const auto& filePath = entry.path();
                        if (!hasFxExtension(filePath))
                            continue;

                        const std::string filenameLower = toLower(filePath.filename().string());
                        const std::string stemLower     = toLower(filePath.stem().string());

                        // Match either full filename (for "Foo.fx") or stem (for "Foo")
                        if ((!queryFilenameLower.empty() && filenameLower == queryFilenameLower)
                            || (!queryStemLower.empty() && stemLower == queryStemLower))
                        {
                            return filePath.string();
                        }
                    }
                }
                catch (const std::filesystem::filesystem_error&)
                {
                    // Keep searching other paths if one directory is inaccessible
                }
            }
            return "";
        }

        // Try to find effect file path
        std::string findEffectPath(const std::string& name, Config* pConfig)
        {
            // If caller already provides a path (absolute or relative), use it directly.
            std::string directPath = tryResolveDirectEffectPath(name);
            if (!directPath.empty())
                return directPath;

            // First check if path is directly configured (e.g. "Vibrance = /path/to/Vibrance.fx")
            std::string path                   = pConfig->getOption<std::string>(name, "");
            std::string resolvedConfiguredPath = tryResolveDirectEffectPath(path);
            if (!resolvedConfiguredPath.empty())
                return resolvedConfiguredPath;

            // Search in reshadeIncludePath from config (colon-separated list)
            std::string includePath = pConfig->getOption<std::string>("reshadeIncludePath", "");
            if (!includePath.empty())
            {
                std::vector<std::string> includeDirs;
                std::stringstream        ss(includePath);
                std::string              dir;
                while (std::getline(ss, dir, ':'))
                {
                    if (!dir.empty())
                        includeDirs.push_back(dir);
                }
                path = searchDirsForEffect(name, includeDirs);
                if (!path.empty())
                    return path;
            }

            // Search in shader manager discovered paths
            ShaderManagerConfig shaderMgrConfig = ConfigSerializer::loadShaderManagerConfig();
            path                                = searchDirsForEffect(name, shaderMgrConfig.discoveredShaderPaths);
            if (!path.empty())
                return path;

            return "";
        }
    } // anonymous namespace

    void EffectRegistry::initialize(Config* pConfig)
    {
        std::lock_guard<std::mutex> lock(mutex);
        this->pConfig    = pConfig;
        depthDefinitions = ConfigSerializer::loadDepthDefinitions(pConfig);
        effects.clear();

        std::vector<std::string> effectNames     = pConfig->getOption<std::vector<std::string>>("effects");
        std::vector<std::string> disabledEffects = pConfig->getOption<std::vector<std::string>>("disabledEffects");

        // Build set for quick lookup
        std::set<std::string> disabledSet(disabledEffects.begin(), disabledEffects.end());

        for (const auto& name : effectNames)
        {
            std::string effectPath = findEffectPath(name, pConfig);
            if (effectPath.empty())
            {
                Logger::warn("EffectRegistry: could not find shader file for: " + name);
                continue;
            }
            initReshadeEffect(name, effectPath);

            // Set enabled state based on disabledEffects list
            if (!effects.empty() && disabledSet.count(name))
                effects.back().enabled = false;
        }

        Logger::debug("EffectRegistry: initialized " + std::to_string(effects.size()) + " effects");
    }

    DepthDefinitionValues EffectRegistry::getDepthDefinitions() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return depthDefinitions;
    }

    void EffectRegistry::setDepthDefinitions(DepthDefinitionValues definitions)
    {
        std::lock_guard<std::mutex> lock(mutex);
        depthDefinitions = std::move(definitions);
    }

    void EffectRegistry::initReshadeEffect(const std::string& name, const std::string& path)
    {
        EffectConfig config;
        config.name     = name;
        config.filePath = path;
        config.enabled  = true;

        // Record file modification time for change detection
        std::error_code ec;
        config.fileModTime = std::filesystem::last_write_time(path, ec);

        config.parameters       = parseReshadeEffect(name, path, pConfig);
        config.preprocessorDefs = extractPreprocessorDefinitions(name, path, pConfig);

        for (auto& def : config.preprocessorDefs)
        {
            std::string configKey  = name + "@" + def.name;
            std::string savedValue = pConfig->getOption<std::string>(configKey, "");
            if (!savedValue.empty())
                def.value = savedValue;
        }

        effects.push_back(std::move(config));
    }

    std::vector<const EffectConfig*> EffectRegistry::getEnabledEffects() const
    {
        std::lock_guard<std::mutex>      lock(mutex);
        std::vector<const EffectConfig*> enabled;

        for (const auto& effect : effects)
        {
            if (effect.enabled)
                enabled.push_back(&effect);
        }

        return enabled;
    }

    std::vector<std::unique_ptr<EffectParam>> EffectRegistry::getAllParameters() const
    {
        std::lock_guard<std::mutex>               lock(mutex);
        std::vector<std::unique_ptr<EffectParam>> params;

        for (const auto& effect : effects)
        {
            for (const auto& p : effect.parameters)
                params.push_back(p->clone());
        }

        return params;
    }

    // Internal helper to find effect by name (assumes mutex is held)
    EffectConfig* EffectRegistry::findEffect(const std::string& effectName)
    {
        for (auto& effect : effects)
        {
            if (effect.name == effectName)
                return &effect;
        }
        return nullptr;
    }

    const EffectConfig* EffectRegistry::findEffect(const std::string& effectName) const
    {
        for (const auto& effect : effects)
        {
            if (effect.name == effectName)
                return &effect;
        }
        return nullptr;
    }

    // Internal helper to find parameter within an effect (assumes mutex is held)
    EffectParam* EffectRegistry::findParam(EffectConfig& effect, const std::string& paramName)
    {
        for (auto& param : effect.parameters)
        {
            if (param->name == paramName)
                return param.get();
        }
        return nullptr;
    }

    const EffectParam* EffectRegistry::findParam(const EffectConfig& effect, const std::string& paramName) const
    {
        for (const auto& param : effect.parameters)
        {
            if (param->name == paramName)
                return param.get();
        }
        return nullptr;
    }

    void EffectRegistry::setEffectEnabled(const std::string& effectName, bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex);

        EffectConfig* effect = findEffect(effectName);
        if (effect)
            effect->enabled = enabled;
    }

    bool EffectRegistry::isEffectEnabled(const std::string& effectName) const
    {
        std::lock_guard<std::mutex> lock(mutex);

        const EffectConfig* effect = findEffect(effectName);
        return effect ? effect->enabled : false;
    }

    std::map<std::string, bool> EffectRegistry::getEffectEnabledStates() const
    {
        std::lock_guard<std::mutex> lock(mutex);

        std::map<std::string, bool> states;
        for (const auto& effect : effects)
            states[effect.name] = effect.enabled;
        return states;
    }

    void EffectRegistry::setParameterValue(const std::string& effectName, const std::string& paramName, float value)
    {
        std::lock_guard<std::mutex> lock(mutex);

        EffectConfig* effect = findEffect(effectName);
        if (!effect)
            return;

        EffectParam* param = findParam(*effect, paramName);
        if (param && param->getType() == ParamType::Float)
            static_cast<FloatParam*>(param)->value = value;
    }

    void EffectRegistry::setParameterValue(const std::string& effectName, const std::string& paramName, int value)
    {
        std::lock_guard<std::mutex> lock(mutex);

        EffectConfig* effect = findEffect(effectName);
        if (!effect)
            return;

        EffectParam* param = findParam(*effect, paramName);
        if (param && param->getType() == ParamType::Int)
            static_cast<IntParam*>(param)->value = value;
    }

    void EffectRegistry::setParameterValue(const std::string& effectName, const std::string& paramName, bool value)
    {
        std::lock_guard<std::mutex> lock(mutex);

        EffectConfig* effect = findEffect(effectName);
        if (!effect)
            return;

        EffectParam* param = findParam(*effect, paramName);
        if (param && param->getType() == ParamType::Bool)
            static_cast<BoolParam*>(param)->value = value;
    }

    EffectParam* EffectRegistry::getParameter(const std::string& effectName, const std::string& paramName)
    {
        std::lock_guard<std::mutex> lock(mutex);

        EffectConfig* effect = findEffect(effectName);
        if (!effect)
            return nullptr;

        return findParam(*effect, paramName);
    }

    const EffectParam* EffectRegistry::getParameter(const std::string& effectName, const std::string& paramName) const
    {
        std::lock_guard<std::mutex> lock(mutex);

        const EffectConfig* effect = findEffect(effectName);
        if (!effect)
            return nullptr;

        return findParam(*effect, paramName);
    }

    std::vector<EffectParam*> EffectRegistry::getParametersForEffect(const std::string& effectName)
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<EffectParam*>   result;

        EffectConfig* effect = findEffect(effectName);
        if (!effect)
            return result;

        for (auto& param : effect->parameters)
            result.push_back(param.get());

        return result;
    }

    bool EffectRegistry::hasEffect(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return findEffect(name) != nullptr;
    }

    std::string EffectRegistry::getEffectFilePath(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectConfig*         effect = findEffect(name);
        return effect ? effect->filePath : "";
    }

    bool EffectRegistry::hasEffectFailed(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectConfig*         effect = findEffect(name);
        return effect ? effect->hasFailed() : false;
    }

    std::string EffectRegistry::getEffectError(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectConfig*         effect = findEffect(name);
        return effect ? effect->compileError : "";
    }

    void EffectRegistry::setEffectError(const std::string& name, const std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        EffectConfig*               effect = findEffect(name);
        if (effect)
        {
            effect->compileError = error;
            effect->enabled      = false; // Disable failed effects
        }
    }

    void EffectRegistry::clearEffectError(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto* effect = findEffect(name))
            effect->compileError.clear();
    }

    void EffectRegistry::setTechniqueError(const std::string& name, const std::string& technique, const std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto* effect = findEffect(name))
        {
            if (error.empty())
                effect->techniqueErrors.erase(technique);
            else
                effect->techniqueErrors[technique] = error;
        }
    }

    std::string EffectRegistry::getTechniqueError(const std::string& name, const std::string& technique) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto* effect = findEffect(name))
        {
            auto it = effect->techniqueErrors.find(technique);
            if (it != effect->techniqueErrors.end())
                return it->second;
        }
        return {};
    }

    void EffectRegistry::ensureEffect(const std::string& instanceName, const std::string& effectPath)
    {
        const std::string requested = effectPath.empty() ? instanceName : effectPath;

        // Do path lookup outside mutex (filesystem I/O can be slow)
        std::string path = findEffectPath(requested, pConfig);
        if (path.empty() || !std::filesystem::exists(path))
        {
            Logger::warn("EffectRegistry::ensureEffect: could not find shader file for: " + requested);
            return;
        }

        // Lock for the check-and-mutate as a single critical section
        std::lock_guard<std::mutex> lock(mutex);

        EffectConfig* existing = findEffect(instanceName);
        if (existing)
        {
            std::error_code ec;
            auto            currentModTime = std::filesystem::last_write_time(path, ec);
            if (ec || currentModTime == existing->fileModTime)
                return;
            Logger::info("EffectRegistry: shader file changed, re-parsing: " + instanceName);
            effects.erase(std::remove_if(effects.begin(), effects.end(), [&](const EffectConfig& e) { return e.name == instanceName; }),
                          effects.end());
        }

        initReshadeEffect(instanceName, path);
    }

    void EffectRegistry::removeEffect(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex);
        effects.erase(std::remove_if(effects.begin(), effects.end(), [&](const EffectConfig& e) { return e.name == name; }), effects.end());
    }

    // Per-instance empty vector for returning when effect not found (avoids shared mutable static)
    std::vector<PreprocessorDefinition>& EffectRegistry::getPreprocessorDefs(const std::string& effectName)
    {
        std::lock_guard<std::mutex> lock(mutex);
        EffectConfig*               effect = findEffect(effectName);
        if (effect)
            return effect->preprocessorDefs;
        static thread_local std::vector<PreprocessorDefinition> emptyDefs;
        emptyDefs.clear();
        return emptyDefs;
    }

    const std::vector<PreprocessorDefinition>& EffectRegistry::getPreprocessorDefs(const std::string& effectName) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectConfig*         effect = findEffect(effectName);
        if (effect)
            return effect->preprocessorDefs;
        static thread_local const std::vector<PreprocessorDefinition> emptyDefs;
        return emptyDefs;
    }

    void EffectRegistry::setPreprocessorDefValue(const std::string& effectName, const std::string& macroName, const std::string& value)
    {
        if (std::none_of(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }))
            return;
        std::lock_guard<std::mutex> lock(mutex);
        EffectConfig*               effect = findEffect(effectName);
        if (!effect)
            return;

        for (auto& def : effect->preprocessorDefs)
        {
            if (def.name == macroName)
            {
                def.value = value;
                return;
            }
        }
    }

    void EffectRegistry::setSelectedEffects(const std::vector<std::string>& effects)
    {
        std::lock_guard<std::mutex> lock(mutex);
        selectedEffects = effects;
    }

    void EffectRegistry::clearSelectedEffects()
    {
        std::lock_guard<std::mutex> lock(mutex);
        selectedEffects.clear();
    }

    void EffectRegistry::initializeSelectedEffectsFromConfig()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (initializedFromConfig || !pConfig)
                return;
        }

        // Read effects list from config
        std::vector<std::string> configEffects   = pConfig->getOption<std::vector<std::string>>("effects", {});
        std::vector<std::string> disabledEffects = pConfig->getOption<std::vector<std::string>>("disabledEffects", {});

        // Build set of disabled effects for quick lookup
        std::set<std::string> disabledSet(disabledEffects.begin(), disabledEffects.end());

        // Set selected effects
        {
            std::lock_guard<std::mutex> lock(mutex);
            selectedEffects = configEffects;
        }

        // Ensure effects exist in registry before setting enabled states
        for (const auto& effectName : configEffects)
            ensureEffect(effectName);

        // Set enabled states (disabled if in disabledEffects list)
        for (const auto& effectName : configEffects)
        {
            bool enabled = (disabledSet.find(effectName) == disabledSet.end());
            setEffectEnabled(effectName, enabled);
        }

        initializedFromConfig = true;
        Logger::debug("EffectRegistry: initialized " + std::to_string(configEffects.size()) + " effects from config ("
                      + std::to_string(disabledEffects.size()) + " disabled)");
    }

} // namespace vkShade
