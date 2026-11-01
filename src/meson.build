subdir('shader')
subdir('reshade')

imgui_src = [
    'imgui/imgui.cpp',
    'imgui/imgui_draw.cpp',
    'imgui/imgui_tables.cpp',
    'imgui/imgui_widgets.cpp',
    'imgui/backends/imgui_impl_vulkan.cpp',
]

overlay_src = [
    'overlay/async_texture_upload.cpp',
    'overlay/imgui_overlay.cpp',
    'overlay/preprocessor_edit.cpp',
    'overlay/shader_metadata_loader.cpp',
    'overlay/view_configs.cpp',
    'overlay/view_debug.cpp',
    'overlay/view_diagnostics.cpp',
    'overlay/view_settings.cpp',
    'overlay/view_shader_manager.cpp',
    'overlay/view_shader_test.cpp',
    'overlay/view_main.cpp',
    'overlay/view_techniques.cpp',
    'overlay/params/field_editor.cpp',
    'overlay/params/fields/field_bool.cpp',
    'overlay/params/fields/field_float.cpp',
    'overlay/params/fields/field_floatvec.cpp',
    'overlay/params/fields/field_int.cpp',
    'overlay/params/fields/field_intvec.cpp',
    'overlay/params/fields/field_uint.cpp',
    'overlay/params/fields/field_uintvec.cpp',
]

vkShade_src = [
    'vkshade.cpp',
    'buffer.cpp',
    'command_buffer.cpp',
    'depth_resolve_resources.cpp',
    'depth_copy_submission.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'settings_manager.cpp',
    'technique_id.cpp',
    'technique_order.cpp',
    'technique_plan.cpp',
    'descriptor_set.cpp',
    'effects/effect.cpp',
    'effects/effect_registry.cpp',
    'effects/effect_reshade.cpp',
    'effects/technique_passes.cpp',
    'effects/effect_simple.cpp',
    'effects/effect_transfer.cpp',
    'reshade_parser.cpp',
    'shader_metadata.cpp',
    'fake_swapchain.cpp',
    'format.cpp',
    'framebuffer.cpp',
    'graphics_pipeline.cpp',
    'image.cpp',
    'image_view.cpp',
    'keyboard_input.cpp',
    'logger.cpp',
    'logical_swapchain.cpp',
    'memory.cpp',
    'renderpass.cpp',
    'reshade_uniforms.cpp',
    'sampler.cpp',
    'shader.cpp',
    'stb_image.c',
    'stb_image_resize.c',
    'depth_image_capture.cpp',
    'depth_frame_tracker.cpp',
    'depth_clear_capture.cpp',
    'util.cpp',
    'vkdispatch.cpp',
    'wayland_pointer_geometry.cpp',
]

x11_dep = dependency('x11')
xi_dep = dependency('xi')
wayland_client_dep = dependency('wayland-client')
wayland_server_dep = dependency('wayland-server')
xkbcommon_dep = dependency('xkbcommon')
wayland_protocols_dep = dependency('wayland-protocols')
wayland_scanner = find_program('wayland-scanner')
dl_dep = cpp.find_library('dl')

# Generate pointer-constraints-unstable-v1 protocol code
wl_protocols_dir = wayland_protocols_dep.get_variable('pkgdatadir')
pointer_constraints_xml = join_paths(
    wl_protocols_dir,
    'unstable',
    'pointer-constraints',
    'pointer-constraints-unstable-v1.xml',
)

