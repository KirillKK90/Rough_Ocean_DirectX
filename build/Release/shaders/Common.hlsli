// Shared declarations for all graphics shaders.

SamplerState samLinearWrap  : register(s0);
SamplerState samLinearClamp : register(s1);
SamplerState samPoint       : register(s2);

static const float PI = 3.14159265359;

// Per-frame constants. Must match FrameCB in App.h exactly.
cbuffer FrameCB : register(b0)
{
    float4x4 gViewProj;     // camera-relative (no translation in view)
    float4x4 gInvViewProj;
    float3 gCamPos;         float gTime;          // absolute world-space camera position
    float3 gCamRight;       float gExposure;
    float3 gCamUp;          float gLightIsMoon;
    float3 gLightDir;       float gStarIntensity; // toward the light
    float3 gLightColor;     float gRoughBase;     // primary light radiance
    float3 gSunDiscColor;   float gLambda;        // disc radiance; wave choppiness
    float4 gCascade0;       // x = 1/L, y = fade distance, z = slope variance add, w = amplitude estimate
    float4 gCascade1;
    float4 gCascade2;
    float3 gWaterDeep;      float gFoamAmount;
    float3 gWaterScatter;   float gSSS;
    float3 gBuoyLightPos;   float gBuoyLightOn;   // camera-relative position
    float3 gBuoyLightColor; float gFogDensity;
    float2 gWindDir;        float gDistRough;     float gPad0;
}

// Per-object constants.
cbuffer ObjectCB : register(b1)
{
    float4x4 gWorld;    // camera-relative world transform
    float4 gEmissive;   // rgb color, w intensity
    float4 gMisc;       // billboard: xyz camera-relative center, w half-size
}

// ---------------------------------------------------------------------------

float3 ACESFilm(float3 x)
{
    // Narkowicz fitted ACES approximation.
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float ValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = Hash12(i);
    float b = Hash12(i + float2(1, 0));
    float c = Hash12(i + float2(0, 1));
    float d = Hash12(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float Fbm(float2 p, int octaves)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < octaves; ++i)
    {
        v += a * ValueNoise(p);
        p = p * 2.13 + 17.7;
        a *= 0.5;
    }
    return v;
}

// GGX microfacet pieces.
float D_GGX(float NdH, float a)
{
    float a2 = a * a;
    float d = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

float V_SmithApprox(float NdL, float NdV, float a)
{
    // Karis' approximation of height-correlated Smith visibility.
    return 0.5 / max(lerp(2.0 * NdL * NdV, NdL + NdV, a), 1e-5);
}

// Fullscreen triangle.
struct FullVSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

FullVSOut VSFullscreen(uint vid : SV_VertexID)
{
    FullVSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// View ray in (camera-relative) world space for a fullscreen pixel.
float3 ViewRayFromUv(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 p = mul(float4(ndc, 0.5, 1.0), gInvViewProj);
    return normalize(p.xyz / p.w);
}
