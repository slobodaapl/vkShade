#pragma once

#include "shader_metadata_loader.hpp"
#include "depth_definitions.hpp"
#include "effects/effect_config.hpp"

#include <optional>
#include <string>

namespace vkShade
{
    std::optional<std::string> renderPreprocessorInput(const PreprocessorDefinition& definition, bool pending);
    std::optional<std::string>
    renderDepthDefinitionInput(const DepthDefinition& definition, const char* label, const std::string& value, bool pending, std::string& notice);

    struct PreprocessorEdit
    {
        std::string           effectName;
        std::string           macroName;
        std::string           value;
        std::string           originalValue;
        std::string           defaultValue;
        std::string           profilePath;
        ShaderMetadataRequest request;
    };

    struct PreprocessorEditResult
    {
        PreprocessorEdit edit;
        bool             valid = false;
        std::string      error;
    };

    class PreprocessorEditValidator
    {
    public:
        bool                                  start(PreprocessorEdit edit);
        std::optional<PreprocessorEditResult> poll();
        bool                                  pending() const
        {
            return current.has_value();
        }
        void cancel();

    private:
        ShaderMetadataLoader            loader;
        std::optional<PreprocessorEdit> current;
    };
} // namespace vkShade
