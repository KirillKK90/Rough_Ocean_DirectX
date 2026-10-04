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
    float4 gSunFx;          // x = horizon-sun shimmer 0..1 (0 = off), y = refraction flattening of the disc
    float4 gSunTauGrad;     // xyz = optical-depth change across the disc, per radius upward
    float4 gZoom;           // x = optical zoom, y = 1/zoom: scales distances used as a pixel-footprint proxy;
                            // zw = sin, cos of the view yaw
    // Zoom grid (Ocean.hlsl ZoomGridOffset, fitted by Ocean::FitZoomGrid):
    float4 gZoomFan;        // x = mesh sectors (0 = plain mesh), y = front sectors, z = first front angle - yaw, w = its step
    float4 gZoomRings;      // x = ln r0, y = 1 / plain ring log-step, z = first window ring radius m, w = window log-step
    float4 gZoomSkirt;      // x = mesh rings, y = skirt rings at each end of the window, z = r0 m, w = sea radius m
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
// The circulation ramps as a smoothstep while the "plug is pulled" (the
// spin-up time), then decays as sech(t/tau) while the core spreads, so the
// funnel deepens and widens, then relaxes away at a gravity-consistent rate.
// Both profiles are C1 at the junction, so nothing in the shape snaps.
//
// The surrounding sea is drawn in by warping the FFT-cascade sampling with
// the vortex's own particle map: features wind by the integrated rotation
//   Theta(r,t) = gain * S(t) / (2 pi r^2) * (1 - exp(-beta r^2/rc^2)),
// with S = integral of Gamma dt (closed form), and converge along the sink
//   rho(r) = sqrt(r^2 + Q(t) * (1 - exp(-r^2/rc^2))),
// so the ambient ripples themselves spiral into the drain. Sampled slopes
// are pulled back through the exact warp Jacobian. When the vortex dies the
// wound pattern crossfades back to the undisturbed sea (dispersive mixing)
// and the relaxing dimple overshoots into a low boil dome that radiates a
// gentle Cauchy-Poisson ring packet.
// Keep the height terms in sync with Whirlpool.cpp (CPU mirror for buoy).
//
// Several whirlpools can be live at once. Each one's particle map is a
// diffeomorphism, so they compose: the sample position is passed through each
// vortex in turn and the Jacobians multiply by the chain rule (water wound by
// one vortex and then pulled by the next). The surface fields - funnel, foam
// streaks and rim, boil dome and rebound rings - superpose linearly on top.
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
static const float WHIRL_SINK_MAX = 2.5;     // largest draw-in radius, in core radii
static const float WHIRL_HOLE_R = 0.12;      // drain mouth radius, in core radii
// Wound-sea detail budget. A vortex core both stretches the map and destroys
// short waves; past a couple of texels of stretch the fine bands cannot be
// carried anyway, so they are faded out rather than amplified into striations.
static const float WHIRL_STRETCH_KNEE  = 2.0; // stretch above which the wound wind-sea fades
static const float WHIRL_STRETCH_KNEE0 = 4.0; // bound on the wound swell's slope gain
static const float WHIRL_CHOP_KNEE     = 1.5; // stretch beyond which wound chop is scaled down
static const float WHIRL_CALM_R        = 2.0; // strain-calmed core: 1/e radius, in core radii
static const float WHIRL_THROAT_DARK   = 0.35; // unlit-water darkening down the throat
// Foam: bubbles are material, so it is drawn as a couple of streak lines that
// ride with the water plus a broken rim where the inflow breaks over the wall.
static const float WHIRL_STREAK_N    = 2.0;   // foam streaks per vortex
static const float WHIRL_STREAK_W    = 0.14;  // streak 1/e half-width, in core radii
static const float WHIRL_STREAK_PX   = 0.006; // + per metre of view distance (stays pixel-visible)
static const float WHIRL_STREAK_FOAM = 0.6;
static const float WHIRL_RIM_R       = 1.05;  // breaking-rim band centre, in core radii
static const float WHIRL_RIM_W       = 0.30;  // rim band 1/e half-width, in core radii
static const float WHIRL_RIM_FOAM    = 0.45;
static const float WHIRL_FOAM_GATE   = 0.20;  // wall slope below which no foam forms
static const float WHIRL_FOAM_LINGER = 1.5;   // foam persistence after the churn stops, in tau
// Collapse. Height terms are mirrored in Whirlpool.cpp (CPU, for the buoy).
static const float WHIRL_BOIL_AMP  = 0.12; // upwelling boil dome height, in peak depths
static const float WHIRL_BOIL_T    = 1.0;  // dome rise time, in decay times
static const float WHIRL_BOIL_R    = 1.5;  // dome Gaussian radius^2, in rcR^2
static const float WHIRL_REB_R     = 1.2;  // rebound length scale, in PEAK core radii (frozen)
static const float WHIRL_REB_R0    = 0.6;  // packet soft-core radius, in rcR
static const float WHIRL_REB_LAM   = 3.0;  // packet dominant wavelength, in rcR
static const float WHIRL_REB_AMP   = 0.15; // packet amplitude, in peak depths
static const float WHIRL_REB_TAU   = 6.0;  // packet temporal decay, s
static const float WHIRL_REB_DELAY = 1.0;  // packet release after the plug, in decay times

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
//
// The torque ramps as a smoothstep and releases as sech(t/tau); both are C1
// at the junction (sech'(0) = 0), so the funnel never snaps between phases -
// a kink there reads as a glitch rather than as water. S keeps a closed form
// in both phases (the decay integral is the Gudermannian), so the wound
// pattern stays exactly consistent with the circulation that wound it.
void WhirlEnvelope(int i, float age, out float rc, out float D, out float S, out float wBlend)
{
    float depthPeak = gWhirl[i].w;
    float grow = gWhirl2[i].x, tau = gWhirl2[i].y, rcPeak = gWhirl2[i].z;
    float gmax = 2.0 * PI * rcPeak * sqrt(2.0 * WHIRL_G * depthPeak);
    float gam;
    if (age <= grow)
    {
        float u = age / grow;
        float sm = u * u * (3.0 - 2.0 * u);                   // smooth torque ramp
        gam = gmax * sm;
        S = gmax * grow * (u * u * u - 0.5 * u * u * u * u);  // int gmax*sm dt; 0.5*gmax*grow at u = 1
        rc = rcPeak * (0.6 + 0.4 * sm);                       // born wide: no centimetre-scale stage
        wBlend = 1.0;
    }
    else
    {
        float d = age - grow;
        float x = d / tau;
        float ex = exp(-x);
        float sech = 2.0 * ex / (1.0 + ex * ex);              // overflow-free 1/cosh
        gam = gmax * sech;
        S = gmax * (0.5 * grow + tau * 2.0 * atan(tanh(0.5 * x))); // Gudermannian
        rc = rcPeak * (1.0 + 0.5 * (1.0 - sech));             // diffusive spread, C1 at d = 0
        float y = d / gWhirl3[i].w;
        wBlend = exp(-y * y);   // holds while it still spins, then dissolves decisively
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

// Largest singular value of the warp Jacobian: the factor by which the map
// stretches a step in its worst direction. Rotation-invariant (the row-norm
// it replaces under-read this by up to 26% when the warp carried an azimuthal
// wobble) and exactly 1 for the identity, so an unwarped sample is untouched.
float WhirlStretch(float4 J)
{
    float fro = dot(J, J);              // s1^2 + s2^2
    float dt = J.x * J.w - J.y * J.z;   // s1 * s2
    float disc = sqrt(max(fro * fro - 4.0 * dt * dt, 0.0));
    return sqrt(0.5 * (fro + disc));
}

// Detail budget for the warped sea. Two things remove the short waves inside
// a vortex, and both are physical: the strain of the core tears them apart,
// and a map step stretched past a couple of texels cannot carry them anyway -
// left in, they come back as slopes multiplied by the same stretch, which is
// exactly the fine concentric striation that made the old vortex read as
// noise. So the wound swell's slope gain is bounded and the fine bands are
// faded by stretch and by the strain-calm weight. This engages inside roughly
// three core radii for every preset, by design; it is exactly (fades) where
// stretch <= the knee and calm == 0, i.e. wherever no vortex is live.
float3 WhirlDetailFades(float3 fades, float stretch, float calm)
{
    float det  = saturate(WHIRL_STRETCH_KNEE  / max(stretch, 1e-3));
    float det0 = saturate(WHIRL_STRETCH_KNEE0 / max(stretch, 1e-3));
    return float3(fades.x * det0 * (1.0 - 0.35 * calm),
                  fades.y * det * (1.0 - 0.8 * calm),
                  fades.z * det * det * (1.0 - calm));
}

// One vortex's particle-map warp of the ambient-sea sampling. Returns the
// sample position, the Jacobian J = d(samp)/d(p) packed as (J00,J01,J10,J11),
// the pattern rotation angle, and this vortex's warped-vs-plain blend weight.
// False means it contributes nothing here (inactive, or out of range).
bool WhirlWarpOne(int i, float2 p, out float2 samp, out float4 J, out float th,
                  out float wBlend, out float calm)
{
    samp = p;
    J = float4(1, 0, 0, 1);
    th = 0.0;
    wBlend = 0.0;
    calm = 0.0;

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

    // Strain-calm weight: the shear and convergence of the core flatten the
    // short waves it drags in. spin = sqrt(D/depthPeak) * rc/rcPeak is
    // identically Gamma/Gamma_max (since D = Gamma^2 / (8 pi^2 g rc^2)), so a
    // vortex that has spun down calms nothing and the sea comes back by itself.
    float spin = sqrt(saturate(D / max(gWhirl[i].w, 1e-3))) * rc / gWhirl2[i].z;
    calm = spin * exp(-eta / (WHIRL_CALM_R * WHIRL_CALM_R));

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
               out float wBlend, out float calm)
{
    samp = worldXZ;
    J = float4(1, 0, 0, 1);
    float thTotal = 0.0;
    float plainWeight = 1.0;
    float calmAcc = 1.0;

    for (int i = 0; i < WHIRL_MAX; ++i)
    {
        float2 p;
        float4 Ji;
        float th, wb, ci;
        [branch] if (!WhirlWarpOne(i, samp, p, Ji, th, wb, ci))
            continue;
        samp = p;
        // J = Ji * J (2x2, row-major in xyzw)
        J = float4(Ji.x * J.x + Ji.y * J.z, Ji.x * J.y + Ji.y * J.w,
                   Ji.z * J.x + Ji.w * J.z, Ji.z * J.y + Ji.w * J.w);
        thTotal += th;
        plainWeight *= 1.0 - wb;
        calmAcc *= 1.0 - ci;
    }

    float s, c;
    sincos(thTotal, s, c);
    rotCS = float2(c, s); // world vector = R(+total) * sampled vector
    wBlend = 1.0 - plainWeight;
    calm = 1.0 - calmAcc;  // overlapping cores calm cumulatively
}

// Value noise periodic in x with integer period px, for break-up patterns
// drawn in the material angle (x = psi * px / 2pi). atan2 puts a branch cut
// at +-pi, so an ordinary noise would leave a visible seam along that ray;
// wrapping the lattice makes the pattern continuous around the circle.
float WhirlNoisePX(float2 p, float px)
{
    float2 ip = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float x0 = ip.x - px * floor(ip.x / px);
    float x1 = (x0 + 1.0 < px) ? x0 + 1.0 : 0.0;
    float a = Hash12(float2(x0, ip.y));
    float b = Hash12(float2(x1, ip.y));
    float c = Hash12(float2(x0, ip.y + 1.0));
    float d = Hash12(float2(x1, ip.y + 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// One whirlpool's own surface: the funnel depression, material-line foam (a
// couple of streaks plus a broken rim) and the collapse boil dome + ring
// packet. Height terms are mirrored in Whirlpool::HeightAt (CPU, for the buoy).
//
// What is deliberately absent: the old spiral "ripple arms". Their radial
// wavenumber was karm - 3*Theta'(r), and since Theta' < 0 the winding ADDED
// to it, so three arms meant to sweep outward collapsed into a metre-pitch
// concentric grating - a dozen near-circular grooves the mesh could not
// resolve and the eye read as noise. A real drain has no such corrugation:
// the wall is glassy and the only fine structure is where bubbles ride.
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
    float depthPeak = gWhirl[i].w;
    float grow = gWhirl2[i].x, tau = gWhirl2[i].y, rcPeak = gWhirl2[i].z;

    float rc, D, S, wb;
    WhirlEnvelope(i, age, rc, D, S, wb);
    float rc2 = rc * rc;
    float eta = r * r / rc2;

    // --- Funnel: cyclostrophic free-surface depression ---
    float inv = 1.0 / (1.0 + eta);
    float ddr = D * (2.0 * r / rc2) * inv * inv; // dh/dr in the grid parameter, > 0
    disp.y += -D * inv;
    // Slight inward pull sharpens the throat. Its radial gradient at the
    // centre is pull * 2D/rc, so a steep funnel would bunch (and past 1,
    // fold) the ocean grid onto itself - visible as terraced rings. Bound it
    // to a fixed compression; the default sits under the cap untouched.
    float pull = min(0.35, 0.3 * rc / max(D, 1e-3));
    disp.xz += -u * ddr * rc * pull;
    // That pull also compresses the grid radially by dr'/dr = 1 - cP, so the
    // lit (world-space) slope is the parameter slope divided by it. Without
    // this the shading disagrees with the geometry exactly where the wall is
    // steepest, which is what made the throat rim look torn.
    float cP = pull * (2.0 * D / rc) * inv * inv * inv * (1.0 - 3.0 * eta);
    slope += u * ddr / max(1.0 - cP, 0.15);

    // --- Foam. Bubbles are material: they ride the water rather than sitting
    // still in space, so the foam is drawn along lines of constant material
    // angle psi = theta - Theta(r) - exactly the lines the warp winds, so the
    // streaks shear and rotate with the sea they float on. A broken rim marks
    // where the inflow breaks over the wall. Gated by wall steepness (a lazy
    // drain makes no bubbles) and lingering briefly after the churn stops.
    // Pixel-only: foam adds no height, so it can never alias the geometry.
    float d = age - grow;
    // 0.6495 = max of 2t/(1+t^2)^2, i.e. the peak wall slope in units of D/rc.
    float gateNow = saturate((0.6495 * D / rc - WHIRL_FOAM_GATE) * 6.0);
    float gatePk = saturate((0.6495 * depthPeak / rcPeak - WHIRL_FOAM_GATE) * 6.0);
    float linger = (d > 0.0) ? gatePk * exp(-d / (WHIRL_FOAM_LINGER * tau)) : 0.0;
    float foamK = max(gateNow, linger);
    [branch] if (foamK > 1e-3 && r < 6.0 * rc)
    {
        float Th, Thp;
        WhirlWinding(i, r, rc, S, Th, Thp);
        float theta = atan2(dv.y, dv.x);
        float psi = theta - Th - 2.1 * float(i);   // material angle, offset per vortex
        float lnr = log(r / rc);
        // Break-up drawn in material coordinates, periodic in psi so the
        // atan2 branch cut leaves no seam.
        float2 sc = float2(psi * (8.0 / (2.0 * PI)), 1.8 * lnr + 0.08 * age + 7.3 * float(i));
        float nS = 0.65 * WhirlNoisePX(sc, 8.0) + 0.35 * WhirlNoisePX(sc * 2.0 + 3.1, 16.0);
        float nR = 0.65 * WhirlNoisePX(sc + float2(2.5, 3.7), 8.0) + 0.35 * WhirlNoisePX(sc * 2.0 + 9.4, 16.0);

        // Streaks: Gaussian in the perpendicular distance to the nearest
        // material line. |grad psi| converts an angle offset into metres, so
        // the lines keep a real width as they wind in toward the throat.
        float dpsi = (frac(psi * WHIRL_STREAK_N / (2.0 * PI) + 0.5) - 0.5) * (2.0 * PI / WHIRL_STREAK_N);
        float gpsi = sqrt(Thp * Thp + 1.0 / (r * r));
        float spacing = 2.0 * PI / (WHIRL_STREAK_N * gpsi);
        float W = min(WHIRL_STREAK_W * rc * sqrt(max(1.0, r / (1.5 * rc)))
                      + WHIRL_STREAK_PX * length(worldXZ - gCamPos.xz) * gZoom.y, 0.25 * spacing);
        float dperp = dpsi / gpsi;
        float streak = exp(-dperp * dperp / (W * W));
        float segs = smoothstep(0.30, 0.65, nS);          // broken into lit segments
        float sEnv = smoothstep(0.30 * rc, 0.65 * rc, r)  // glassy eye
                   * exp(-(lnr - 0.3) * (lnr - 0.3) / 0.81)
                   * saturate(2.0 - r / (3.0 * rc));      // clean cut-off inside the 6 rc branch

        float xr = (r / rc - WHIRL_RIM_R) / WHIRL_RIM_W;
        float rim = exp(-xr * xr) * (0.3 + 0.7 * smoothstep(0.3, 0.7, nR));

        foam += foamK * (WHIRL_STREAK_FOAM * streak * segs * sEnv + WHIRL_RIM_FOAM * rim);
    }

    // --- Collapse: the relaxing depression does not simply flatten, it
    // overshoots. The column of water that was held down rises into a low
    // upwelling boil which then radiates a gentle Cauchy-Poisson ring packet.
    // The packet's scale radius is frozen to the PEAK core, so its wavelength
    // cannot drift as rc spreads; the soft core is C1, so the centre has no
    // kink; and nothing here is steep enough to make foam.
    [branch] if (d > 0.0)
    {
        float rcR = WHIRL_REB_R * rcPeak;
        float etaR = r * r / (rcR * rcR);
        float x = d / (WHIRL_BOIL_T * tau);
        float xe = x * x * exp(2.0 * (1.0 - x));   // 0 -> 1 at x = 1 -> 0
        float B = WHIRL_BOIL_AMP * depthPeak * xe;
        float dome = exp(-etaR / WHIRL_BOIL_R);
        disp.y += B * dome;
        slope += u * (-B * dome * 2.0 * r / (WHIRL_BOIL_R * rcR * rcR));

        float dR = d - WHIRL_REB_DELAY * tau;
        [branch] if (dR > 0.05)
        {
            float r0 = WHIRL_REB_R0 * rcR;
            float rr = sqrt(r * r + r0 * r0);      // C1 soft core (no plateau)
            float kloc = WHIRL_G * dR * dR / (4.0 * rr * rr);
            float k0 = 2.0 * PI / (WHIRL_REB_LAM * rcR);
            float phase = -WHIRL_G * dR * dR / (4.0 * rr) + k0 * r0;
            float lx = log(max(kloc / k0, 1e-6));
            float envr = exp(-lx * lx * 1.4);
            float Ar = WHIRL_REB_AMP * depthPeak * sqrt(rcR / (rcR + r)) * exp(-dR / WHIRL_REB_TAU) * envr;
            float sr, cr;
            sincos(phase, sr, cr);
            disp.y += Ar * cr;
            // exact d(phase)/dr through the soft core: vanishes at r = 0
            slope += u * (-Ar * kloc * sr * (r / rr));
        }
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
        // Conservative reject before any envelope work: the throat shading
        // reaches at most the spread core radius, 1.5x the peak.
        float2 d = worldXZ - gWhirl[i].xy;
        float r = length(d);
        if (r > 1.5 * gWhirl2[i].z)
            continue;

        float rc, D, S, wb;
        WhirlEnvelope(i, age, rc, D, S, wb);
        float open = saturate(D / max(gWhirl[i].w, 1e-3)); // 0 at spawn -> 1 at full spin-up
        // Deep, unlit water down the throat: a soft gradient from ~0.7 rc into
        // the mouth, so the eye reads as the bottom of a dark bowl rather than
        // a black dot pasted on lit water.
        float eta = r * r / (rc * rc);
        float throat = open * exp(-eta / 0.30) * (1.0 - smoothstep(0.7 * rc, rc, r));
        float dk = WHIRL_THROAT_DARK * throat;
        float holeR = WHIRL_HOLE_R * rc * open;
        [branch] if (holeR >= 1e-3 && r <= holeR * 2.8)
        {
            float core = 1.0 - smoothstep(holeR * 0.75, holeR * 1.05, r); // pure black
            float shaft = 1.0 - smoothstep(holeR, holeR * 2.8, r);        // its shadowed lip
            dk = max(dk, max(core, 0.7 * shaft));
        }
        dark = max(dark, dk);
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
    foam = min(foam, 0.85); // rim + streaks (+ a neighbour) never reach solid white
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
