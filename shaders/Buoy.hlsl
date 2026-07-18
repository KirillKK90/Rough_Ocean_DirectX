// Buoy rendering: simple vertex-colored PBR-ish shading plus an additive
// billboard for the lamp glow.

#include "Common.hlsli"

TextureCube<float4> tSky : register(t0);

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float3 col : COLOR;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 rel : TEXCOORD0;
    float3 nrm : TEXCOORD1;
    float3 col : TEXCOORD2;
};

VSOut VSBuoy(VSIn v)
{
    VSOut o;
    float3 rel = mul(float4(v.pos, 1.0), gWorld).xyz;
    o.pos = mul(float4(rel, 1.0), gViewProj);
    o.rel = rel;
    o.nrm = normalize(mul(v.nrm, (float3x3)gWorld));
    o.col = v.col;
    return o;
}

float4 PSBuoy(VSOut i) : SV_Target
{
    float3 N = normalize(i.nrm);
    float dist = max(length(i.rel), 1e-3);
    float3 V = -i.rel / dist;
    float3 L = gLightDir;

    float NdL = saturate(dot(N, L));
    float NdV = max(dot(N, V), 1e-3);
    float3 H = normalize(V + L);

    float rough = 0.5;
    float a = rough * rough;
    float3 ambient = tSky.SampleLevel(samLinearClamp, N, 4.0).rgb;

    float3 albedo = i.col;
    float3 col = albedo * (NdL * gLightColor * (1.0 / PI) + ambient * 0.7);

    float fs = 0.04 + 0.96 * pow(1.0 - saturate(dot(V, H)), 5.0);
    col += D_GGX(saturate(dot(N, H)), a) * V_SmithApprox(NdL, NdV, a) * fs * NdL * gLightColor;

    // Environment sheen at grazing angles.
    float3 R = reflect(-V, N);
    float fres = 0.04 + 0.96 * pow(1.0 - NdV, 5.0);
    col += tSky.SampleLevel(samLinearClamp, R, 3.0).rgb * fres * 0.4;

    // Lamp emissive (nonzero only on the lamp draw call).
    col += gEmissive.rgb * gEmissive.w;

    // Buoy lamp lighting the buoy's own structure.
    if (gBuoyLightOn > 0.0)
    {
        float3 toL = gBuoyLightPos - i.rel;
        float ld = max(length(toL), 0.05);
        float atten = gBuoyLightOn / (1.0 + 2.0 * ld + 3.0 * ld * ld);
        col += gBuoyLightColor * atten * saturate(dot(N, toL / ld));
    }

    return float4(col, 1.0);
}

// --- Additive lamp glow billboard ---
struct GlowVSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

GlowVSOut VSGlow(uint vid : SV_VertexID)
{
    // Two-triangle strip quad: (-1,-1) (1,-1) (-1,1) (1,1)
    float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
    float3 rel = gMisc.xyz + (gCamRight * c.x + gCamUp * c.y) * gMisc.w;
    GlowVSOut o;
    o.pos = mul(float4(rel, 1.0), gViewProj);
    o.uv = c;
    return o;
}

float4 PSGlow(GlowVSOut i) : SV_Target
{
    float r2 = dot(i.uv, i.uv);
    float core = exp(-r2 * 14.0) * 2.0;
    float halo = exp(-r2 * 3.0) * 0.35;
    float3 col = gEmissive.rgb * gEmissive.w * (core + halo);
    return float4(col, 1.0);
}
