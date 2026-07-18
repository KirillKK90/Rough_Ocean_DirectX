// Atmosphere: single-scattering Rayleigh+Mie raymarch baked into a small
// cubemap whenever the time of day changes (CSSkyGen + CSSkyMip), then a
// fullscreen skybox pass (VSSky/PSSky) that adds the sun disc, moon and stars
// at full resolution.

#include "Common.hlsli"

// --- Compute: cubemap generation ---
cbuffer SkyCB : register(b0)
{
    float3 gSunDir;  float gSunI;
    float3 gMoonDir; float gMoonI;
    float gCloudCover; float gRes; uint gMipSrc; float gPadS;
}

RWTexture2DArray<float4> uCube : register(u2);
Texture2DArray<float4> tPrevMip : register(t2);

static const float Re = 6371e3;         // planet radius
static const float Ra = 6431e3;         // atmosphere top
static const float3 BetaR = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float BetaM = 4.5e-6;
static const float Hr = 8500.0;
static const float Hm = 1200.0;

float2 RaySphere(float3 ro, float3 rd, float radius)
{
    // returns (tNear, tFar), tFar < 0 if no hit
    float b = dot(ro, rd);
    float c = dot(ro, ro) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0)
        return float2(1e9, -1e9);
    float s = sqrt(disc);
    return float2(-b - s, -b + s);
}

float PhaseRayleigh(float mu)
{
    return 3.0 / (16.0 * PI) * (1.0 + mu * mu);
}

float PhaseMie(float mu)
{
    float g = 0.76;
    float g2 = g * g;
    return 3.0 / (8.0 * PI) * ((1.0 - g2) * (1.0 + mu * mu))
         / ((2.0 + g2) * pow(abs(1.0 + g2 - 2.0 * g * mu), 1.5));
}

// Optical depth along a ray to the top of the atmosphere.
float2 OpticalDepth(float3 pos, float3 dir, int steps)
{
    float t = RaySphere(pos, dir, Ra).y;
    float dt = t / float(steps);
    float2 od = 0;
    float3 p = pos + dir * dt * 0.5;
    for (int i = 0; i < steps; ++i)
    {
        float h = length(p) - Re;
        od += exp(-h / float2(Hr, Hm)) * dt;
        p += dir * dt;
    }
    return od;
}

float3 MarchScattering(float3 rd, float3 lightDir, float intensity)
{
    float3 ro = float3(0, Re + 30.0, 0);
    float tMax = RaySphere(ro, rd, Ra).y;
    // If the ray points below the horizon, march only to the ground plane.
    float2 tg = RaySphere(ro, rd, Re);
    if (tg.x > 0.0 && tg.y > 0.0)
        tMax = min(tMax, tg.x);

    const int STEPS = 24;
    float dt = tMax / float(STEPS);
    float mu = dot(rd, lightDir);
    float phR = PhaseRayleigh(mu);
    float phM = PhaseMie(mu);

    float3 sumR = 0, sumM = 0;
    float2 odView = 0;
    float3 p = ro + rd * dt * 0.5;
    for (int i = 0; i < STEPS; ++i)
    {
        float h = max(length(p) - Re, 0.0);
        float2 dens = exp(-h / float2(Hr, Hm)) * dt;
        odView += dens;

        // Light reaching this sample (skip if the planet shadows it).
        float2 tls = RaySphere(p, lightDir, Re);
        if (!(tls.x > 0.0 && tls.y > 0.0))
        {
            float2 odLight = OpticalDepth(p, lightDir, 6);
            float3 tau = BetaR * (odView.x + odLight.x) + BetaM * 1.1 * (odView.y + odLight.y);
            float3 attn = exp(-tau);
            sumR += attn * dens.x;
            sumM += attn * dens.y;
        }
        p += rd * dt;
    }
    return (sumR * BetaR * phR + sumM * BetaM * phM) * intensity;
}

// Thin high-altitude cirrus, baked into the cube (cheap, static per preset).
float3 ApplyCirrus(float3 col, float3 rd, float3 lightDir, float intensity)
{
    if (gCloudCover <= 0.001 || rd.y <= 0.01)
        return col;
    float3 ro = float3(0, Re, 0);
    float t = RaySphere(ro, rd, Re + 7500.0).y;
    float2 uv = (ro + rd * t).xz * 0.00009;
    float cov = Fbm(uv * 2.0, 4);
    cov = smoothstep(1.0 - gCloudCover * 0.85, 1.25 - gCloudCover * 0.85, cov);
    cov *= smoothstep(0.01, 0.12, rd.y); // fade into horizon haze
    float sunH = saturate(lightDir.y * 2.5 + 0.1);
    float3 warm = lerp(float3(1.0, 0.45, 0.22), float3(1.0, 0.98, 0.95), sunH);
    float lit = 0.15 + 0.85 * pow(saturate(dot(rd, lightDir) * 0.5 + 0.5), 2.0);
    float3 cloudCol = warm * lit * intensity * 0.028 * (0.25 + saturate(lightDir.y + 0.35));
    return lerp(col, cloudCol, cov * 0.6);
}

float3 CubeDirFromThread(uint3 id, float res)
{
    float2 uv = (float2(id.xy) + 0.5) / res * 2.0 - 1.0;
    float u = uv.x, v = uv.y;
    switch (id.z)
    {
        case 0: return normalize(float3(1, -v, -u));   // +X
        case 1: return normalize(float3(-1, -v, u));   // -X
        case 2: return normalize(float3(u, 1, v));     // +Y
        case 3: return normalize(float3(u, -1, -v));   // -Y
        case 4: return normalize(float3(u, -v, 1));    // +Z
        default: return normalize(float3(-u, -v, -1)); // -Z
    }
}

