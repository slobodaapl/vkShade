texture2D BackBuffer : COLOR;
sampler2D BackBufferSampler { Texture = BackBuffer; };
texture2D SharedTexture { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler2D SharedSampler { Texture = SharedTexture; };

void Vertex(uint id : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD)
{
    uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 WriteShared(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return float4(0.0, 0.0, 1.0, 1.0);
}

float4 WriteRed(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return float4(1.0, 0.0, 0.0, 1.0);
}

float4 Combine(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 color = tex2D(BackBufferSampler, uv);
    float4 stored = tex2D(SharedSampler, uv);
    return float4(color.r, color.g, stored.b, 1.0);
}

technique First
{
    pass { VertexShader = Vertex; PixelShader = WriteShared; RenderTarget = SharedTexture; }
    pass { VertexShader = Vertex; PixelShader = WriteRed; }
}

technique Second
{
    pass { VertexShader = Vertex; PixelShader = Combine; }
}
