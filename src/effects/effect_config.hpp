#ifndef EFFECT_CONFIG_HPP_INCLUDED
#define EFFECT_CONFIG_HPP_INCLUDED

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <filesystem>

#include "params/effect_param.hpp"

namespace vkShade
{
    // Preprocessor definition extracted from ReShade shader
    // These are user-configurable compile-time constants (#define macros)
    struct PreprocessorDefinition
    {
        std::string name;         // Macro name, e.g., "ENABLE_SCANLINES"
        std::string value;        // Current value (will be passed to compiler)
        std::string defaultValue; // Default from shader or "1"
        std::string effectName;   // Which effect this belongs to
    };

    struct EffectConfig
    {
        std::string                               name;
        std::string                               filePath;
        bool                                      enabled = true;
        std::vector<std::unique_ptr<EffectParam>> parameters;
        std::vector<PreprocessorDefinition>       preprocessorDefs; // ReShade: user-configurable macros
        std::string                               compileError;     // Empty if compiled successfully, error message if failed
        std::map<std::string, std::string>        techniqueErrors;
        std::filesystem::file_time_type           fileModTime{}; // Last modification time of .fx file when parsed
        bool                                      hasFailed() const
        {
            return !compileError.empty();
        }
    };

} // namespace vkShade

#endif // EFFECT_CONFIG_HPP_INCLUDED
