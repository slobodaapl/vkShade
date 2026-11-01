texture2D BackBuffer : COLOR;
sampler2D BackBufferSampler { Texture = BackBuffer; };

void Vertex(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Yellow(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return float4(tex2D(BackBufferSampler, uv).r, 1.0, 0.0, 1.0);
}

technique Only
{
    pass { VertexShader = Vertex; PixelShader = Yellow; }
}
