#include "Local.fxh"

uniform float LocalAmount < ui_type = "slider"; > = LOCAL_GAIN;

float4 Vertex(uint id : SV_VertexID) : SV_Position
{
    float2 position = float2((id << 1) & 2, id & 2);
    return float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 LocalPS(float4 position : SV_Position) : SV_Target
{
    return float4(LocalAmount, 0.0, 0.0, 1.0);
}

technique Local
{
    pass { VertexShader = Vertex; PixelShader = LocalPS; }
}
