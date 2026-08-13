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
    float4 gImpacts[4];     // xy = world XZ, z = seconds since impact (<0 off), w = amplitude
    float4 gWhirl;          // xy = world XZ, z = seconds since spawn (<0 off), w = peak funnel depth m
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

// ---------------------------------------------------------------------------
// Meteorite impact waves: Cauchy-Poisson dispersive rings on deep water.
// An impulsive point disturbance radiates a ring packet in which, at radius r
// and time t, the locally dominant wavenumber is k = g t^2 / (4 r^2) (the
// stationary-phase solution of the deep-water dispersion relation), with
// phase -g t^2 / (4 r). Long waves lead, short ripples trail, amplitude falls
// with cylindrical spreading and decays in time — and it superimposes
// linearly on the FFT sea. Keep in sync with Meteor::HeightAt (CPU).
// ---------------------------------------------------------------------------
static const float IMP_G = 9.81;
static const float IMP_K0 = 0.09;      // ring packet dominant wavenumber (~70 m)
static const float IMP_BAND = 1.65;    // 1/(2 sigma^2) of the log-space band
static const float IMP_R0 = 45.0;      // spreading falloff radius
static const float IMP_TAU = 45.0;     // temporal decay, seconds
static const float IMP_SPLASH_W = 14.0;
static const float IMP_BORE_C = 40.0;  // leading bore speed, m/s (tsunami-like)

void ImpactWaves(float2 worldXZ, out float3 disp, out float2 slope, out float foam)
{
    disp = 0;
    slope = 0;
    foam = 0;
    [unroll]
    for (int ii = 0; ii < 4; ++ii)
    {
        float t = gImpacts[ii].z;
        float a0 = gImpacts[ii].w;
        if (t <= 0.0 || a0 <= 0.0)
            continue;
        float2 d = worldXZ - gImpacts[ii].xy;
        float r = max(length(d), 2.0);
        float2 rhat = d / r;

        float kloc = IMP_G * t * t / (4.0 * r * r);
        float phase = -IMP_G * t * t / (4.0 * r);
        float lx = log(kloc / IMP_K0);
        float env = exp(-lx * lx * IMP_BAND);
        float A = a0 * pow(IMP_R0 / (IMP_R0 + r), 0.75) * exp(-t / IMP_TAU) * env;

        float s, c;
        sincos(phase, s, c);

        // Central splash: crater and rebound during the first seconds.
        float sedge = exp(-r * r / (IMP_SPLASH_W * IMP_SPLASH_W));
        float spulse = a0 * 1.8 * cos(2.2 * t) * exp(-t / 2.8);

        disp.y += A * c - spulse * sedge;
        disp.xz += rhat * (-A * s) * 1.4; // Gerstner-style crest sharpening

        float detadr = -A * kloc * s;
        float dsplashdr = spulse * sedge * (2.0 * r / (IMP_SPLASH_W * IMP_SPLASH_W));
        slope += rhat * (detadr + dsplashdr);
        // Whitecapped crests where the rings are steep, fading with radius.
        foam += saturate((abs(detadr) - 0.05) * 6.0) * saturate(1.3 - r / 260.0);

        // Leading bore: the impact dumps enormous energy into a fast,
        // long-wavelength solitary crest (with a trailing drawdown) that
        // races ahead of the dispersive ring packet at tsunami-like speed.
        float rb = IMP_BORE_C * t;
        float wb = 16.0 + 0.05 * rb; // front widens as it spreads
        float xb = (r - rb) / wb;
        float g1 = exp(-xb * xb);
        float g2 = exp(-(xb + 1.6) * (xb + 1.6));
        float Ab = a0 * 2.2 * (80.0 / (80.0 + rb)) * exp(-t / 70.0);
        disp.y += Ab * (g1 - 0.4 * g2);
        disp.xz += rhat * Ab * g1 * 0.7;
        slope += rhat * (Ab * (-2.0 * xb * g1 + 0.8 * (xb + 1.6) * g2) / wb);
        foam += g1 * saturate(Ab * 0.5) * 0.9; // churning white front while tall
    }
}

