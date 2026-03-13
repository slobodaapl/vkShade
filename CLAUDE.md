# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Setup build directory (release — recommended)
meson setup --buildtype=release builddir

# Setup build directory (debug)
meson setup --buildtype=debug builddir

# Build
ninja -C builddir

# Install
ninja -C builddir install

# 32-bit build
ASFLAGS=--32 CFLAGS=-m32 CXXFLAGS=-m32 PKG_CONFIG_PATH=/usr/lib32/pkgconfig \
meson setup --prefix=/usr --buildtype=release --libdir=lib32 -Dwith_json=false builddir.32
ninja -C builddir.32
```

**Dependencies**: GCC >= 9, X11 development files, glslang, SPIR-V Headers, Vulkan Headers

### Nix / Flake Build

The flake defaults to **release** mode. A debug variant is available:

```bash
nix build .#vkbasalt-overlay          # release (default)
nix build .#vkbasalt-overlay-debug    # debug build
```

### Pre-commit Formatting

This repo has a treefmt pre-commit hook. **Always run `nix fmt` before committing `.nix` files** — the hook uses `--fail-on-change` and will reject unformatted code.

## Testing

Run any Vulkan game/application with:
```bash
ENABLE_VKBASALT=1 VKBASALT_LOG_LEVEL=debug ./game
```

Use `VKBASALT_CONFIG_FILE=/path/to/config.conf` to test specific configurations.

### Dev Build Testing (Implicit Layer)

For implicit layers, `VK_LAYER_PATH` does NOT work. Use `VK_ADD_IMPLICIT_LAYER_PATH` to prioritize the dev build over the system-installed layer:

```bash
VK_ADD_IMPLICIT_LAYER_PATH="$PWD/builddir/config" ENABLE_VKBASALT=1 VKBASALT_LOG_LEVEL=debug vkcube
```

Verify the correct `.so` is loaded with `VK_LOADER_DEBUG=layer`.

**Note:** After `meson setup`, `builddir/config/vkBasalt-overlay.json` must have an absolute `library_path` pointing to `builddir/src/libvkbasalt-overlay.so`.

### Standalone Shader Test Tool

`tools/test_shaders.cpp` compiles and tests all `.fx` shaders without needing Vulkan or ImGui:

```bash
# Build (links only against libreshade.a)
g++ -std=c++20 -O2 -Isrc -Isrc/reshade tools/test_shaders.cpp -Lbuilddir/src/reshade -lreshade -o builddir/test_shaders

# Run (reads shader paths from ~/.config/vkBasalt-overlay/shader_manager.conf)
LD_LIBRARY_PATH=builddir/src/reshade ./builddir/test_shaders

# Or test specific directories
LD_LIBRARY_PATH=builddir/src/reshade ./builddir/test_shaders /path/to/Shaders/
```

Reports: pass/fail, warnings, depth usage, grouped failure diagnostics. Deduplicates by canonical path (resolves symlinks like `/etc/...` → `/nix/store/...`).

## CRITICAL: EffectRegistry is the Single Source of Truth

**THIS IS THE MOST IMPORTANT ARCHITECTURAL CONCEPT IN THE CODEBASE.**

`EffectRegistry` (`src/effects/effect_registry.hpp`) is the **ONLY** authoritative source for effect parameter values at runtime. All components read from and write to the registry.

### Data Flow (MUST follow this pattern)

```
Config file ──(parsed ONCE at startup)──► EffectRegistry ◄──► UI (reads/writes)
                                               │
                                               ▼
                                    Effects read from Registry
                                               │
                                               ▼
                                           Rendering
