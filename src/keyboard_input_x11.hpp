#pragma once

#include <cstdint>
#include <string>
#include "keyboard_input.hpp"

namespace vkShade
{
    uint32_t      convertToKeySymX11(const std::string& key);
    void          beginKeyboardInputFrameX11();
    bool          isKeyPressedX11(uint32_t ks);
    KeyboardState getKeyboardStateX11();

    // For input blocking - returns the keyboard display connection
    void* getKeyboardDisplay();
} // namespace vkShade
