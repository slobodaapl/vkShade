shader_src = [
    'depth_resolve.frag.glsl',
    'full_screen_triangle.vert.glsl',
    'present_copy.frag.glsl',
]

glsl_compiler = find_program('glslangValidator')
glsl_generator = generator(
    glsl_compiler,
    output: ['@BASENAME@.h'],
    arguments: ['-V', '-x', '@INPUT@', '-o', '@OUTPUT@'],
)

shader_include = glsl_generator.process(shader_src)
