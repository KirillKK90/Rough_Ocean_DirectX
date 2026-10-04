// Atmosphere: single-scattering Rayleigh+Mie raymarch (plus, when enabled, a
// multiple-scattering term from a small LUT, CSMsLut) baked into a small
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
    float gHaze;       // aerosol turbidity multiplier on BetaM (1 = clear)
    float gOzone;      // ozone column, 0 = off, 1 = standard
    float gCloudSunlit; // 1 = cirrus lit by the sunlight that reaches its altitude
    float gPadS2;
    float gAloft;      // haze layer aloft (~4 km): peak scattering at 550 nm, 1/m (0 = none)
    float gMultiScatter; // 0 = single scattering only, 1 = + multiple scattering (LUT)
    float2 gPadS3;
}

RWTexture2DArray<float4> uCube : register(u2);
RWTexture2D<float4> uMsLut : register(u3);
Texture2DArray<float4> tPrevMip : register(t2);
Texture2D<float4> tMsLut : register(t3);

static const float Re = 6371e3;         // planet radius
static const float Ra = 6431e3;         // atmosphere top
static const float3 BetaR = float3(5.8e-6, 13.5e-6, 33.1e-6);
static const float BetaM = 4.5e-6;
static const float Hr = 8500.0;
static const float Hm = 1200.0;
// Ozone absorbs in the Chappuis band (peak in the orange-green, none in the
// red). Negligible for a high sun, but a horizon ray crosses the layer for
// hundreds of km: it deepens the sunset reds and keeps the zenith blue.
// Absorption at peak density, tent profile 10-40 km (Bruneton 2017).
static const float3 BetaO = float3(0.650e-6, 1.881e-6, 0.085e-6);
// Haze layer aloft, above the marine boundary layer (a residual / advected
// aerosol layer, Gaussian about 4 km, Angstrom exponent 1.0). Thin in any
// vertical column, but the sunlight that reaches the air opposite a setting
// sun skims the planet *through* it, tangentially, for hundreds of km, while
// the beam to the observer only crosses it obliquely. So it deepens the
// Earth's shadow and mutes the anti-twilight arch far more than it dims the
// sun (a layer starting at the sea would mostly dim the sun).
// Keep in sync with ExtraTransmittance in App.cpp.
static const float kAloftZ = 4000.0, kAloftW = 1500.0;
static const float3 kAloftSpectrum = float3(0.81, 1.0, 1.25);

float AloftDensity(float h)
{
    float x = (h - kAloftZ) / kAloftW;
    return exp(-x * x);
}

float OzoneDensity(float h)
{
    return max(0.0, 1.0 - abs(h - 25000.0) / 15000.0);
}

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

// Optical depth along a ray to the top of the atmosphere: x Rayleigh, y Mie,
// z ozone, w free-tropospheric aerosol (in metres at the reference density).
float4 OpticalDepth(float3 pos, float3 dir, int steps)
{
    float t = RaySphere(pos, dir, Ra).y;
    float dt = t / float(steps);
    float4 od = 0;
    float3 p = pos + dir * dt * 0.5;
    for (int i = 0; i < steps; ++i)
    {
        float h = length(p) - Re;
        od.xy += exp(-h / float2(Hr, Hm)) * dt;
        od.z += OzoneDensity(h) * dt;
        od.w += AloftDensity(h) * dt;
        p += dir * dt;
    }
    return od;
}

// Aerosol beyond the clear baseline (gHaze > 1) is spectral, Angstrom exponent
// 1.3 (fine haze dims blue more than red): relative to 550 nm at 680/550/440.
// Keep in sync with ExtraTransmittance in App.cpp.
static const float3 kHazeSpectrum = float3(0.76, 1.0, 1.34);

float3 MieBeta()
{
    return BetaM * (1.0 + (gHaze - 1.0) * kHazeSpectrum);
}

float3 AloftBeta()
{
    return gAloft * kAloftSpectrum;
}

// Total extinction for an optical-depth quadruple (see OpticalDepth).
float3 Extinction(float4 od)
{
    return BetaR * od.x + MieBeta() * 1.1 * od.y + BetaO * gOzone * od.z + AloftBeta() * 1.1 * od.w;
}

// Scattering coefficient times step length, for per-step densities.
float3 ScatteringStep(float densR, float densM, float densF)
{
    return BetaR * densR + MieBeta() * densM + AloftBeta() * densF;
}

