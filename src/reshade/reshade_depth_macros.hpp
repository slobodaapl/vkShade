#ifndef VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED
#define VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED

#include "reshade/effect_preprocessor.hpp"

namespace vkShade
{
    inline void addReshadeDepthMacros(reshadefx::preprocessor& pp)
    {
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", "0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", "0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", "0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_SCALE", "1.0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_SCALE", "1.0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_OFFSET", "0.0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_OFFSET", "0.0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", "0");
        pp.add_macro_definition("RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", "0");
        pp.add_macro_definition("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", "1000.0");
        pp.add_macro_definition("RESHADE_DEPTH_MULTIPLIER", "1.0");
        pp.add_macro_definition("RESHADE_MIX_STAGE_DEPTH_MAP", "0");
    }
} // namespace vkShade

#endif // VKSHADE_RESHADE_DEPTH_MACROS_HPP_INCLUDED
