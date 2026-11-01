#ifndef MOUSE_INPUT_HPP_INCLUDED
#define MOUSE_INPUT_HPP_INCLUDED

#include <cstdint>

namespace vkShade
{
    struct MouseState
    {
        double x                 = 0;
        double y                 = 0;
        double framebufferScaleX = 1;
        double framebufferScaleY = 1;
        bool   leftButton        = false;
        bool   rightButton       = false;
        bool   middleButton      = false;
        float  scrollDelta       = 0.0f; // Positive = up, negative = down
    };

    MouseState getMouseState(uint32_t framebufferWidth, uint32_t framebufferHeight);

} // namespace vkShade

#endif // MOUSE_INPUT_HPP_INCLUDED