```

### Rules (NEVER violate these)

1. **Config is parsed ONCE** - At startup, config values populate EffectRegistry. After that, Config is NOT the source of truth.

2. **UI reads/writes EffectRegistry directly** - The overlay modifies `EffectRegistry` parameters in-place. No intermediate copies.

3. **Effects read from EffectRegistry** - When effects are created/recreated, they get parameter values from `EffectRegistry->getParameter()`, NOT from `pConfig`.

4. **Never sync Registry → pConfig for rendering** - The old pattern of syncing Registry to Config overrides is WRONG. Effects read directly from Registry.

5. **Save writes Registry → file** - When saving config, serialize values FROM EffectRegistry to the config file.

### For ReShade Effects

ReShade effects use specialization constants. The `ReshadeEffect` constructor takes `EffectRegistry*` and reads spec constant values like this:

```cpp
EffectParam* param = pEffectRegistry->getParameter(effectName, paramName);
if (auto* fp = dynamic_cast<FloatParam*>(param))
    value = fp->value;  // Read directly from registry
```

For vector types (float2/3/4, int2/3/4, uint2/3/4), components share the same parameter name but use an index:
```cpp
if (auto* fvp = dynamic_cast<FloatVecParam*>(param))
    value = fvp->value[vectorComponentIndex];  // 0, 1, 2, or 3
```

## Architecture Overview

vkBasalt is a **Vulkan implicit layer** that intercepts Vulkan API calls to apply post-processing effects to game graphics.

### Layer System (basalt.cpp)

The layer intercepts these key Vulkan functions:
- `vkCreateInstance` / `vkDestroyInstance` - Layer initialization
- `vkCreateDevice` / `vkDestroyDevice` - LogicalDevice setup
- `vkCreateSwapchainKHR` / `vkDestroySwapchainKHR` - Create intermediate images for effect processing
- `vkQueuePresentKHR` - **Main entry point**: applies effects before presentation
- `vkGetSwapchainImagesKHR` - Returns wrapped images

**Dispatch tables** (`vkdispatch.hpp`): Store Vulkan function pointers per-instance and per-device. Functions are listed via macros in `vkfuncs.hpp`.

**Global maps** track state by handle:
- `instanceDispatchMap`, `deviceMap` - Dispatch tables
- `swapchainMap` - Per-swapchain effect state (LogicalSwapchain)
- `effectRegistry` - Global registry for effect configurations and parameters

### Effect Processing Flow

```
Original Swapchain Image
    → Effect 1 (CAS, FXAA, etc.)
    → Effect 2
    → ...
    → Final image presented
