// Meteorite: glowing rock mesh + additive billboard particles for the fiery
// smoke trail, splash spray and glow.

#include "Common.hlsli"

// --- Rock ---
struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float3 col : COLOR;
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 nrm : TEXCOORD0;
    float3 col : TEXCOORD1;
    float3 rel : TEXCOORD2;
};

VSOut VSRock(VSIn v)
{
    VSOut o;
    float3 rel = mul(float4(v.pos, 1.0), gWorld).xyz;
    o.pos = mul(float4(rel, 1.0), gViewProj);
    o.nrm = normalize(mul(v.nrm, (float3x3)gWorld));
    o.col = v.col;
    o.rel = rel;
    return o;
}

float4 PSRock(VSOut i) : SV_Target
{
    float3 N = normalize(i.nrm);
    float NdL = saturate(dot(N, gLightDir));
    // Dark rock body, dominated by the emissive heat glow.
    float3 col = i.col * (NdL * gLightColor * 0.20 + 0.25) + gEmissive.rgb * gEmissive.w;
    return float4(col, 1.0);
}

// --- Trail / spray particles (camera-facing quads, additive) ---
struct Particle
{
    float4 posSize; // world xyz, half-size
    float4 color;   // rgb, intensity
};

StructuredBuffer<Particle> tParticles : register(t0);

struct PVSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 col : TEXCOORD1;
};

PVSOut VSParticle(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    Particle p = tParticles[iid];
    float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
    float3 rel = (p.posSize.xyz - gCamPos) + (gCamRight * c.x + gCamUp * c.y) * p.posSize.w;
    PVSOut o;
    o.pos = mul(float4(rel, 1.0), gViewProj);
    o.uv = c;
    o.col = p.color;
    return o;
}

float4 PSParticle(PVSOut i) : SV_Target
{
    float r2 = dot(i.uv, i.uv);
    float a = exp(-r2 * 3.0) * i.col.a;
    return float4(i.col.rgb * a, 1.0);
}