[numthreads(8, 8, 1)]
void CSSkyGen(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gRes || id.y >= (uint)gRes)
        return;
    float3 rd = CubeDirFromThread(id, gRes);

    float3 col = 0;
    if (gSunI > 0.001)
        col += MarchScattering(rd, gSunDir, gSunI);
    if (gMoonI > 0.001)
        col += MarchScattering(rd, gMoonDir, gMoonI) * float3(0.75, 0.85, 1.1);
    // Faint airglow so night isn't pitch black.
    col += float3(0.00035, 0.0005, 0.0009) * (0.4 + 0.6 * saturate(1.0 - rd.y));

    float3 primary = gSunI >= gMoonI ? gSunDir : gMoonDir;
    float primaryI = max(gSunI, gMoonI);
    col = ApplyCirrus(col, rd, primary, primaryI);

    uCube[id] = float4(col, 1.0);
}

[numthreads(8, 8, 1)]
void CSSkyMip(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gRes || id.y >= (uint)gRes)
        return;
    float2 uv = (float2(id.xy) + 0.5) / gRes;
    // 4-tap box from the previous mip.
    float o = 0.25 / gRes;
    float3 c = 0;
    c += tPrevMip.SampleLevel(samLinearClamp, float3(uv + float2(-o, -o), id.z), 0).rgb;
    c += tPrevMip.SampleLevel(samLinearClamp, float3(uv + float2(o, -o), id.z), 0).rgb;
    c += tPrevMip.SampleLevel(samLinearClamp, float3(uv + float2(-o, o), id.z), 0).rgb;
    c += tPrevMip.SampleLevel(samLinearClamp, float3(uv + float2(o, o), id.z), 0).rgb;
    uCube[id] = float4(c * 0.25, 1.0);
}

// ---------------------------------------------------------------------------
// Skybox draw
// ---------------------------------------------------------------------------
TextureCube<float4> tSkyCube : register(t0);

struct SkyVSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

SkyVSOut VSSky(uint vid : SV_VertexID)
{
    SkyVSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0); // z=0: far plane (reversed-Z)
    o.uv = uv;
    return o;
}

float3 StarField(float3 rd)
{
    // Octahedral projection -> 2D grid of hashed star points.
    float3 a = abs(rd);
    float2 oct = rd.xz / (a.x + a.y + a.z);
    if (rd.y < 0.0)
        oct = (1.0 - abs(oct.yx)) * float2(oct.x >= 0 ? 1 : -1, oct.y >= 0 ? 1 : -1);
    float2 p = (oct * 0.5 + 0.5) * 700.0;
    float2 cell = floor(p);
    float2 f = frac(p);
    float h = Hash12(cell);
    float2 starPos = float2(Hash12(cell + 13.1), Hash12(cell + 27.7));
    float d = length(f - starPos);
    float brightness = smoothstep(0.10, 0.0, d) * pow(h, 14.0) * 3.5;
    float3 tint = lerp(float3(0.75, 0.85, 1.0), float3(1.0, 0.92, 0.8), Hash12(cell + 5.5));
    return brightness * tint;
}

float3 MoonDisc(float3 rd, float3 dir, float3 discCol)
{
    float cosAng = dot(rd, dir);
    float angR = 0.0046; // ~0.26 deg
    float cosR = 1.0 - angR * angR * 0.5;
    if (cosAng < cosR - 0.0004)
        return 0;
    // Local disc coordinates.
    float3 t1 = normalize(cross(dir, abs(dir.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 t2 = cross(dir, t1);
    float2 lc = float2(dot(rd, t1), dot(rd, t2)) / angR;
    float r2 = dot(lc, lc);
    float mask = smoothstep(1.0, 0.92, sqrt(r2));
    if (mask <= 0.0)
        return 0;
    float nz = sqrt(saturate(1.0 - r2));
    float limb = pow(nz, 0.35);
    float mare = 1.0 - 0.45 * smoothstep(0.45, 0.75, Fbm(lc * 2.6 + 4.7, 4));
    return discCol * limb * mare * mask;
}

float3 SunDisc(float3 rd, float3 dir, float3 discCol)
{
    float cosAng = dot(rd, dir);
    float angR = 0.00465;
    float cosR = 1.0 - angR * angR * 0.5;
    float t = smoothstep(cosR - 0.00012, cosR + 0.00006, cosAng);
    float limb = lerp(0.6, 1.0, saturate((cosAng - cosR) / (angR * angR)));
    return discCol * t * limb;
}

float4 PSSky(SkyVSOut i) : SV_Target
{
    float3 rd = ViewRayFromUv(i.uv);
    float3 col = tSkyCube.SampleLevel(samLinearClamp, rd, 0).rgb;

    if (rd.y > -0.05)
    {
        if (gLightIsMoon > 0.5)
            col += MoonDisc(rd, gLightDir, gSunDiscColor);
        else
            col += SunDisc(rd, gLightDir, gSunDiscColor);

        if (gStarIntensity > 0.001 && rd.y > 0.0)
        {
            float horizonFade = saturate(rd.y * 4.0);
            col += StarField(rd) * gStarIntensity * horizonFade;
        }
    }
    return float4(col, 1.0);
}
