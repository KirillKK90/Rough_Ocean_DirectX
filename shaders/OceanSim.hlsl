// FFT ocean simulation (Tessendorf) compute shaders.
//
// Pipeline per cascade, per frame:
//   1. CSSpectrumInit   (on parameter change) -> h0(k), conj(h0(-k))
//   2. CSSpectrumUpdate (per frame)           -> packed time-evolved spectra
//   3. CSFFT x4         (rows + columns, two packed float4 buffers)
//   4. CSAssemble       -> displacement / derivative / foam textures
//
// Packing (each float4 holds two complex numbers; FFT of a float4 performs
// two independent complex FFTs sharing twiddles):
//   Buf0.xy = Dx + i*Dy          Buf0.zw = Dz + i*dDy/dx
//   Buf1.xy = dDy/dz + i*dDx/dx  Buf1.zw = dDz/dz + i*dDx/dz
// All eight signals have Hermitian spectra, so after the inverse FFT the
// real/imaginary parts are the eight real fields.

static const float G = 9.81;
static const float PI = 3.14159265359;

cbuffer SimCB : register(b0)
{
    uint  gN;        float gLpatch;   float gSimTime;  float gDt;
    float2 gWindD;   float gU10;      float gFetch;      // wind unit dir, speed m/s, fetch m
    float gAmp;      float gSpreadExp; float gMinK;    float gMaxK;
    float gChop;     float gFoamBias; float gFoamDecay; float gFoamAdd;
    uint  gSeed;     uint gFFTDir;    float gSmallCut; float gSwellAmp;
    float2 gSwellDir; float gSwellK;  float gSwellSpread;
    // gSmallCut: small-wave suppression length l, m (Tessendorf exp(-k^2 l^2)).
    // gSwellAmp: per-texel swell amplitude, already normalized on the CPU.
    // gSwellDir/K/Spread: swell ridge direction, peak wavenumber, lobe power.
}

StructuredBuffer<float4>   SrvBuf0 : register(t0);
StructuredBuffer<float4>   SrvBuf1 : register(t1);
RWStructuredBuffer<float4> UavBuf0 : register(u0);
RWStructuredBuffer<float4> UavBuf1 : register(u1);

RWTexture2D<float4> OutDisplacement : register(u2);
RWTexture2D<float4> OutDerivatives  : register(u3);
RWTexture2D<float>  OutFoam         : register(u4);

float2 CMul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// ---------------------------------------------------------------------------
// Random numbers: PCG hash -> Box-Muller gaussian pair, deterministic per texel.
// ---------------------------------------------------------------------------
uint PcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float2 GaussianPair(uint2 id, uint seed)
{
    uint h1 = PcgHash(id.x + id.y * 65537u + seed * 386243u);
    uint h2 = PcgHash(h1);
    float u1 = max((h1 & 0xFFFFFF) / 16777216.0, 1e-6);
    float u2 = (h2 & 0xFFFFFF) / 16777216.0;
    float r = sqrt(-2.0 * log(u1));
    return float2(r * cos(2.0 * PI * u2), r * sin(2.0 * PI * u2));
}

// ---------------------------------------------------------------------------
// JONSWAP spectrum with directional spreading (Horvath-style synthesis).
// ---------------------------------------------------------------------------
float JonswapS(float omega)
{
    float wp = 22.0 * pow(G * G / (gU10 * gFetch), 1.0 / 3.0); // peak frequency
    float alpha = 0.076 * pow(gU10 * gU10 / (gFetch * G), 0.22);
    float sigma = omega <= wp ? 0.07 : 0.09;
    float r = exp(-(omega - wp) * (omega - wp) / (2.0 * sigma * sigma * wp * wp));
    float gamma = 3.3;
    float w5 = omega * omega * omega * omega * omega;
    return alpha * G * G / max(w5, 1e-9) * exp(-1.25 * pow(wp / omega, 4.0)) * pow(gamma, r);
}

float DirectionalSpread(float2 kdir)
{
    float d = dot(kdir, gWindD);
    // cos^(2s)(theta/2) style lobe; waves running against the wind are damped.
    float s = pow(saturate((1.0 + d) * 0.5), gSpreadExp);
    if (d < 0.0)
        s *= 0.12;
    return lerp(0.08, 1.0, s);
}

// Long-crested swell from a distant storm: a narrow Gaussian ridge in k
// around gSwellK, focused into a tight one-sided directional lobe. Its energy
// lives at the low-frequency end of the spectrum - big rolling waves, no
// added chop. Keep in sync with the normalization sum in Ocean.cpp (CPU).
float SwellShape(float klen, float2 khat)
{
    if (gSwellAmp <= 0.0 || gSwellK <= 0.0)
        return 0.0;
    float x = (klen - gSwellK) / (0.25 * gSwellK); // relative bandwidth 0.25
    float lobe = pow(saturate(dot(khat, gSwellDir)), gSwellSpread);
    return exp(-x * x) * lobe;
}

