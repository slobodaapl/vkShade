float4 Vertex(uint id : SV_VertexID) : SV_Position
{
    float2 position = float2((id << 1) & 2, id & 2);
    return float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Red(float4 position : SV_Position) : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0);
}

float4 Green(float4 position : SV_Position) : SV_Target
{
    return float4(0.0, 1.0, 0.0, 1.0);
}

float4 Blue(float4 position : SV_Position) : SV_Target
{
    return float4(0.0, 0.0, 1.0, 1.0);
}

technique First < ui_label = "First look"; ui_tooltip = "First tooltip"; >
{
    pass { VertexShader = Vertex; PixelShader = Red; }
}

technique Second < enabled = true; noedit = true; >
{
    pass { VertexShader = Vertex; PixelShader = Green; }
}

technique Hidden < hidden = 1; >
{
    pass { VertexShader = Vertex; PixelShader = Blue; }
}

#ifndef EXTRA_TECHNIQUE
#define EXTRA_TECHNIQUE 0
#endif
#if EXTRA_TECHNIQUE
technique Extra
{
    pass { VertexShader = Vertex; PixelShader = Red; }
}
#endif