// ---------------------------------------------------------------------------
// Multiple scattering (Hillaire 2020, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique", sec. 5.5). Single scattering alone turns
// the whole horizon ring of a sunset orange: away from the sun the only light
// it knows is the reddened beam that skimmed the planet. Real twilight also
// carries light scattered more than once, mostly out of the sunlit, blue
// upper atmosphere, which fills the Earth's shadow with blue-grey and turns
// the anti-twilight arch pink. For a point at altitude h with the sun at
// zenith cosine mus, the second-order in-scatter L2 (isotropic phase) is
// averaged over the sphere and the infinite series of higher orders is
// folded in by 1 / (1 - f_ms), f_ms being the fraction re-scattered:
//   Psi_ms = L2 / (1 - f_ms)   per unit sun intensity,
// so the sky march adds sigma_s * Psi_ms along the view ray.
// ---------------------------------------------------------------------------
static const float kMsLutSize = 32.0;
static const float kMsTop = Ra - Re;

// LUT addressing: u = sun zenith cosine, v = sqrt(altitude) (finer near the
// sea, where the haze is). Texel centres sit exactly on the grid below.
float2 MsLutUv(float h, float mus)
{
    float2 g = float2(saturate(mus * 0.5 + 0.5), sqrt(saturate(h / kMsTop)));
    return (g * (kMsLutSize - 1.0) + 0.5) / kMsLutSize;
}

float3 MultiScatterPsi(float3 p, float3 lightDir)
{
    float r = length(p);
    return tMsLut.SampleLevel(samLinearClamp, MsLutUv(r - Re, dot(p / r, lightDir)), 0).rgb;
}

groupshared float3 gsMsL2[64];
groupshared float3 gsMsF[64];

