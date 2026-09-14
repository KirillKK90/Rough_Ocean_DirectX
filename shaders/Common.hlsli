// Shared declarations for all graphics shaders.

SamplerState samLinearWrap  : register(s0);
SamplerState samLinearClamp : register(s1);
SamplerState samPoint       : register(s2);
SamplerState samAnisoWrap   : register(s3); // ocean maps at grazing angles

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
    float4 gCascade1;       // w = FFT map resolution, texels (same for all cascades)
    float4 gCascade2;
    float3 gWaterDeep;      float gFoamAmount;
    float3 gWaterScatter;   float gSSS;
    float3 gBuoyLightPos;   float gBuoyLightOn;   // camera-relative position
    float3 gBuoyLightColor; float gFogDensity;
    float2 gWindDir;        float gDistRough;     float gGridScale; // radial mesh vertex spacing per meter of distance
    float4 gImpacts[4];     // xy = world XZ, z = seconds since impact (<0 off), w = amplitude
    // Up to WHIRL_MAX concurrent whirlpools (keep the 4s in sync).
    float4 gWhirl[4];       // xy = world XZ, z = seconds since spawn (<0 off), w = peak funnel depth m
    float4 gWhirl2[4];      // x = spin-up s, y = decay s, z = peak core radius m, w = swirl gain (signed: <0 = clockwise)
    float4 gWhirl3[4];      // x = draw-in, y = reach m, z = cull radius m, w = pattern dissolve s
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
// The circulation ramps linearly while the "plug is pulled" (the spin-up
// time), then decays exponentially while the core spreads diffusively, so the
// funnel deepens and widens, then swiftly relaxes away.
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
//
// Several whirlpools can be live at once. Each one's particle map is a
// diffeomorphism, so they compose: the sample position is passed through each
// vortex in turn and the Jacobians multiply by the chain rule (water wound by
// one vortex and then pulled by the next). The surface fields - funnel,
// spiral arms, rebound rings, foam - superpose linearly on top.
//
// Everything that shapes a vortex is a per-event runtime parameter (UI
// sliders -> gWhirl/gWhirl2/gWhirl3), captured when it spawns:
//   gWhirl.w   peak funnel depth D at full spin-up, metres
//   gWhirl2.x  spin-up time: how long the "plug" keeps pulling
//   gWhirl2.y  decay time of the circulation once the forcing stops
//   gWhirl2.z  peak core radius rc (the throat's width)
//   gWhirl2.w  swirl gain: how far the surrounding sea is wound; sign = spin
//   gWhirl3.x  draw-in: how strongly the surroundings converge on the drain
//   gWhirl3.y  reach: distance scale over which the vortex disturbs the sea
// ---------------------------------------------------------------------------
static const int WHIRL_MAX = 4;              // must match the gWhirl* array sizes
static const float WHIRL_G = 9.81;
static const float WHIRL_BETA = 1.2564312;   // Lamb-Oseen peak-velocity constant
static const float WHIRL_WIND_KNEE = 12.566; // 2 turns: winding is exact below this
static const float WHIRL_WIND_MAX = 37.699;  // 6 turns: saturated core plateau
static const float WHIRL_WIND_PER_M = 6.6;   // resolvable winding per metre of core
static const float WHIRL_STRETCH_KNEE = 12.0; // map stretch the cascades can still carry
static const float WHIRL_SINK_MAX = 2.5;     // largest draw-in radius, in core radii
static const float WHIRL_ARM_K = 14.8;       // spiral-arm wavenumber * core radius
static const float WHIRL_ARM_FOAM = 68.4;    // arm-streak foam threshold * core radius
static const float WHIRL_HOLE_R = 0.12;      // drain mouth radius, in core radii

// Smooth saturating limiter: exactly the identity for |x| <= knee, asymptotes
// to +-lim beyond it. Also returns dy/dx so warped gradients stay exact.
void WhirlSoftLimit(float x, float knee, float lim, out float y, out float dydx)
{
    float ax = abs(x);
    [branch] if (ax <= knee)
    {
        y = x;
        dydx = 1.0;
        return;
    }
    float s = max(lim - knee, 1e-3);
    float th = tanh((ax - knee) / s);
    y = sign(x) * (knee + s * th);
    dydx = 1.0 - th * th;
}