// Amplitude for one k-space texel (integrated over the texel's dk x dk cell).
float SpectrumAmplitude(float2 k)
{
    float klen = length(k);
    if (klen < gMinK || klen >= gMaxK || klen < 1e-5)
        return 0.0;
    float2 khat = k / klen;

    float omega = sqrt(G * klen);
    float dwdk = G / (2.0 * omega);
    float dk = 2.0 * PI / gLpatch;

    float S = JonswapS(omega) * DirectionalSpread(khat);
    float amp2 = 2.0 * S * dwdk / klen * dk * dk;

    // Soften the band edges so cascade splits don't ring.
    float edge = smoothstep(gMinK, gMinK * 1.25, klen) * (1.0 - smoothstep(gMaxK * 0.85, gMaxK, klen));
    // Tessendorf small-wave suppression: waves shorter than ~gSmallCut carry
    // almost no visible shape - they only alias into glint noise - so their
    // energy is removed at the source.
    edge *= exp(-klen * klen * gSmallCut * gSmallCut);

    float aWind = sqrt(max(amp2, 0.0)) * edge * gAmp;
    // Swell is independent of the local wind sea; energies add, so the
    // amplitudes combine in quadrature. gSwellAmp is pre-normalized on the
    // CPU (divided by sqrt(sum G^2) over this cascade's k-grid) so the total
    // swell energy is independent of patch size and FFT resolution.
    float aSwell = gSwellAmp * SwellShape(klen, khat) * edge;
    return sqrt(aWind * aWind + aSwell * aSwell);
}

float2 WaveK(uint2 id)
{
    float2 idx = float2(id) - float(gN) * 0.5;
    return idx * (2.0 * PI / gLpatch);
}

// h0 buffer layout: .xy = h0(k), .zw = conj(h0(-k))
[numthreads(8, 8, 1)]
void CSSpectrumInit(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gN || id.y >= gN)
        return;

    float2 k = WaveK(id.xy);
    uint2 idNeg = uint2((gN - id.x) % gN, (gN - id.y) % gN);
    float2 kNeg = WaveK(idNeg);

    float2 xi = GaussianPair(id.xy, gSeed);
    float2 xiNeg = GaussianPair(idNeg, gSeed);

    float2 h0k = xi * (SpectrumAmplitude(k) / sqrt(2.0));
    float2 h0mk = xiNeg * (SpectrumAmplitude(kNeg) / sqrt(2.0));

    UavBuf0[id.y * gN + id.x] = float4(h0k, h0mk.x, -h0mk.y);
}

[numthreads(8, 8, 1)]
void CSSpectrumUpdate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gN || id.y >= gN)
        return;

    uint addr = id.y * gN + id.x;
    float4 h0 = SrvBuf0[addr];
    float2 k = WaveK(id.xy);
    float klen = max(length(k), 1e-6);

    float omega = sqrt(G * klen);
    float c = cos(omega * gSimTime);
    float s = sin(omega * gSimTime);

    // h(k,t) = h0(k) e^{iwt} + conj(h0(-k)) e^{-iwt}
    float2 h = CMul(h0.xy, float2(c, s)) + CMul(h0.zw, float2(c, -s));
    float2 ih = float2(-h.y, h.x); // i*h

    float2 khat = k / klen;

    float2 Dx = ih * khat.x;      // i kx/k h
    float2 Dy = h;
    float2 Dz = ih * khat.y;
    float2 dDy_dx = ih * k.x;     // i kx h
    float2 dDy_dz = ih * k.y;
    float2 dDx_dx = -h * k.x * khat.x;  // -kx^2/k h
    float2 dDz_dz = -h * k.y * khat.y;
    float2 dDx_dz = -h * k.x * khat.y;  // -kx kz/k h

    // Pack pairs of Hermitian signals: (A + iB) has spectrum SA + i*SB.
    float2 p0 = Dx + float2(-Dy.y, Dy.x);
    float2 p1 = Dz + float2(-dDy_dx.y, dDy_dx.x);
    float2 p2 = dDy_dz + float2(-dDx_dx.y, dDx_dx.x);
    float2 p3 = dDz_dz + float2(-dDx_dz.y, dDx_dz.x);

    UavBuf0[addr] = float4(p0, p1);
    UavBuf1[addr] = float4(p2, p3);
}