// One group per LUT texel, one thread per direction of a 64-point Fibonacci
// sphere.
[numthreads(64, 1, 1)]
void CSMsLut(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    float mus = gid.x / (kMsLutSize - 1.0) * 2.0 - 1.0;
    float v = gid.y / (kMsLutSize - 1.0);
    float3 x = float3(0, Re + max(v * v * kMsTop, 5.0), 0);
    float3 L = float3(0, mus, sqrt(saturate(1.0 - mus * mus)));

    float fi = gi + 0.5;
    float cosT = 1.0 - 2.0 * fi / 64.0;
    float sinT = sqrt(saturate(1.0 - cosT * cosT));
    float phi = 2.39996323 * fi; // golden angle
    float3 w = float3(cos(phi) * sinT, cosT, sin(phi) * sinT);

    float tMax = RaySphere(x, w, Ra).y;
    float2 tg = RaySphere(x, w, Re);
    if (tg.x > 0.0 && tg.y > 0.0)
        tMax = min(tMax, tg.x);

    const int STEPS = 20;
    float dt = max(tMax, 0.0) / float(STEPS);
    float3 l2 = 0, f = 0;
    float4 od = 0;
    float3 p = x + w * dt * 0.5;
    for (int i = 0; i < STEPS; ++i)
    {
        float h = max(length(p) - Re, 0.0);
        float3 d = float3(exp(-h / float2(Hr, Hm)), AloftDensity(h)) * dt;
        float4 dOd = float4(d.xy, OzoneDensity(h) * dt, d.z);
        float3 tv = exp(-Extinction(od + 0.5 * dOd));
        od += dOd;
        float3 s = ScatteringStep(d.x, d.y, d.z);
        float2 tls = RaySphere(p, L, Re);
        float3 ts = (tls.x > 0.0 && tls.y > 0.0) ? 0.0 : exp(-Extinction(OpticalDepth(p, L, 8)));
        l2 += tv * s * ts;
        f += tv * s;
        p += w * dt;
    }
    gsMsL2[gi] = l2;
    gsMsF[gi] = f;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (gi < stride)
        {
            gsMsL2[gi] += gsMsL2[gi + stride];
            gsMsF[gi] += gsMsF[gi + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0)
    {
        // sphere averages; the isotropic phase 1/4pi applies to the 2nd order
        float3 L2 = gsMsL2[0] / 64.0 / (4.0 * PI);
        float3 fms = gsMsF[0] / 64.0;
        uMsLut[gid.xy] = float4(L2 / max(1.0 - fms, 1e-3), 1.0);
    }
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

    float3 sumR = 0, sumM = 0, sumF = 0, sumMs = 0;
    float4 odView = 0;
    float3 p = ro + rd * dt * 0.5;
    for (int i = 0; i < STEPS; ++i)
    {
        float h = max(length(p) - Re, 0.0);
        float2 dens = exp(-h / float2(Hr, Hm)) * dt;
        float densF = AloftDensity(h) * dt;
        float4 dOd = float4(dens, OzoneDensity(h) * dt, densF);
        odView += dOd;

        // Light reaching this sample (skip if the planet shadows it).
        float2 tls = RaySphere(p, lightDir, Re);
        if (!(tls.x > 0.0 && tls.y > 0.0))
        {
            float4 odLight = OpticalDepth(p, lightDir, 6);
            float3 attn = exp(-Extinction(odView + odLight));
            sumR += attn * dens.x;
            sumM += attn * dens.y;
            sumF += attn * densF;
        }
        // Multiply scattered light reaches the shadowed samples too. Attenuate
        // to the step's midpoint: a near-horizontal step spans ~36 km of the
        // densest air, and the blue that fills the Earth's shadow dies in it.
        [branch] if (gMultiScatter > 0.0)
            sumMs += exp(-Extinction(odView - 0.5 * dOd)) * ScatteringStep(dens.x, dens.y, densF)
                   * MultiScatterPsi(p, lightDir);
        p += rd * dt;
    }
    float3 L = (sumR * BetaR * phR + sumM * MieBeta() * phM) * intensity;
    // Zero unless the haze layer aloft / multiple scattering are on.
    return L + (sumF * AloftBeta() * phM + sumMs * gMultiScatter) * intensity;
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
    [branch] if (gCloudSunlit > 0.0)
    {
        // Light the ice with the sunlight that actually reaches it. At 7.5 km
        // a horizon sun still stands ~2.8 deg up, its beam reddened by the
        // grazing path below: golden cirrus toward the sun, deepening to red
        // where the sun sits on that cloud's own horizon.
        float3 pc = ro + rd * t;
        float2 sh = RaySphere(pc, lightDir, Re);
        float lit = (sh.x > 0.0 && sh.y > 0.0) ? 0.0 : 1.0; // earth shadow
        float3 T = exp(-Extinction(OpticalDepth(pc, lightDir, 8))) * lit;
        warm = lerp(warm, T * 1.4, gCloudSunlit);
    }
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
    // A star is a point source: through the zoom lens it spreads apart from
    // its neighbours but keeps its on-screen size.
    float brightness = smoothstep(0.10 * gZoom.y, 0.0, d) * pow(h, 14.0) * 3.5;
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

// Horizon sun (Sunset). Unlike the soft high-sun glow above, a sun this dim
// shows its true limb: a crisp, limb-darkened disc, squashed vertically by
// refraction (gSunFx.y) and graded by the steep air-mass change across it -
// yellow-orange at the upper limb, red at the waterline (gSunTauGrad). Thin
// turbulent layers over the sea shift each horizontal slice of it sideways
// and back, so the limb "boils"; the effect grows toward the horizon, where
// the line of sight is longest.
static const float kSunDiscR = 0.00855; // rad, = kSunDiscRadiusDeg (App.cpp)

float3 SunDiscLow(float3 rd, float3 dir, float3 discCol, float pixAng)
{
    if (dot(rd, dir) < 0.9995)
        return 0;
    float3 up = normalize(float3(0, 1, 0) - dir * dir.y);
    float3 right = cross(up, dir);
    // apparent offset from the disc centre, in disc radii (+y = up)
    float2 lc = float2(dot(rd, right), dot(rd, up)) / kSunDiscR;

    float sh = gSunFx.x;
    float t = gTime;
    float nearH = saturate(1.0 - lc.y); // 1 at the waterline, 0 at the top
    float layer = lc.y * 2.6 - t * 0.35; // layers creep upward with the warm air
    float wob = (ValueNoise(float2(layer, t * 1.6)) - 0.5)
              + 0.5 * (ValueNoise(float2(layer * 2.3 + 5.3, t * 2.9)) - 0.5);
    lc.x += sh * wob * (0.08 + 0.18 * nearH);
    lc.y += sh * 0.08 * (ValueNoise(float2(lc.x * 1.8 + 1.7, t * 1.2)) - 0.5);

    float r = length(float2(lc.x, lc.y / gSunFx.y)); // undo the refraction squash
    float aa = max(pixAng / kSunDiscR, 1e-3);
    float mask = saturate((1.0 - r) / aa + 0.5);
    float mu = sqrt(saturate(1.0 - r * r));
    float limb = 1.0 - 0.5 * (1.0 - mu); // limb darkening, u ~ 0.5
    return discCol * exp(gSunTauGrad.xyz * lc.y) * limb * mask;
}

float4 PSSky(SkyVSOut i) : SV_Target
{
    float3 rd = ViewRayFromUv(i.uv);
    float3 col = tSkyCube.SampleLevel(samLinearClamp, rd, 0).rgb;
    float pixAng = max(length(ddx(rd)), length(ddy(rd))); // radians per pixel

    if (rd.y > -0.05)
    {
        if (gLightIsMoon > 0.5)
            col += MoonDisc(rd, gLightDir, gSunDiscColor);
        else if (gSunFx.x > 0.0)
            col += SunDiscLow(rd, gLightDir, gSunDiscColor, pixAng);
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
