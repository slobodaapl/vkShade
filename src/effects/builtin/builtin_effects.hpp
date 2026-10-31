#ifndef BUILTIN_EFFECTS_HPP_INCLUDED
#define BUILTIN_EFFECTS_HPP_INCLUDED

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>

#include "../effect.hpp"
#include "../params/effect_param.hpp"
#include "logical_device.hpp"
#include "config.hpp"

namespace vkShade
{
    // Parameter definition for built-in effects
    struct ParamDef
    {
        std::string name;
        std::string label;
        ParamType   type         = ParamType::Float;
        float       defaultFloat = 0.0f;
        float       minFloat     = 0.0f;
        float       maxFloat     = 1.0f;
        int         defaultInt   = 0;
        int         minInt       = 0;
        int         maxInt       = 100;
    };

    // Factory function signature for creating effects
    using EffectFactory = std::function<std::shared_ptr<Effect>(LogicalDevice*              pLogicalDevice,
                                                                VkFormat                    format,
                                                                VkExtent2D                  extent,
                                                                const std::vector<VkImage>& inputImages,
                                                                const std::vector<VkImage>& outputImages,
                                                                Config*                     pConfig)>;

    // Built-in effect definition
    struct BuiltInEffectDef
    {
        std::string           typeName;
        bool                  usesSrgbFormat = false;
        std::vector<ParamDef> params;
        EffectFactory         factory;
    };

    // Registry of all built-in effects
    class BuiltInEffects
    {
    public:
        static const BuiltInEffects& instance();

        // Check if effect type is built-in
        bool isBuiltIn(const std::string& typeName) const;

        // Get effect definition (returns nullptr if not found)
        const BuiltInEffectDef* getDef(const std::string& typeName) const;

        // Get all built-in effect type names
        std::vector<std::string> getTypeNames() const;

    private:
        BuiltInEffects();
        std::map<std::string, BuiltInEffectDef> effects;
    };

} // namespace vkShade

#endif // BUILTIN_EFFECTS_HPP_INCLUDED