// ---------------------------------------------------------------------------
// Stockham radix-2 FFT, one dispatch per direction, whole line in LDS.
// Sign convention: +i (inverse transform), unnormalized.
// Compiled per size with FFT_SIZE / FFT_LOG2 defines.
// Group: (line index, buffer select). Threads: FFT_SIZE/2.
// ---------------------------------------------------------------------------
#ifndef FFT_SIZE
#define FFT_SIZE 256
#define FFT_LOG2 8
#endif

groupshared float4 ldsA[FFT_SIZE];
groupshared float4 ldsB[FFT_SIZE];

[numthreads(FFT_SIZE / 2, 1, 1)]
void CSFFT(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    const uint N = FFT_SIZE;
    uint lineIdx = gid.x;
    uint bufSel = gid.y;
    uint t = gtid.x;

    uint i0 = t;
    uint i1 = t + N / 2;
    // gFFTDir: 0 = rows (line = y), 1 = columns (line = x)
    uint addr0 = (gFFTDir == 0) ? (lineIdx * N + i0) : (i0 * N + lineIdx);
    uint addr1 = (gFFTDir == 0) ? (lineIdx * N + i1) : (i1 * N + lineIdx);

    ldsA[i0] = (bufSel == 0) ? UavBuf0[addr0] : UavBuf1[addr0];
    ldsA[i1] = (bufSel == 0) ? UavBuf0[addr1] : UavBuf1[addr1];

    bool fromA = true;
    uint Ns = 1;
    [unroll]
    for (uint stage = 0; stage < FFT_LOG2; ++stage)
    {
        GroupMemoryBarrierWithGroupSync();
        uint base = (t / Ns) * (Ns * 2);
        uint off = t % Ns;
        float ang = 2.0 * PI * float(off) / float(Ns * 2); // +i: inverse
        float2 w = float2(cos(ang), sin(ang));

        float4 v0 = fromA ? ldsA[t] : ldsB[t];
        float4 vr = fromA ? ldsA[t + N / 2] : ldsB[t + N / 2];
        float4 v1 = float4(CMul(vr.xy, w), CMul(vr.zw, w));

        uint d0 = base + off;
        uint d1 = d0 + Ns;
        if (fromA) { ldsB[d0] = v0 + v1; ldsB[d1] = v0 - v1; }
        else       { ldsA[d0] = v0 + v1; ldsA[d1] = v0 - v1; }
        fromA = !fromA;
        Ns *= 2;
    }
    GroupMemoryBarrierWithGroupSync();

    float4 r0 = fromA ? ldsA[i0] : ldsB[i0];
    float4 r1 = fromA ? ldsA[i1] : ldsB[i1];
    if (bufSel == 0) { UavBuf0[addr0] = r0; UavBuf0[addr1] = r1; }
    else             { UavBuf1[addr0] = r0; UavBuf1[addr1] = r1; }
}

// ---------------------------------------------------------------------------
// Unpack FFT results into displacement/derivative textures, accumulate foam.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void CSAssemble(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gN || id.y >= gN)
        return;

    uint addr = id.y * gN + id.x;
    float sgn = ((id.x + id.y) & 1) ? -1.0 : 1.0; // spectrum was centered at N/2

    float4 b0 = SrvBuf0[addr] * sgn;
    float4 b1 = SrvBuf1[addr] * sgn;

    float Dx = b0.x, Dy = b0.y, Dz = b0.z;
    float dDy_dx = b0.w;
    float dDy_dz = b1.x, dDx_dx = b1.y, dDz_dz = b1.z, dDx_dz = b1.w;

    // Slope second moment rides in disp.w: mip-averaging E[s^2] alongside the
    // mean slope lets the pixel shader recover the slope variance inside its
    // filter footprint (CLEAN mapping) and widen the specular highlight by it,
    // instead of the unresolved waves sparkling as per-pixel noise.
    float M = 0.5 * (dDy_dx * dDy_dx + dDy_dz * dDy_dz);
    OutDisplacement[id.xy] = float4(Dx, Dy, Dz, M);
    OutDerivatives[id.xy] = float4(dDy_dx, dDy_dz, dDx_dx, dDz_dz);

    // Jacobian of the horizontal (choppy) displacement: folding -> whitecaps.
    float jxx = 1.0 + gChop * dDx_dx;
    float jzz = 1.0 + gChop * dDz_dz;
    float jxz = gChop * dDx_dz;
    float J = jxx * jzz - jxz * jxz;

    float foam = OutFoam[id.xy];
    foam *= exp(-gFoamDecay * gDt);
    float inject = saturate((gFoamBias - J) * gFoamAdd);
    foam = saturate(foam + inject * gDt * 4.0);
    OutFoam[id.xy] = foam;
}