```

Effects read from one image and write to another. "Fake images" are intermediate buffers created in `vkCreateSwapchainKHR`.

### Effect System

**Base class** (`effect.hpp`):
```cpp
class Effect {
    virtual void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) = 0;
    virtual void updateEffect() {}
};
```

**Built-in effects** (inherit from `SimpleEffect`):
- `effect_cas.cpp` - Contrast Adaptive Sharpening
- `effect_dls.cpp` - Denoised Luma Sharpening
- `effect_fxaa.cpp` - Fast Approximate Anti-Aliasing
- `effect_smaa.cpp` - Subpixel Morphological AA (multi-pass)
- `effect_deband.cpp` - Color banding reduction
- `effect_lut.cpp` - 3D color lookup table

**ReShade FX support** (`effect_reshade.cpp`): Compiles .fx shader files using the embedded ReShade compiler (`src/reshade/`). Takes `EffectRegistry*` to read parameter values.

### Parameter System

**EffectParam** hierarchy (`src/effects/params/effect_param.hpp`):

| Type | Description | Storage |
|------|-------------|---------|
| `FloatParam` | Scalar float | `float value` |
| `FloatVecParam` | float2/3/4 vector | `float value[4]` + `componentCount` |
| `IntParam` | Scalar signed int | `int value` |
| `IntVecParam` | int2/3/4 vector | `int value[4]` + `componentCount` |
| `UintParam` | Scalar unsigned int | `uint32_t value` |
| `UintVecParam` | uint2/3/4 vector | `uint32_t value[4]` + `componentCount` |
| `BoolParam` | Boolean | `bool value` |

**Vector types use `componentCount`**: Instead of separate Float2Param/Float3Param/Float4Param classes, a single `FloatVecParam` class uses a `componentCount` field (2, 3, or 4) to track the vector size. Same pattern for IntVecParam and UintVecParam.

**Serialization format**: Vector parameters serialize to array-style keys: `ParamName[0]`, `ParamName[1]`, etc.

**Field editors** (`src/overlay/params/fields/`): Each param type has a corresponding field editor that renders the ImGui controls. Registered via `REGISTER_FIELD_EDITOR(ParamType, EditorClass)` macro.

Parameters are stored in `EffectConfig` within `EffectRegistry`. The UI renders these directly and modifies them in-place.

### Shaders

GLSL shaders in `src/shader/` are compiled to SPIR-V by glslangValidator at build time, then embedded as C headers.

To add a new shader:
1. Create `src/shader/myshader.frag.glsl`
2. Add to `src/shader/meson.build`
3. Include generated header in your effect

### Configuration

Config files are parsed at startup to populate EffectRegistry. After initialization, the registry is the source of truth.

Search order for `vkBasalt.conf`:
1. `$VKBASALT_CONFIG_FILE`
2. Game working directory
3. `~/.config/vkBasalt/`
4. `/etc/vkBasalt/`

### ImGui Overlay (imgui_overlay.cpp)

In-game UI toggled with End key (configurable). The overlay:
- Reads parameters directly from EffectRegistry
- Writes changes directly to EffectRegistry
- Triggers effect reload when Apply is clicked

### Key Structures

- **LogicalDevice** (`logical_device.hpp`): Wraps VkDevice with dispatch tables and queue info
- **LogicalSwapchain** (`logical_swapchain.hpp`): Per-swapchain state including effects vector, command buffers, and semaphores
- **EffectRegistry** (`effect_registry.hpp`): **THE** source of truth for effect configs and parameter values
- **EffectConfig** (`effect_config.hpp`): Per-effect configuration including parameters

### Input Handling

Input backends are compiled as separate static libraries to avoid symbol conflicts:

- **X11**: `keyboard_input_x11.cpp`, `mouse_input.cpp` — uses `XQueryKeymap`/`XQueryPointer` with separate `Display*`
- **Wayland**: `keyboard_input_wayland.cpp`, `mouse_input_wayland.cpp` — `wl_keyboard`/`wl_pointer` on a private `wl_event_queue`, with xkbcommon for key translation
- **Shared Wayland**: `wayland_input_common.cpp` — shared `wl_seat` and event queue for both keyboard and mouse
- **Input blocker**: `input_blocker.cpp` — X11: `XGrabPointer`/`XGrabKeyboard`; Wayland: sets atomic flag read by interposition wrapper
- **Wayland interposition**: `wayland_interpose.cpp` — interposes `wl_proxy_add_listener` to wrap game's pointer/keyboard listeners; suppresses events when `isInputBlocked()` returns true. Overlay proxies are registered via `registerOverlayProxy()` and passed through unwrapped.
- **Pointer constraints**: `wayland_pointer_constraints.cpp` — `zwp_confined_pointer_v1` (not used for input blocking, only for optional cursor confinement)

### Depth Detection Engine (Safe Anti-Cheat)

When `safeAntiCheat` is enabled per-profile, shaders that use the depth buffer are blocked to avoid anti-cheat detection. The detection must have **zero false negatives** (never allow a depth shader) and **zero false positives** (never block a safe shader like Vibrance).

**Problem**: ReShade.fxh and qUINT_common.fxh always declare depth texture + sampler + helper functions (e.g., `GetLinearizedDepth()`). These compile into the SPIR-V even when no entry point calls them. A naive scan would flag every shader that includes these headers.

**Solution** (`src/reshade_parser.cpp` + `tools/test_shaders.cpp`): SPIR-V call graph reachability analysis.

1. Find depth texture (semantic `"DEPTH"`) and its associated samplers in `module.textures`/`module.samplers`
2. Parse SPIR-V bytecode (skip 5-word header: magic, version, generator, bound, reserved)
3. Collect `OpEntryPoint` (opcode 15) function IDs
4. Build per-function map from `OpFunction` (54) / `OpFunctionEnd` (56) blocks:
   - Track `OpLoad` (61) where word 3 = depth sampler variable ID → marks function as "loads depth"
   - Track `OpFunctionCall` (57) → call graph edges
5. BFS from entry points through call graph — if any reachable function loads the depth sampler, `usesDepth = true`

**Key SPIR-V details**:
- Samplers are `StorageClassUniformConstant` — opaque handles, NOT loaded via regular `OpLoad` of data. But reshadefx DOES emit `OpLoad` for sampler variables to get the `SampledImage` value.
- The sampler variable ID (`sampler_info.id`) is assigned by `make_id()` in `define_sampler()`. It's the `result` operand of the `OpVariable` instruction.
- `module.spirv` starts with a 5-word SPIR-V header. Iterating from offset 0 misinterprets the magic number (`0x07230203`) as an instruction with word count 1827, skipping real instructions. **Always start iteration at offset 5.**

**Auto-test on overlay open**: When the Add Effects tab opens with safe anti-cheat enabled, `startShaderTest()` automatically compiles all shaders (one per frame via `processShaderTest()`). Results populate `depthShaders` set and show per-shader status in the UI. Memory is reclaimed via `malloc_trim()` every 25 shaders to prevent OOM.

## Code Patterns

- **Thread safety**: Global mutex `globalLock` protects all maps
- **Memory management**: `std::shared_ptr` for LogicalDevice, LogicalSwapchain
- **Format handling**: Always consider SRGB vs UNORM variants (`format.cpp`)
- **Logging**: Use `Logger::debug()`, `Logger::info()`, `Logger::err()`
- **Registry access**: Always use `EffectRegistry` for parameter values, never read from Config at runtime

## Code Style

- **Avoid nested ifs**: Keep code flat with single-level indentation where possible. Use early returns, early continues, and guard clauses instead of nesting conditions.
  ```cpp
  // Bad - deeply nested
  if (condition1) {
      if (condition2) {
          if (condition3) {
              doSomething();
          }
      }
  }

  // Good - flat with early returns
  if (!condition1)
      return;
  if (!condition2)
      return;
  if (!condition3)
      return;
  doSomething();
  ```

## Environment Variables

- `ENABLE_VKBASALT=1` - Enable the layer
- `DISABLE_VKBASALT=1` - Force disable
- `VKBASALT_LOG_LEVEL=debug` - Log levels: trace, debug, info, warn, error, none
- `VKBASALT_LOG_FILE=/path/to/log` - Output to file instead of stderr
- `VKBASALT_CONFIG_FILE=/path/to/conf` - Override config location

**Note:** This fork uses the same `ENABLE_VKBASALT` env var as the original because [gamescope filters known layer env vars](https://github.com/Boux/vkBasalt_overlay/issues/5#issuecomment-3706694598). The library/layer names are different to avoid file conflicts:
- Library: `libvkbasalt-overlay.so` (vs `libvkbasalt.so`)
- Layer: `VK_LAYER_VKBASALT_OVERLAY_post_processing` (vs `VK_LAYER_VKBASALT_post_processing`)
- JSON: `vkBasalt-overlay.json` (vs `vkBasalt.json`)

## User Preferences

- **File organization**: Proactively suggest splitting code into separate files when a file gets too large or a distinct responsibility emerges. Always inform the user before refactoring.
- **After meson reconfigure (debug only)**: During development, the `builddir/config/vkBasalt-overlay.json` library_path may revert to relative path. Manually edit it to use the absolute path to your build's `libvkbasalt-overlay.so`. This is not needed for actual builds/releases.