// Time envelope shared by the warp and the wave field. Outputs the current
// core radius, funnel depth, wound-angle integral S = int Gamma dt (always
// >= 0; the spin direction rides on the sign of the gain), and the
// warped-vs-plain sea blend weight (1 = fully wound, -> 0 as the pattern
// dissolves after the vortex dies).
void WhirlEnvelope(int i, float age, out float rc, out float D, out float S, out float wBlend)
{
    float depthPeak = gWhirl[i].w;
    float grow = gWhirl2[i].x, tau = gWhirl2[i].y, rcPeak = gWhirl2[i].z;
    float gmax = 2.0 * PI * rcPeak * sqrt(2.0 * WHIRL_G * depthPeak);
    float gam;
    if (age <= grow)
    {
        float u = age / grow;
        gam = gmax * u;                    // linear torque ramp
        S = 0.5 * gmax * grow * u * u;
        rc = rcPeak * (0.35 + 0.65 * u);
        wBlend = 1.0;
    }
    else
    {
        float d = age - grow;
        float e = exp(-d / tau);
        gam = gmax * e;
        S = gmax * (0.5 * grow + tau * (1.0 - e));
        rc = rcPeak * (1.0 + 0.5 * (1.0 - exp(-d / (1.6 * tau)))); // diffusive spread
        wBlend = exp(-d / gWhirl3[i].w);
    }
    D = gam * gam / (8.0 * PI * PI * WHIRL_G * rc * rc);
}

// Winding angle Theta(r) and dTheta/dr, smoothly faded to exactly zero in the
// far field so the warp never leaves a seam. Theta saturates near the core:
// a Lamb-Oseen core rotates as a solid body, so there is no shear there to
// wind a pattern up - which is also what keeps strong settings from grinding
// the (mip-less) cascade textures into aliased noise.
void WhirlWinding(int i, float r, float rc, float S, out float Th, out float Thp)
{
    float gain = gWhirl2[i].w;   // signed: negative spins the other way
    float fadeR = gWhirl3[i].y;
    float eta = r * r / (rc * rc);
    float ebn = exp(-WHIRL_BETA * eta);
    float f = exp(-(r * r) / (fadeR * fadeR));
    float fp = -2.0 * r / (fadeR * fadeR) * f;
    float A = gain * S / (2.0 * PI);
    float w0 = (1.0 - ebn) / (r * r);
    float w0p = (2.0 / (r * r * r)) * (WHIRL_BETA * eta * ebn - (1.0 - ebn));
    float raw = A * w0 * f;
    float rawp = A * (w0p * f + w0 * fp);
    // A narrow throat has no room to hold many spiral turns, so cap the
    // plateau by the core size as well (6 turns for a wide maelstrom, 2 for a
    // tight drain). Defaults sit below the knee, so they pass through exactly.
    float lim = clamp(WHIRL_WIND_PER_M * rc, WHIRL_WIND_KNEE + 0.1, WHIRL_WIND_MAX);
    float knee = min(WHIRL_WIND_KNEE, 0.7 * lim);
    float dydx;
    WhirlSoftLimit(raw, knee, lim, Th, dydx);
    Thp = dydx * rawp;
}

// Detail attenuation for the warped sea. Where the vortex map stretches a
// step far enough that the fine ripples would be sampled below their own
// texel rate, they are faded out rather than aliased into terracing - which
// is also what turbulence does to ripples wound past the point of
// recognition. (The cascade maps carry mip chains now, but the vertex path
// samples explicit levels, and the fade doubles as the physical churn look.)
// The knee sits above the stretch a default whirlpool reaches, so it only
// engages on strong settings.
float3 WhirlDetailFades(float3 fades, float4 J)
{
    float stretch = max(length(J.xy), length(J.zw));
    float det = saturate(WHIRL_STRETCH_KNEE / max(stretch, 1e-3));
    return float3(fades.x, fades.y * lerp(1.0, det, 0.6), fades.z * det);
}

