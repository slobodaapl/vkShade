uniform bool ShowPreview < ui_category = "Preview"; ui_category_toggle = true; > = false;
uniform bool FlipPreview < ui_category = "Preview"; > = false;

float4 Vertex(uint id : SV_VertexID) : SV_Position
{
    float2 position = float2((id << 1) & 2, id & 2);
    return float4(position * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 Pixel(float4 position : SV_Position) : SV_Target
{
    return ShowPreview && FlipPreview ? float4(1, 1, 1, 1) : float4(0, 0, 0, 1);
}

technique Preview
{
    pass { VertexShader = Vertex; PixelShader = Pixel; }
}
