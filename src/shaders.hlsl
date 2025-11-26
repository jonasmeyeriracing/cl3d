cbuffer CameraConstants : register(b0)
{
    float4x4 viewProjection;
    float3 cameraPos;
    float padding;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPos : WORLDPOS;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.worldPos = input.position;
    output.position = mul(viewProjection, float4(input.position, 1.0));
    output.normal = input.normal;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // Grid pattern
    float2 grid = frac(input.uv * 100.0); // 100 grid lines across 1km = 10m spacing
    float lineWidth = 0.02;
    float gridLine = (grid.x < lineWidth || grid.y < lineWidth) ? 1.0 : 0.0;

    // Base color with grid overlay
    float3 baseColor = float3(0.3, 0.35, 0.3);
    float3 lineColor = float3(0.15, 0.2, 0.15);
    float3 color = lerp(baseColor, lineColor, gridLine);

    // Simple distance fog
    float dist = length(input.worldPos - cameraPos);
    float fog = saturate(dist / 2000.0);
    float3 fogColor = float3(0.5, 0.6, 0.7);
    color = lerp(color, fogColor, fog);

    return float4(color, 1.0);
}