// ---------------------------------------------------------------------------
// Whirlpool (left-click event): a bathtub / maelstrom vortex.
//
// Model: a Lamb-Oseen vortex over a softened line sink. The azimuthal flow
//   v_theta(r) = Gamma/(2 pi r) * (1 - exp(-beta r^2/rc^2))
// is in cyclostrophic balance with the free surface, g dh/dr = v^2/r, whose
// integrated depression is closely approximated by the hyperbolic funnel
//   h(r) = -D / (1 + r^2/rc^2),   D = Gamma^2 / (8 pi^2 g rc^2)
// (the exact 1/r^2 far-field of the Lamb-Oseen dip, parabolic in the core).
// The circulation ramps linearly while the "plug is pulled" (WHIRL_GROW
// seconds), then decays exponentially while the core spreads diffusively, so
// the funnel deepens and widens for 5 s and then swiftly relaxes away.
//
// The surrounding sea is drawn in by warping the FFT-cascade sampling with
// the vortex's own particle map: features wind by the integrated rotation
//   Theta(r,t) = gain * S(t) / (2 pi r^2) * (1 - exp(-beta r^2/rc^2)),
// with S = integral of Gamma dt (closed form), and converge along the sink
//   rho(r) = sqrt(r^2 + Q(t) * (1 - exp(-r^2/rc^2))),
// so the ambient ripples themselves spiral into the drain. Sampled slopes
// are pulled back through the exact warp Jacobian. When the vortex dies the
// wound pattern crossfades back to the undisturbed sea (dispersive mixing)
// and the rebounding dimple radiates a small Cauchy-Poisson ring packet.
// Keep the height terms in sync with Whirlpool.cpp (CPU mirror for buoy).
// ---------------------------------------------------------------------------
static const float WHIRL_G = 9.81;
static const float WHIRL_GROW = 5.0;       // forcing duration, seconds
static const float WHIRL_TAU = 1.0;        // circulation decay after release, s
static const float WHIRL_BETA = 1.2564312; // Lamb-Oseen peak-velocity constant
static const float WHIRL_GAIN = 2.2;       // visible-winding multiplier
static const float WHIRL_SINK = 0.10;      // drawn-in area per circulation-time
static const float WHIRL_FADE_R = 140.0;   // smooth spatial falloff of the warp
static const float WHIRL_DISSOLVE = 2.2;   // wound-pattern crossfade after death, s
static const float WHIRL_SKIP_R = 300.0;   // beyond this the vortex is exactly zero
static const float WHIRL_MAX_AGE = 17.0;

// Time envelope shared by the warp and the wave field. Outputs the current
// core radius, funnel depth, wound-angle integral S = int Gamma dt, and the
// warped-vs-plain sea blend weight (1 = fully wound, -> 0 as the pattern
// dissolves after the vortex dies).
void WhirlEnvelope(float age, float depthPeak,
                   out float rc, out float D, out float S, out float wBlend)
{
    float rcPeak = 3.0 + 0.6 * depthPeak;  // deeper whirl is also broader
    float gmax = 2.0 * PI * rcPeak * sqrt(2.0 * WHIRL_G * depthPeak);
    float gam;
    if (age <= WHIRL_GROW)
    {
        float u = age / WHIRL_GROW;
        gam = gmax * u;                        // linear torque ramp
        S = 0.5 * gmax * WHIRL_GROW * u * u;
        rc = rcPeak * (0.35 + 0.65 * u);
        wBlend = 1.0;
    }
    else
    {
        float d = age - WHIRL_GROW;
        float e = exp(-d / WHIRL_TAU);
        gam = gmax * e;
        S = gmax * (0.5 * WHIRL_GROW + WHIRL_TAU * (1.0 - e));
        rc = rcPeak * (1.0 + 0.5 * (1.0 - exp(-d / 1.6))); // diffusive spread
        wBlend = exp(-d / WHIRL_DISSOLVE);
    }
    D = gam * gam / (8.0 * PI * PI * WHIRL_G * rc * rc);
}

// Winding angle Theta(r) and dTheta/dr, smoothly faded to exactly zero in the
// far field so the warp never leaves a seam.
void WhirlWinding(float r, float rc, float S, out float Th, out float Thp)
{
    float eta = r * r / (rc * rc);
    float ebn = exp(-WHIRL_BETA * eta);
    float f = exp(-(r * r) / (WHIRL_FADE_R * WHIRL_FADE_R));
    float fp = -2.0 * r / (WHIRL_FADE_R * WHIRL_FADE_R) * f;
    float A = WHIRL_GAIN * S / (2.0 * PI);
    float w0 = (1.0 - ebn) / (r * r);
    float w0p = (2.0 / (r * r * r)) * (WHIRL_BETA * eta * ebn - (1.0 - ebn));
    Th = A * w0 * f;
    Thp = A * (w0p * f + w0 * fp);
}

