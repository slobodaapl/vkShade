#ifndef LOCAL_OPTION
#define LOCAL_OPTION 1
#endif
#ifndef TEST_NUMERIC_DEPTH_ALIASES
#define TEST_NUMERIC_DEPTH_ALIASES 0
#endif

#define RESHADE_DEPTH_INPUT_IS_REVERSED 0
#undef DEPTH_INPUT_IS_REVERSED
#define DEPTH_INPUT_IS_REVERSED 0

#if DEPTH_INPUT_IS_REVERSED != RESHADE_DEPTH_INPUT_IS_REVERSED
#error Depth reversal aliases must agree
#endif
#if DEPTH_INPUT_IS_UPSIDE_DOWN != RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN
#error Depth orientation aliases must agree
#endif
#if DEPTH_INPUT_IS_MIRRORED != RESHADE_DEPTH_INPUT_IS_MIRRORED
#error Depth mirror aliases must agree
#endif
#if DEPTH_INPUT_IS_LOGARITHMIC != RESHADE_DEPTH_INPUT_IS_LOGARITHMIC
#error Logarithmic depth aliases must agree
#endif

#if TEST_NUMERIC_DEPTH_ALIASES
#if DEPTH_INPUT_X_SCALE != RESHADE_DEPTH_INPUT_X_SCALE || DEPTH_INPUT_Y_SCALE != RESHADE_DEPTH_INPUT_Y_SCALE
#error Depth scale aliases must agree
#endif
#if DEPTH_INPUT_X_OFFSET != RESHADE_DEPTH_INPUT_X_OFFSET || DEPTH_INPUT_Y_OFFSET != RESHADE_DEPTH_INPUT_Y_OFFSET
#error Depth offset aliases must agree
#endif
#if DEPTH_INPUT_X_PIXEL_OFFSET != RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET || DEPTH_INPUT_Y_PIXEL_OFFSET != RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET
#error Depth pixel offset aliases must agree
#endif
#if DEPTH_LINEARIZATION_FAR_PLANE != RESHADE_DEPTH_LINEARIZATION_FAR_PLANE || DEPTH_MULTIPLIER != RESHADE_DEPTH_MULTIPLIER
#error Depth linearization aliases must agree
#endif
#endif

float4 Vertex(uint id : SV_VertexID) : SV_Position
{
    float2 position = float2((id << 1) & 2, id & 2);
    return float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Pixel(float4 position : SV_Position) : SV_Target
{
    return float4(DEPTH_INPUT_X_SCALE, RESHADE_DEPTH_INPUT_X_SCALE, LOCAL_OPTION, 1.0);
}

#if DEPTH_INPUT_IS_REVERSED
technique Reversed
#else
technique Forward
#endif
{
    pass { VertexShader = Vertex; PixelShader = Pixel; }
}