// One vortex's particle-map warp of the ambient-sea sampling. Returns the
// sample position, the Jacobian J = d(samp)/d(p) packed as (J00,J01,J10,J11),
// the pattern rotation angle, and this vortex's warped-vs-plain blend weight.
// False means it contributes nothing here (inactive, or out of range).
bool WhirlWarpOne(int i, float2 p, out float2 samp, out float4 J, out float th,
                  out float wBlend)
{
    samp = p;
    J = float4(1, 0, 0, 1);
    th = 0.0;
    wBlend = 0.0;

    float age = gWhirl[i].z;
    if (age <= 0.0 || gWhirl[i].w <= 0.0)
        return false;
    float2 d = p - gWhirl[i].xy;
    float r = max(length(d), 1e-3);
    if (r > gWhirl3[i].z)
        return false;

    float rc, D, S, wb;
    WhirlEnvelope(i, age, rc, D, S, wb);
    wBlend = wb;

    float Th, Thp;
    WhirlWinding(i, r, rc, S, Th, Thp);
    th = Th;

    // Sink pull rho(r) and drho/dr (softened at the core: the water that
    // converges past the throat plunges down the drain instead of piling up).
    // The drawn-in area saturates at (WHIRL_SINK_MAX * rc)^2 - a drain of a
    // given throat width can only swallow so much - which also bounds how far
    // the map compresses. Q is spatially constant, so this costs no chain rule.
    float fadeR = gWhirl3[i].y;
    float rc2 = rc * rc;
    float eta = r * r / rc2;
    float en = exp(-eta);
    float f = exp(-(r * r) / (fadeR * fadeR));
    float fp = -2.0 * r / (fadeR * fadeR) * f;
    float Q = gWhirl3[i].x * S;
    float Qmax = (WHIRL_SINK_MAX * rc) * (WHIRL_SINK_MAX * rc);
    Q = Qmax * Q / (Q + Qmax);
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

    samp = gWhirl[i].xy + rho * Rmu;

    // J = a u^T + (rho/r) Rm (I - u u^T), a = d(samp)/dr.
    float2 a = rhop * Rmu + rho * Thp * RmGu;
    float k = rho / r;
    J = float4(a.x * u.x + k * ( c - Rmu.x * u.x),
               a.x * u.y + k * ( s - Rmu.x * u.y),
               a.y * u.x + k * (-s - Rmu.y * u.x),
               a.y * u.y + k * ( c - Rmu.y * u.y));
    return true;
}

// Composed warp of every live vortex. Each map is applied to the previous
// one's output and the Jacobians multiply (chain rule), so water wound by one
// whirlpool is then carried by the next. rotCS = (cos,sin) of the total
// pattern rotation, for horizontal-displacement vectors; wBlend is the
// warped-vs-plain weight (0 = no vortex here: sample the sea as usual).
// A vortex fully dissolves back to plain sea only where no live one overlaps
// it - which the per-vortex cull radius already decides.
void WhirlWarp(float2 worldXZ, out float2 samp, out float4 J, out float2 rotCS,
               out float wBlend)
{
    samp = worldXZ;
    J = float4(1, 0, 0, 1);
    float thTotal = 0.0;
    float plainWeight = 1.0;

    for (int i = 0; i < WHIRL_MAX; ++i)
    {
        float2 p;
        float4 Ji;
        float th, wb;
        [branch] if (!WhirlWarpOne(i, samp, p, Ji, th, wb))
            continue;
        samp = p;
        // J = Ji * J (2x2, row-major in xyzw)
        J = float4(Ji.x * J.x + Ji.y * J.z, Ji.x * J.y + Ji.y * J.w,
                   Ji.z * J.x + Ji.w * J.z, Ji.z * J.y + Ji.w * J.w);
        thTotal += th;
        plainWeight *= 1.0 - wb;
    }

    float s, c;
    sincos(thTotal, s, c);
    rotCS = float2(c, s); // world vector = R(+total) * sampled vector
    wBlend = 1.0 - plainWeight;
}