// Vortex particle-map warp of the ambient-sea sampling. Returns the sample
// position, the Jacobian J = d(samp)/d(world) packed as (J00,J01,J10,J11)
// for gradient pull-back (slope_world = J^T slope_sampled), rotCS = (cos,sin)
// of the local pattern rotation for horizontal-displacement vectors, and the
// warped-vs-plain blend weight (0 = vortex off: sample the sea as usual).
void WhirlWarp(float2 worldXZ, out float2 samp, out float4 J, out float2 rotCS,
               out float wBlend)
{
    samp = worldXZ;
    J = float4(1, 0, 0, 1);
    rotCS = float2(1, 0);
    wBlend = 0.0;

    float age = gWhirl.z;
    if (age <= 0.0 || gWhirl.w <= 0.0)
        return;
    float2 d = worldXZ - gWhirl.xy;
    float r = max(length(d), 1e-3);
    if (r > WHIRL_SKIP_R)
        return;

    float rc, D, S, wb;
    WhirlEnvelope(age, gWhirl.w, rc, D, S, wb);
    wBlend = wb;

    float Th, Thp;
    WhirlWinding(r, rc, S, Th, Thp);

    // Sink pull rho(r) and drho/dr (softened at the core: the water that
    // converges past the throat plunges down the drain instead of piling up).
    float rc2 = rc * rc;
    float eta = r * r / rc2;
    float en = exp(-eta);
    float f = exp(-(r * r) / (WHIRL_FADE_R * WHIRL_FADE_R));
    float fp = -2.0 * r / (WHIRL_FADE_R * WHIRL_FADE_R) * f;
    float Q = WHIRL_SINK * S;
    float Qe = Q * (1.0 - en) * f;
    float Qep = Q * ((2.0 * r / rc2) * en * f + (1.0 - en) * fp);
    float rho = sqrt(r * r + Qe);
    float rhop = (r + 0.5 * Qep) / rho;

    float s, c;
    sincos(Th, s, c);
    float2 u = d / r;
    float2 Rmu = float2(c * u.x + s * u.y, -s * u.x + c * u.y);   // R(-Th) u
    float2 Gu = float2(u.y, -u.x);
    float2 RmGu = float2(c * Gu.x + s * Gu.y, -s * Gu.x + c * Gu.y);

    samp = gWhirl.xy + rho * Rmu;

    // J = a u^T + (rho/r) Rm (I - u u^T), a = d(samp)/dr.
    float2 a = rhop * Rmu + rho * Thp * RmGu;
    float k = rho / r;
    J = float4(a.x * u.x + k * ( c - Rmu.x * u.x),
               a.x * u.y + k * ( s - Rmu.x * u.y),
               a.y * u.x + k * (-s - Rmu.y * u.x),
               a.y * u.y + k * ( c - Rmu.y * u.y));
    rotCS = float2(c, s); // world vector = R(+Th) * sampled vector
}

// The whirlpool's own surface: funnel depression, wound spiral ripple arms,
// churned foam collar, and the ring packet radiated by the collapse rebound.
void WhirlWaves(float2 worldXZ, out float3 disp, out float2 slope, out float foam)
{
    disp = 0;
    slope = 0;
    foam = 0;
    float age = gWhirl.z;
    if (age <= 0.0 || gWhirl.w <= 0.0)
        return;
    float2 dv = worldXZ - gWhirl.xy;
    float r = max(length(dv), 1e-3);
    if (r > WHIRL_SKIP_R)
        return;
    float2 u = dv / r;

    float rc, D, S, wb;
    WhirlEnvelope(age, gWhirl.w, rc, D, S, wb);
    float rc2 = rc * rc;
    float eta = r * r / rc2;

    // --- Funnel: cyclostrophic free-surface depression ---
    float inv = 1.0 / (1.0 + eta);
    float ddr = D * (2.0 * r / rc2) * inv * inv; // wall slope d(-h)/dr, > 0
    disp.y += -D * inv;
    disp.xz += -u * ddr * rc * 0.35; // slight inward pull sharpens the throat
    slope += u * ddr;

    // Churned white collar where the wall is steep, thinning toward the
    // throat so the drain keeps a glassy dark eye.
    float collar = saturate((ddr - 0.28) * 2.8);
    collar *= 1.0 - 0.85 * exp(-eta / 0.10);
    foam += collar * 0.85;

    // --- Spiral ripple arms: short waves wound by the differential rotation,
    // sharing Theta with the warp so they stay in phase with the dragged sea.
    float Th, Thp;
    WhirlWinding(r, rc, S, Th, Thp);
    float theta = atan2(dv.y, dv.x);
    float armBand = (r - 1.45 * rc) / (2.4 * rc);
    float armEnv = exp(-armBand * armBand) * (1.0 - exp(-1.5 * eta));
    float aA = min(0.03 * D, 0.07) * armEnv;
    [branch] if (aA > 1e-4)
    {
        float phi = 3.0 * (theta - Th) + 2.6 * r;
        float sph, cph;
        sincos(phi, sph, cph);
        float2 that = float2(-u.y, u.x);
        float2 gphi = (2.6 - 3.0 * Thp) * u + (3.0 / r) * that;
        disp.y += aA * cph;
        slope += (-aA * sph) * gphi;
        foam += saturate(aA * 12.0 - 0.25) * (0.5 + 0.5 * cph);
    }

    // --- Collapse rebound: when the forcing stops, the recovering dimple
    // radiates a gentle dispersive ring packet (Cauchy-Poisson, like a small
    // inverted impact).
    float dAge = age - WHIRL_GROW;
    [branch] if (dAge > 0.05)
    {
        float rr = max(r, 0.6 * rc);
        float kloc = WHIRL_G * dAge * dAge / (4.0 * rr * rr);
        float phase = -WHIRL_G * dAge * dAge / (4.0 * rr);
        float k0 = 2.0 * PI / (2.5 * rc);
        float lx = log(max(kloc / k0, 1e-6));
        float envr = exp(-lx * lx * 1.4);
        float Ar = 0.32 * gWhirl.w * pow(rc / (rc + r), 0.8) * exp(-dAge / 4.5) * envr;
        float sr, cr;
        sincos(phase, sr, cr);
        disp.y += Ar * cr;
        float detadr = -Ar * kloc * sr;
        slope += u * detadr;
        foam += saturate((abs(detadr) - 0.06) * 5.0) * saturate(1.2 - r / (18.0 * rc));
    }
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
