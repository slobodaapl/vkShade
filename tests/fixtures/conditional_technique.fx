#ifndef ENABLE_TECHNIQUE
#define ENABLE_TECHNIQUE 1
#endif

#if ENABLE_TECHNIQUE
float4 Vertex(uint id : SV_VertexID) : SV_Position
{
    float2 position = float2((id << 1) & 2, id & 2);
    return float4(position * float2(2.0, -1.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Pixel(float4 position : SV_Position) : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0);
}

technique Example
{
    pass { VertexShader = Vertex; PixelShader = Pixel; }
}
#endif