// One whirlpool's own surface: funnel depression, wound spiral ripple arms,
// churned foam collar, and the ring packet radiated by the collapse rebound.
void WhirlWavesOne(int i, float2 worldXZ, inout float3 disp, inout float2 slope,
                   inout float foam)
{
    float age = gWhirl[i].z;
    if (age <= 0.0 || gWhirl[i].w <= 0.0)
        return;
    float2 dv = worldXZ - gWhirl[i].xy;
    float r = max(length(dv), 1e-3);
    if (r > gWhirl3[i].z)
        return;
    float2 u = dv / r;

    float rc, D, S, wb;
    WhirlEnvelope(i, age, rc, D, S, wb);
    float rc2 = rc * rc;
    float eta = r * r / rc2;

    // --- Funnel: cyclostrophic free-surface depression ---
    float inv = 1.0 / (1.0 + eta);
    float ddr = D * (2.0 * r / rc2) * inv * inv; // wall slope d(-h)/dr, > 0
    disp.y += -D * inv;
    // Slight inward pull sharpens the throat. Its radial gradient at the
    // centre is pull * 2D/rc, so a steep funnel would bunch (and past 1,
    // fold) the ocean grid onto itself - visible as terraced rings. Bound it
    // to a fixed compression; the default sits under the cap untouched.
    float pull = min(0.35, 0.3 * rc / max(D, 1e-3));
    disp.xz += -u * ddr * rc * pull;
    slope += u * ddr;

    // Churned white collar where the wall is steep, thinning toward the
    // throat so the drain keeps a glassy dark eye.
    float collar = saturate((ddr - 0.28) * 2.8);
    collar *= 1.0 - 0.85 * exp(-eta / 0.10);
    foam += collar * 0.85;

    // --- Spiral ripple arms: short waves wound by the differential rotation,
    // sharing Theta with the warp so they stay in phase with the dragged sea.
    float Th, Thp;
    WhirlWinding(i, r, rc, S, Th, Thp);
    // Arm wavelength and height scale with the core, so a wide maelstrom gets
    // long sweeping arms rather than the same centimetre ripples as a small one.
    float theta = atan2(dv.y, dv.x);
    float karm = WHIRL_ARM_K / rc;
    float armBand = (r - 1.45 * rc) / (2.4 * rc);
    float armEnv = exp(-armBand * armBand) * (1.0 - exp(-1.5 * eta));
    float aA = min(0.03 * D, 0.012 * rc) * armEnv;
    [branch] if (aA > 1e-4)
    {
        float phi = 3.0 * (theta - Th) + karm * r;
        float sph, cph;
        sincos(phi, sph, cph);
        float2 that = float2(-u.y, u.x);
        float2 gphi = (karm - 3.0 * Thp) * u + (3.0 / r) * that;
        disp.y += aA * cph;
        slope += (-aA * sph) * gphi;
        foam += saturate(aA * (WHIRL_ARM_FOAM / rc) - 0.25) * (0.5 + 0.5 * cph);
    }

    // --- Collapse rebound: when the forcing stops, the recovering dimple
    // radiates a gentle dispersive ring packet (Cauchy-Poisson, like a small
    // inverted impact).
    float dAge = age - gWhirl2[i].x;
    [branch] if (dAge > 0.05)
    {
        float rr = max(r, 0.6 * rc);
        float kloc = WHIRL_G * dAge * dAge / (4.0 * rr * rr);
        float phase = -WHIRL_G * dAge * dAge / (4.0 * rr);
        float k0 = 2.0 * PI / (2.5 * rc);
        float lx = log(max(kloc / k0, 1e-6));
        float envr = exp(-lx * lx * 1.4);
        float Ar = 0.32 * gWhirl[i].w * pow(rc / (rc + r), 0.8) * exp(-dAge / 4.5) * envr;
        float sr, cr;
        sincos(phase, sr, cr);
        disp.y += Ar * cr;
        float detadr = -Ar * kloc * sr;
        slope += u * detadr;
        foam += saturate((abs(detadr) - 0.06) * 5.0) * saturate(1.2 - r / (18.0 * rc));
    }
}

// The drain mouth: an absolutely black disc at the throat, opening as the
// vortex pulls the funnel down and closing again as it collapses. Its size
// follows the funnel actually achieved (D relative to the peak the settings
// ask for), so a stronger whirlpool tears a wider hole. Returned as a
// darkening weight applied to the final shaded colour, after fog, so the
// core stays absolutely black at any distance - the water is pouring into
// something below the surface, not onto a dark patch painted on it.
float WhirlDrainDarkness(float2 worldXZ)
{
    float dark = 0.0;
    for (int i = 0; i < WHIRL_MAX; ++i)
    {
        float age = gWhirl[i].z;
        [branch] if (age <= 0.0 || gWhirl[i].w <= 0.0)
            continue;
        // Conservative reject before any envelope work: the core can only
        // spread to 1.5x its peak radius and `open` never exceeds 1, so this
        // bounds the lit region without changing a pixel of the result.
        float2 d = worldXZ - gWhirl[i].xy;
        float r = length(d);
        if (r > WHIRL_HOLE_R * 1.5 * gWhirl2[i].z * 2.8)
            continue;

        float rc, D, S, wb;
        WhirlEnvelope(i, age, rc, D, S, wb);
        float open = saturate(D / max(gWhirl[i].w, 1e-3)); // 0 at spawn -> 1 at full spin-up
        float holeR = WHIRL_HOLE_R * rc * open;
        [branch] if (holeR < 1e-3 || r > holeR * 2.8)
            continue;

        float core = 1.0 - smoothstep(holeR * 0.75, holeR * 1.05, r); // pure black
        float shaft = 1.0 - smoothstep(holeR, holeR * 2.8, r);        // its shadowed lip
        dark = max(dark, max(core, 0.7 * shaft));
    }
    return saturate(dark);
}

// Every live whirlpool's surface, superposed. Surface elevations add
// linearly, so overlapping vortices simply sum - two drains side by side
// share one churned trough between them.
void WhirlWaves(float2 worldXZ, out float3 disp, out float2 slope, out float foam)
{
    disp = 0;
    slope = 0;
    foam = 0;
    for (int i = 0; i < WHIRL_MAX; ++i)
        WhirlWavesOne(i, worldXZ, disp, slope, foam);
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