pointer_constraints_client_h = custom_target(
    'pointer-constraints-client-header',
    input: pointer_constraints_xml,
    output: 'pointer-constraints-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

pointer_constraints_code = custom_target(
    'pointer-constraints-code',
    input: pointer_constraints_xml,
    output: 'pointer-constraints-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

relative_pointer_xml = join_paths(
    wl_protocols_dir,
    'unstable',
    'relative-pointer',
    'relative-pointer-unstable-v1.xml',
)
relative_pointer_client_h = custom_target(
    'relative-pointer-client-header',
    input: relative_pointer_xml,
    output: 'relative-pointer-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
relative_pointer_code = custom_target(
    'relative-pointer-code',
    input: relative_pointer_xml,
    output: 'relative-pointer-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
relative_pointer_server_h = custom_target(
    'relative-pointer-server-header',
    input: relative_pointer_xml,
    output: 'relative-pointer-server-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

viewporter_xml = join_paths(
    wl_protocols_dir,
    'stable',
    'viewporter',
    'viewporter.xml',
)
viewporter_client_h = custom_target(
    'viewporter-client-header',
    input: viewporter_xml,
    output: 'viewporter-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
viewporter_code = custom_target(
    'viewporter-code',
    input: viewporter_xml,
    output: 'viewporter-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

conf_paths = configuration_data()
conf_paths.set_quoted(
    'SYSCONFDIR',
    join_paths(get_option('prefix'), get_option('sysconfdir')),
)
conf_paths.set_quoted(
    'DATADIR',
    join_paths(get_option('prefix'), get_option('datadir')),
)
conf_paths.set_quoted(
    'BINDIR',
    join_paths(get_option('prefix'), get_option('bindir')),
)

configure_file(output: 'config_paths.hpp', configuration: conf_paths)

# X11/X.h library contains many definitions (e.g. None)
# which conflicts with the rest codebase with with Meson --unity=on option
keyboard_input_x11_lib = static_library(
    'keyboard_input_x11',
    'keyboard_input_x11.cpp',
)
mouse_input_lib = static_library('mouse_input', 'mouse_input.cpp')
input_blocker_lib = static_library('input_blocker', 'input_blocker.cpp')

# Wayland input backends — built as static libs to isolate wayland-client headers.
# wayland_input_common provides a shared seat/queue used by both keyboard and mouse.
wayland_input_common_lib = static_library(
    'wayland_input_common',
    'wayland_input_common.cpp',
    'wayland_display.cpp',
    relative_pointer_code,
    relative_pointer_client_h,
    dependencies: [wayland_client_dep],
)
keyboard_input_wayland_lib = static_library(
    'keyboard_input_wayland',
    'keyboard_input_wayland.cpp',
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep, xkbcommon_dep],
)
mouse_input_wayland_lib = static_library(
    'mouse_input_wayland',
    'mouse_input_wayland.cpp',
    relative_pointer_client_h,
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep],
)
wayland_pointer_constraints_lib = static_library(
    'wayland_pointer_constraints',
    'wayland_pointer_constraints.cpp',
    pointer_constraints_code,
    pointer_constraints_client_h,
    link_with: [wayland_input_common_lib],
    dependencies: [wayland_client_dep],
)
# Wayland event interposition — wraps game's pointer/keyboard listeners to
# suppress events when overlay has input blocked. Needs default visibility
# for the wl_proxy_add_listener symbol override.
wayland_interpose_lib = static_library(
    'wayland_interpose',
    'wayland_interpose.cpp',
    viewporter_client_h,
    pointer_constraints_client_h,
    relative_pointer_client_h,
    dependencies: [wayland_client_dep],
    c_args: ['-fvisibility=default'],
    cpp_args: ['-fvisibility=default'],
)

imgui_inc = include_directories('imgui')
overlay_inc = include_directories('overlay')
effects_inc = include_directories('effects')
effects_params_inc = include_directories('effects/params')

vkshade_library = shared_library(
    'vkshade',
    vkShade_src,
    overlay_src,
    imgui_src,
    shader_include,
    link_with: [
        keyboard_input_x11_lib,
        mouse_input_lib,
        input_blocker_lib,
        wayland_input_common_lib,
        keyboard_input_wayland_lib,
        mouse_input_wayland_lib,
        wayland_pointer_constraints_lib,
        wayland_interpose_lib,
    ],
    include_directories: [
        vkShade_include_path,
        imgui_inc,
        overlay_inc,
        effects_inc,
        effects_params_inc,
    ],
    cpp_args: ['-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES'],
    dependencies: [
        x11_dep,
        xi_dep,
        wayland_client_dep,
        xkbcommon_dep,
        reshade_dep,
    ],
    gnu_symbol_visibility: 'hidden',
    install: true,
    install_dir: lib_dir,
)

shader_checker = executable(
    'vkshade-shader-check',
    'shader_check.cpp',
    'shader_metadata.cpp',
    'reshade_parser.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'logger.cpp',
    include_directories: [
        vkShade_include_path,
        imgui_inc,
        overlay_inc,
        effects_inc,
        effects_params_inc,
    ],
    dependencies: [reshade_dep],
    install: true,
)

shader_metadata_loader_test = executable(
    'shader-metadata-loader-test',
    '../tests/shader_metadata_loader.cpp',
    'overlay/shader_metadata_loader.cpp',
    'shader_metadata.cpp',
    include_directories: [vkShade_include_path, effects_inc, effects_params_inc],
    dependencies: [dl_dep],
)
test(
    'shader-metadata-loader',
    shader_metadata_loader_test,
    args: [
        join_paths(
            meson.current_source_dir(),
            '..',
            'tests',
            'fixtures',
            'multiple_techniques.fx',
        ),
    ],
    depends: [shader_checker],
)

depth_selection_test = executable(
    'depth-selection-test',
    '../tests/depth_selection.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'logger.cpp',
    include_directories: [vkShade_include_path, effects_inc, effects_params_inc],
)
depth_frame_tracker_test = executable(
    'depth-frame-tracker-test',
    '../tests/depth_frame_tracker.cpp',
    'depth_frame_tracker.cpp',
    include_directories: [vkShade_include_path],
)
test('depth-frame-tracker', depth_frame_tracker_test)
depth_copy_submission_test = executable(
    'depth-copy-submission-test',
    '../tests/depth_copy_submission.cpp',
    'depth_copy_submission.cpp',
    'overlay/shader_metadata_loader.cpp',
    'shader_metadata.cpp',
    'logger.cpp',
    include_directories: [vkShade_include_path, effects_inc, effects_params_inc],
    dependencies: [dl_dep],
)
test('depth-copy-submission', depth_copy_submission_test)
depth_resolve_copy_test = executable(
    'depth-resolve-copy-test',
    '../tests/depth_resolve_copy.cpp',
    'depth_resolve_resources.cpp',
    'format.cpp',
    'overlay/shader_metadata_loader.cpp',
    'shader_metadata.cpp',
    'logger.cpp',
    include_directories: [vkShade_include_path, effects_inc, effects_params_inc],
    dependencies: [dl_dep],
)
test('depth-resolve-copy', depth_resolve_copy_test)
effect_profile_persistence_test = executable(
    'effect-profile-persistence-test',
    '../tests/effect_profile_persistence.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'logger.cpp',
    'technique_id.cpp',
    include_directories: [vkShade_include_path, effects_inc, effects_params_inc],
)
test(
    'effect-profile-persistence',
    effect_profile_persistence_test,
    args: [meson.current_build_dir()],
    env: ['VKSHADE_LOG_LEVEL=none'],
)
effect_registry_test = executable(
    'effect-registry-test',
    '../tests/effect_registry.cpp',
    'effects/effect_registry.cpp',
    'reshade_parser.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'logger.cpp',
    include_directories: [vkShade_include_path, effects_inc, effects_params_inc],
    dependencies: [reshade_dep],
)
test(
    'effect-registry',
    effect_registry_test,
    args: [
        meson.current_build_dir(),
        files('../tests/fixtures/multiple_techniques.fx'),
    ],
    env: ['VKSHADE_LOG_LEVEL=none'],
)
game_identity_test = executable(
    'game-identity-test',
    '../tests/game_identity.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'logger.cpp',
    'technique_id.cpp',
    include_directories: [vkShade_include_path, effects_inc, effects_params_inc],
)
test(
    'game-identity',
    game_identity_test,
    args: [meson.current_build_dir()],
    env: ['VKSHADE_LOG_LEVEL=none'],
)
reshade_technique_metadata_test = executable(
    'reshade-technique-metadata-test',
    '../tests/reshade_technique_metadata.cpp',
    'reshade_parser.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'logger.cpp',
    include_directories: [
        vkShade_include_path,
        imgui_inc,
        overlay_inc,
        effects_inc,
        effects_params_inc,
    ],
    dependencies: [reshade_dep],
)
test(
    'reshade-technique-metadata',
    reshade_technique_metadata_test,
    args: files('../tests/fixtures/multiple_techniques.fx'),
    env: ['VKSHADE_LOG_LEVEL=none'],
)
technique_id_test = executable(
    'technique-id-test',
    '../tests/technique_id.cpp',
    'technique_id.cpp',
    include_directories: [vkShade_include_path],
)
test('technique-id', technique_id_test)
technique_order_test = executable(
    'technique-order-test',
    '../tests/technique_order.cpp',
    'technique_id.cpp',
    'technique_order.cpp',
    include_directories: [vkShade_include_path],
)
test('technique-order', technique_order_test)
technique_plan_test = executable(
    'technique-plan-test',
    '../tests/technique_plan.cpp',
    'technique_id.cpp',
    'technique_plan.cpp',
    include_directories: [vkShade_include_path, effects_inc, effects_params_inc],
)
test('technique-plan', technique_plan_test)
technique_passes_test = executable(
    'technique-passes-test',
    '../tests/technique_passes.cpp',
    'effects/technique_passes.cpp',
    include_directories: [vkShade_include_path],
)
test('technique-passes', technique_passes_test)
depth_capture_settings_test = executable(
    'depth-capture-settings-test',
    '../tests/depth_capture_settings.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'settings_manager.cpp',
    'logger.cpp',
    include_directories: [vkShade_include_path],
)
test(
    'depth-capture-settings',
    depth_capture_settings_test,
    args: [meson.current_build_dir()],
    env: ['VKSHADE_LOG_LEVEL=none'],
)
overlay_font_settings_test = executable(
    'overlay-font-settings-test',
    '../tests/overlay_font_settings.cpp',
    'config.cpp',
    'config_serializer.cpp',
    'settings_manager.cpp',
    'logger.cpp',
    include_directories: [vkShade_include_path],
)
test(
    'overlay-font-settings',
    overlay_font_settings_test,
    args: [meson.current_build_dir()],
    env: ['VKSHADE_LOG_LEVEL=none'],
)
depth_image_capture_test = executable(
    'depth-image-capture-test',
    '../tests/depth_image_capture.cpp',
    'depth_image_capture.cpp',
    'format.cpp',
    'logger.cpp',
    include_directories: [
        vkShade_include_path,
        imgui_inc,
        overlay_inc,
        effects_inc,
        effects_params_inc,
    ],
)
test(
    'depth-image-capture',
    depth_image_capture_test,
    env: ['VKSHADE_LOG_LEVEL=none'],
)
swapchain_usage_test = executable(
    'swapchain-usage-test',
    '../tests/swapchain_usage.cpp',
    include_directories: [vkShade_include_path],
)
test('swapchain-usage', swapchain_usage_test)
wayland_pointer_geometry_test = executable(
    'wayland-pointer-geometry-test',
    '../tests/wayland_pointer_geometry.cpp',
    'wayland_pointer_geometry.cpp',
    include_directories: [vkShade_include_path],
    dependencies: [wayland_client_dep],
)
test('wayland-pointer-geometry', wayland_pointer_geometry_test)
preprocessor_edit_test = executable(
    'preprocessor-edit-test',
    '../tests/preprocessor_edit.cpp',
    'overlay/preprocessor_edit.cpp',
    'overlay/shader_metadata_loader.cpp',
    'shader_metadata.cpp',
    'imgui/imgui.cpp',
    'imgui/imgui_draw.cpp',
    'imgui/imgui_tables.cpp',
    'imgui/imgui_widgets.cpp',
    include_directories: [
        vkShade_include_path,
        imgui_inc,
        overlay_inc,
        effects_inc,
    ],
    dependencies: [dl_dep],
)
test(
    'preprocessor-edit',
    preprocessor_edit_test,
    args: files(
        '../tests/fixtures/multiple_techniques.fx',
        '../tests/fixtures/conditional_technique.fx',
    ),
    depends: [shader_checker],
)
wayland_interpose_test = executable(
    'wayland-interpose-test',
    '../tests/wayland_interpose.cpp',
    'wayland_interpose.cpp',
    'input_blocker.cpp',
    'wayland_pointer_geometry.cpp',
    'logger.cpp',
    pointer_constraints_code,
    pointer_constraints_client_h,
    relative_pointer_code,
    relative_pointer_client_h,
    viewporter_code,
    viewporter_client_h,
    include_directories: [vkShade_include_path],
    dependencies: [wayland_client_dep, dl_dep],
    export_dynamic: true,
)
test(
    'wayland-interpose',
    wayland_interpose_test,
    env: ['VKSHADE_LOG_LEVEL=none'],
)
dlsym_next_outer = shared_library(
    'dlsym-next-outer',
    '../tests/fixtures/dlsym_next_outer.cpp',
    dependencies: [dl_dep],
    install: false,
)
dlsym_next_inner = shared_library(
    'dlsym-next-inner',
    '../tests/fixtures/dlsym_next_inner.cpp',
    install: false,
)
dlsym_next_test = executable(
    'dlsym-next-test',
    '../tests/dlsym_next.cpp',
    dependencies: [dl_dep],
)
test(
    'dlsym-next',
    dlsym_next_test,
    env: [
        'ENABLE_VKSHADE=1',
        'VKSHADE_LOG_LEVEL=none',
        'LD_PRELOAD=' + vkshade_library.full_path() + ':' + dlsym_next_outer.full_path() + ':' + dlsym_next_inner.full_path(),
    ],
    depends: [vkshade_library, dlsym_next_outer, dlsym_next_inner],
    timeout: 10,
)
wayland_mouse_input_test = executable(
    'wayland-mouse-input-test',
    '../tests/wayland_mouse_input.cpp',
    'mouse_input_wayland.cpp',
    'keyboard_input_wayland.cpp',
    'wayland_input_common.cpp',
    'wayland_pointer_geometry.cpp',
    'logger.cpp',
    relative_pointer_code,
    relative_pointer_client_h,
    relative_pointer_server_h,
    include_directories: [vkShade_include_path],
    dependencies: [wayland_client_dep, wayland_server_dep, xkbcommon_dep],
)
test(
    'wayland-mouse-input',
    wayland_mouse_input_test,
    env: ['VKSHADE_LOG_LEVEL=none'],
)
async_texture_upload_test = executable(
    'async-texture-upload-test',
    '../tests/async_texture_upload.cpp',
    'overlay/async_texture_upload.cpp',
    include_directories: [vkShade_include_path],
)
test('async-texture-upload', async_texture_upload_test)
vulkan_helpers_test = executable(
    'vulkan-helpers-test',
    '../tests/vulkan_helpers.cpp',
    'descriptor_set.cpp',
    'image_view.cpp',
    'shader.cpp',
    'logger.cpp',
    'overlay/shader_metadata_loader.cpp',
    'shader_metadata.cpp',
    include_directories: [
        vkShade_include_path,
        imgui_inc,
        overlay_inc,
        effects_inc,
        effects_params_inc,
    ],
    dependencies: [dl_dep],
)
test('vulkan-helpers', vulkan_helpers_test, env: ['VKSHADE_LOG_LEVEL=none'])
fake_swapchain_test = executable(
    'fake-swapchain-test',
    '../tests/fake_swapchain.cpp',
    'fake_swapchain.cpp',
    'format.cpp',
    'memory.cpp',
    'logger.cpp',
    'overlay/shader_metadata_loader.cpp',
    'shader_metadata.cpp',
    include_directories: [
        vkShade_include_path,
        imgui_inc,
        overlay_inc,
        effects_inc,
        effects_params_inc,
    ],
    dependencies: [dl_dep],
)
test('fake-swapchain', fake_swapchain_test, env: ['VKSHADE_LOG_LEVEL=none'])
test('vkcube-launcher', find_program('../scripts/test-vkcube.sh'))
test(
    'depth-selection',
    depth_selection_test,
    args: [join_paths(meson.current_build_dir(), 'depth-selection-test.conf')],
)

# LD_AUDIT library for Wine Wayland input interposition.
# Redirects wl_proxy_add_listener bindings from dlopen(RTLD_LOCAL) scopes
# (e.g. Wine's winewayland.so) to our wrapper in libvkshade.so.
shared_library(
    'vkshade-audit',
    'wayland_audit.c',
    dependencies: [dl_dep],
    install: true,
    install_dir: lib_dir,
)

vkshade_launcher_conf = configuration_data()
vkshade_launcher_conf.set_quoted(
    'VKSHADE_LIB',
    join_paths(get_option('prefix'), lib_dir, 'libvkshade.so'),
)
vkshade_launcher_conf.set_quoted(
    'VKSHADE_AUDIT_LIB',
    join_paths(get_option('prefix'), lib_dir, 'libvkshade-audit.so'),
)

vkshade_launcher = configure_file(
    input: '../scripts/vkshade.in',
    output: 'vkshade',
    configuration: vkshade_launcher_conf,
)

install_data(
    vkshade_launcher,
    install_dir: get_option('bindir'),
    install_mode: ['rwxr-xr-x'],
)

test(
    'vkshade-launcher',
    find_program('../scripts/test-vkshade.sh'),
    args: [
        vkshade_launcher,
        files('../scripts/vkshade.sh'),
        meson.current_build_dir(),
        join_paths(meson.current_build_dir(), 'libvkshade.so'),
        join_paths(meson.current_build_dir(), 'libvkshade-audit.so'),
    ],
)
