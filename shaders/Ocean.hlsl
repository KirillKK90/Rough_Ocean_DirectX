// Ocean surface rendering: radial camera-centered grid displaced by the FFT
// cascades, shaded with GGX sun glints, sky reflection, subsurface scattering
// and whitecap foam.

#include "Common.hlsli"

Texture2D<float4> tDisp0  : register(t0);
Texture2D<float4> tDeriv0 : register(t1);
Texture2D<float>  tFoam0  : register(t2);
Texture2D<float4> tDisp1  : register(t3);
Texture2D<float4> tDeriv1 : register(t4);
Texture2D<float>  tFoam1  : register(t5);
Texture2D<float4> tDisp2  : register(t6);
Texture2D<float4> tDeriv2 : register(t7);
Texture2D<float>  tFoam2  : register(t8);
TextureCube<float4> tSky  : register(t9);

struct VSIn
{
    float2 off : POSITION; // horizontal offset from the camera, meters
};

struct VSOut
{
    float4 pos : SV_Position;
    float3 rel : TEXCOORD0;   // camera-relative world position
    float2 worldXZ : TEXCOORD1;
    float3 fades : TEXCOORD2; // per-cascade distance fade
    float dispY : TEXCOORD3;
    float2 gridXZ : TEXCOORD4; // undisplaced grid position = the surface parameter
};

float3 CascadeFades(float dist)
{
    return float3(
        exp(-dist / gCascade0.y),
        exp(-dist / gCascade1.y),
        exp(-dist / gCascade2.y));
}

// Distance-matched displacement mip: at range the radial mesh strides many
// map texels per vertex, so sample the pyramid at the level whose texel pitch
// matches the local vertex spacing. This is the vertex-shader counterpart of
// the pixel shader's anisotropic filtering - without it the horizon geometry
// point-samples fine waves and crawls.
float CascadeLod(float dist, float invL)
{
    // vertex spacing (m) = dist * gGridScale; texel pitch (m) = L / N.
    return log2(max(dist * gGridScale * gCascade1.w * invL, 1.0));
}

// ---------------------------------------------------------------------------
// Horizon-sun glitter shimmer (Sunset only, gSunFx.x > 0).
// A real sun path is made of countless capillary facets that flash and die
// far faster than the resolved waves change. The filtered maps average them,
// correctly, into a smooth sheen - right on average, but static. This puts the
// averaged-away variance back as a mean-1 random modulation of the sun
// specular: cells matched to the pixel footprint along and across the sun
// path (so the path breaks into the familiar horizontal dashes, never finer
// than ~2 px, never per-pixel noise), each cell flaring and fading in place.
// ---------------------------------------------------------------------------
// A value-noise field that evolves in place is a blend of two random slices;
// this picks the slice offsets (xy, zw) and blend weight for time t.
void GlitterSlices(float t, float seed, out float4 o, out float f)
{
    float k = floor(t);
    f = smoothstep(0.0, 1.0, t - k);
    o = float4(Hash12(float2(k, seed)), Hash12(float2(seed, k)),
               Hash12(float2(k + 1.0, seed)), Hash12(float2(seed, k + 1.0))) * 97.0;
}

// Two independent twinkling fields beat against each other; E = 0.5 * 0.5.
float GlitterCell(float2 p, float4 oa, float fa, float4 ob, float fb)
{
    float a = lerp(ValueNoise(p + oa.xy), ValueNoise(p + oa.zw), fa);
    float2 q = p * 1.31 + 7.7;
    float b = lerp(ValueNoise(q + ob.xy), ValueNoise(q + ob.zw), fb);
    return a * b * 4.0;
}

// uv: position in a sun-aligned frame (m); fp: pixel footprint along each axis (m).
float GlitterShimmer(float2 uv, float2 fp, float t)
{
    float4 oa, ob;
    float fa, fb;
    GlitterSlices(t * 1.7, 3.0, oa, fa);
    GlitterSlices(t * 2.3, 11.0, ob, fb);

    const float c = 0.3; // capillary cell size, m
    float2 lod = log2(max(fp * 2.0 / c, 1.0));
    float2 l0 = floor(lod);
    float2 f = lod - l0;
    float2 c0 = c * exp2(l0);
    float s00 = GlitterCell(uv / c0, oa, fa, ob, fb);
    float s10 = GlitterCell(uv / (c0 * float2(2, 1)) + 31.0, oa, fa, ob, fb);
    float s01 = GlitterCell(uv / (c0 * float2(1, 2)) + 57.0, oa, fa, ob, fb);
    float s11 = GlitterCell(uv / (c0 * 2.0) + 92.0, oa, fa, ob, fb);
    return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

VSOut VSOcean(VSIn v)
{
    VSOut o;
    float2 worldXZ = gCamPos.xz + v.off;
    float dist = length(v.off);
    float3 fades = CascadeFades(dist);

    // Whirlpool: the ambient sea is sampled through the vortex particle map,
    // so the existing waves visibly wind and converge into the drain. While
    // the dead vortex's wound pattern dissolves (0 < wBlend < 1) both the
    // warped and the plain sea are sampled and crossfaded.
    float2 wSamp;
    float4 wJ;
    float2 wRot;
    float wBlend, wCalm;
    WhirlWarp(worldXZ, wSamp, wJ, wRot, wBlend, wCalm);

    float3 lods = float3(
        CascadeLod(dist, gCascade0.x),
        CascadeLod(dist, gCascade1.x),
        CascadeLod(dist, gCascade2.x));

    float3 disp = 0;
    [branch] if (wBlend < 0.999)
    {
        float3 dp = 0;
        dp += fades.x * tDisp0.SampleLevel(samLinearWrap, worldXZ * gCascade0.x, lods.x).xyz;
        dp += fades.y * tDisp1.SampleLevel(samLinearWrap, worldXZ * gCascade1.x, lods.y).xyz;
        dp += fades.z * tDisp2.SampleLevel(samLinearWrap, worldXZ * gCascade2.x, lods.z).xyz;
        disp += (1.0 - wBlend) * dp;
    }
    [branch] if (wBlend > 0.001)
    {
        // A vertex step strides `st` times further through the map than through
        // the world, so sample the level whose texel pitch matches that - the
        // vertex-shader counterpart of the pixel shader's anisotropic filter -
        // and fade what the core's strain has destroyed.
        float st = WhirlStretch(wJ);
        float lodBias = log2(max(st, 1.0));
        float3 fw = WhirlDetailFades(fades, st, wCalm);
        float3 dw = 0;
        dw += fw.x * tDisp0.SampleLevel(samLinearWrap, wSamp * gCascade0.x, lods.x + lodBias).xyz;
        dw += fw.y * tDisp1.SampleLevel(samLinearWrap, wSamp * gCascade1.x, lods.y + lodBias).xyz;
        dw += fw.z * tDisp2.SampleLevel(samLinearWrap, wSamp * gCascade2.x, lods.z + lodBias).xyz;
        // rotate the sampled choppy vector into the wound pattern's frame
        dw.xz = float2(wRot.x * dw.x - wRot.y * dw.z, wRot.y * dw.x + wRot.x * dw.z);
        // Across the compressed axis a particle displacement is carried by
        // J^-1, so it shrinks by 1/stretch. Without this the wound chop keeps
        // its map-space size while the grid around it is squeezed, and the
        // surface folds over itself into the jagged rim.
        dw.xz *= saturate(WHIRL_CHOP_KNEE / st);
        disp += wBlend * dw;
    }
    disp.xz *= gLambda;

    // Meteorite impact rings superimpose linearly on the wind sea.
    float3 impDisp;
    float2 impSlope;
    float impFoam;
    ImpactWaves(worldXZ, impDisp, impSlope, impFoam);
    disp += impDisp;

    // Whirlpool funnel, foam and collapse rings.
    float3 whDisp;
    float2 whSlope;
    float whFoam;
    WhirlWaves(worldXZ, whDisp, whSlope, whFoam);
    disp += whDisp;

    float3 rel = float3(v.off.x + disp.x, disp.y - gCamPos.y, v.off.y + disp.z);
    o.pos = mul(float4(rel, 1.0), gViewProj);
    o.rel = rel;
    o.worldXZ = worldXZ + disp.xz;
    o.gridXZ = worldXZ;   // the parameter the whirl terms were evaluated at
    o.fades = fades;
    o.dispY = disp.y;
    return o;
}

float4 PSOcean(VSOut i) : SV_Target
{
    float dist = length(i.rel);
    float3 E = i.rel / dist;   // view ray (from eye)
    float3 V = -E;

    // Whirlpool warp (must match VSOcean): sampled slopes come back through
    // the Jacobian so lighting agrees with the wound displacement field. It is
    // evaluated at the UNDISPLACED grid position, exactly as the vertex shader
    // did - feeding it the displaced position instead made the shading solve a
    // different vortex than the geometry, which is what tore the throat rim.
    float2 wSamp;
    float4 wJ;
    float2 wRot;
    float wBlend, wCalm;
    WhirlWarp(i.gridXZ, wSamp, wJ, wRot, wBlend, wCalm);

    // --- Normal from analytic derivatives, cascade-faded ---
    // The maps are mip-chained and sampled anisotropically: each pixel gets
    // the slope averaged over its own footprint. What the filter averages
    // away is recovered as slope variance (E[s^2] rides in disp.w, CLEAN
    // mapping) and widens the specular lobe below, so unresolved fine waves
    // read as sheen instead of glinting per-pixel noise.
    float2 slope = 0;
    float sigma2 = 0; // slope variance filtered out of the maps at this footprint
    [branch] if (wBlend < 0.999)
    {
        float4 d0 = tDeriv0.Sample(samAnisoWrap, i.worldXZ * gCascade0.x);
        float4 d1 = tDeriv1.Sample(samAnisoWrap, i.worldXZ * gCascade1.x);
        float4 d2 = tDeriv2.Sample(samAnisoWrap, i.worldXZ * gCascade2.x);
        float m0 = tDisp0.Sample(samAnisoWrap, i.worldXZ * gCascade0.x).w;
        float m1 = tDisp1.Sample(samAnisoWrap, i.worldXZ * gCascade1.x).w;
        float m2 = tDisp2.Sample(samAnisoWrap, i.worldXZ * gCascade2.x).w;
        float w = 1.0 - wBlend;
        sigma2 += w * (i.fades.x * i.fades.x * max(0.0, m0 - 0.5 * dot(d0.xy, d0.xy))
                     + i.fades.y * i.fades.y * max(0.0, m1 - 0.5 * dot(d1.xy, d1.xy))
                     + i.fades.z * i.fades.z * max(0.0, m2 - 0.5 * dot(d2.xy, d2.xy)));
        float4 d = d0 * i.fades.x + d1 * i.fades.y + d2 * i.fades.z;
        slope += w * float2(d.x / max(1.0 + gLambda * d.z, 0.15),
                            d.y / max(1.0 + gLambda * d.w, 0.15));
    }
    [branch] if (wBlend > 0.001)
    {
        float stP = WhirlStretch(wJ);
        float chopF = saturate(WHIRL_CHOP_KNEE / stP);   // the VS scaled the wound chop by this
        float3 fw = WhirlDetailFades(i.fades, stP, wCalm);
        float4 d0 = tDeriv0.Sample(samAnisoWrap, wSamp * gCascade0.x);
        float4 d1 = tDeriv1.Sample(samAnisoWrap, wSamp * gCascade1.x);
        float4 d2 = tDeriv2.Sample(samAnisoWrap, wSamp * gCascade2.x);
        float m0 = tDisp0.Sample(samAnisoWrap, wSamp * gCascade0.x).w;
        float m1 = tDisp1.Sample(samAnisoWrap, wSamp * gCascade1.x).w;
        float m2 = tDisp2.Sample(samAnisoWrap, wSamp * gCascade2.x).w;
        // The variance filtered out in map space transforms through J too: for
        // isotropic sigma^2, E|J^T s|^2 = sigma^2 * tr(J^T J) / 2. So detail the
        // warp stretched past the pixel footprint widens the highlight instead
        // of sparkling in it.
        float jvar = 0.5 * dot(wJ, wJ);
        sigma2 += wBlend * jvar * (fw.x * fw.x * max(0.0, m0 - 0.5 * dot(d0.xy, d0.xy))
                                 + fw.y * fw.y * max(0.0, m1 - 0.5 * dot(d1.xy, d1.xy))
                                 + fw.z * fw.z * max(0.0, m2 - 0.5 * dot(d2.xy, d2.xy)));
        float4 d = d0 * fw.x + d1 * fw.y + d2 * fw.z;
        float2 sw = float2(d.x / max(1.0 + gLambda * chopF * d.z, 0.15),
                           d.y / max(1.0 + gLambda * chopF * d.w, 0.15));
        // slope_world = J^T * slope_sampled
        slope += wBlend * float2(wJ.x * sw.x + wJ.z * sw.y,
                                 wJ.y * sw.x + wJ.w * sw.y);
    }

    // Meteorite impact rings: analytic slopes give crisp per-pixel normals.
    float3 impDisp;
    float2 impSlope;
    float impFoam;
    ImpactWaves(i.worldXZ, impDisp, impSlope, impFoam);
    slope += impSlope;

    // Whirlpool funnel, foam and collapse rings.
    float3 whDisp;
    float2 whSlope;
    float whFoam;
    WhirlWaves(i.gridXZ, whDisp, whSlope, whFoam);
    slope += whSlope;

    // Screen-space slope derivatives catch whatever residual detail even the
    // texture filter passes through (analytic impact/whirl waves included) -
    // Tokuyoshi-style specular antialiasing.
    float2 sdx = ddx(slope), sdy = ddy(slope);
    sigma2 += 0.25 * (dot(sdx, sdx) + dot(sdy, sdy));
    sigma2 = min(sigma2, 0.25);

    float3 N = normalize(float3(-slope.x, 1.0, -slope.y));
    // keep normals from tipping past vertical toward the eye
    if (dot(N, V) < 0.0)
        N = normalize(N - 1.9 * dot(N, V) * V);

    float NdV = max(dot(N, V), 1e-3);

    // --- Foam coverage ---
    // The whitecap field is sampled through the same warp, so existing foam
    // streaks get dragged into the vortex spiral along with the waves.
    float2 foamXZ = lerp(i.worldXZ, wSamp, wBlend);
    float foamAcc = tFoam0.Sample(samAnisoWrap, foamXZ * gCascade0.x) * i.fades.x
                  + tFoam1.Sample(samAnisoWrap, foamXZ * gCascade1.x) * i.fades.y * 0.75;
    // Never sample the break-up through the warp: it is mip-less, so the
    // vortex's own compression would grind it into per-pixel grain.
    float foamTexture = Fbm(i.worldXZ * 0.9 + float2(0.07, 0.05) * gTime, 3);
    // The procedural break-up has no mip chain: ease it toward its mean at
    // range so it doesn't reintroduce the speckle the filtered maps removed.
    foamTexture = lerp(foamTexture, 0.55, saturate(dist / 900.0));
    // Break the accumulated whitecap field into crest streaks and drop the thin
    // veil below a threshold, so even heavy seas read as dark water with bright
    // foam accents rather than a solid sheet (which a high, bright sun blows
    // out). The surviving foam keeps full contrast.
    float coverage = foamAcc * gFoamAmount * (0.30 + 0.85 * foamTexture);
    float foam = saturate((coverage - 0.32) * 2.4);
    foam = foam * foam * (3.0 - 2.0 * foam);
    foam = saturate(foam + impFoam * (0.4 + 0.6 * foamTexture)
                         + whFoam * (0.6 + 0.4 * foamTexture)); // whirl foam carries its own break-up

    // --- Roughness: base + fading detail cascades add variance + foam ---
    float detailLoss = (1.0 - i.fades.y) * gCascade1.z + (1.0 - i.fades.z) * gCascade2.z;
    float rough = saturate(gRoughBase + detailLoss + gDistRough * saturate(dist / 2500.0) + foam * 0.35);
    // Widen the microfacet lobe by the filtered-out slope variance
    // (alpha^2 ~ 2 sigma^2 for GGX). This is where sub-pixel wave detail goes
    // instead of sparkling: the distant sea turns satin under the sun path.
    float alpha = max(rough * rough, 0.0008);
    alpha = sqrt(alpha * alpha + 2.0 * sigma2);
    float roughEff = sqrt(alpha);

    // --- Sky reflection (pre-blurred + clamped against aureole fireflies) ---
    float3 R = reflect(E, N);
    R.y = abs(R.y) + 0.02;
    R = normalize(R);
    float mip = 1.0 + roughEff * 4.5;
    float3 env = tSky.SampleLevel(samLinearClamp, R, mip).rgb;
    env = min(env, 8.0);

    // --- Fresnel ---
    // Uses the macro roughness only: filtered-out micro detail scatters the
    // reflection (wider lobe) but does not destroy grazing reflectivity.
    float f0 = 0.02;
    float fres = f0 + (1.0 - f0) * pow(1.0 - NdV, 5.0);
    fres = saturate(fres * (1.0 - 0.6 * rough) + 0.35 * rough * f0);

    // --- Sun / moon GGX specular ---
    float3 L = gLightDir;
    float NdL = saturate(dot(N, L));
    float3 H = normalize(V + L);
    float NdH = saturate(dot(N, H));
    float VdH = saturate(dot(V, H));
    float Fs = f0 + (1.0 - f0) * pow(1.0 - VdH, 5.0);
    float3 spec = D_GGX(NdH, alpha) * V_SmithApprox(NdL, NdV, alpha) * Fs * NdL * gLightColor;
    spec = min(spec, 60.0); // tame residual fireflies (filtering does the real work)
    // Sun-aligned frame and its footprint, in uniform flow (derivatives).
    float2 sunAz = normalize(gLightDir.xz + float2(0.0, 1e-5));
    float2 sunUV = float2(dot(i.worldXZ, sunAz), dot(i.worldXZ, float2(-sunAz.y, sunAz.x)));
    float2 sunFp = float2(length(float2(ddx(sunUV.x), ddy(sunUV.x))),
                        length(float2(ddx(sunUV.y), ddy(sunUV.y))));
    [branch] if (gSunFx.x > 0.0)
    {
        // A mirror facet cannot outshine the disc it reflects: bound each
        // glint by F x disc radiance. The reddened horizon sun is dim, so its
        // glitter stays the sun's own orange instead of clipping to yellow.
        spec = min(spec, Fs * gSunDiscColor * 1.6);
        [branch] if (spec.r > 0.01) // only glints bright enough to see twinkle
            spec *= lerp(1.0, GlitterShimmer(sunUV, sunFp, gTime), 0.75 * gSunFx.x);
    }

    // --- Water body: ambient-lit deep color + subsurface scattering ---
    float3 ambient = tSky.SampleLevel(samLinearClamp, float3(0, 1, 0), 4.0).rgb;
    float ampEst = max(gCascade0.w, 0.05);
    float heightN = pow(saturate(i.dispY / ampEst * 0.6 + 0.05), 1.6); // crests only
    float towardLight = pow(saturate(dot(E, L)), 3.0);
    float sss = gSSS * heightN * (0.10 + 0.90 * towardLight) * (0.3 + 0.7 * pow(1.0 - NdV, 2.0));
    float3 body = gWaterDeep * (ambient + 0.25 * gLightColor * NdL)
                + sss * gWaterScatter * gLightColor;

    float3 col = lerp(body, env, fres) + spec;

    // --- Foam ---
    // Foam is a bright, near-white diffuse surface. Under a high, intense sun
    // (e.g. noon) its irradiance is large on every whitecap at once, so a
    // heavily-foamed sea (high sea states) would otherwise blow out to a
    // featureless, over-bloomed white sheet. Softly roll the foam irradiance
    // off toward a white point (luminance-preserving): dim foam at low sun is
    // left essentially untouched, while bright foam is pulled back into the
    // responsive part of the tonemap so the whitecaps keep their wave shape.
    float3 foamAmb = ambient;
    [branch] if (gSunFx.x > 0.0)
    {
        // A horizon sun lights the sky from the side: the bright glow over the
        // sunset falls on foam facing it, only the dim zenith on the rest. So
        // light the foam by the sky around its own facing, not the zenith alone.
        float3 nAmb = tSky.SampleLevel(samLinearClamp, normalize(float3(N.x, max(N.y, 0.05), N.z)), 5.0).rgb;
        foamAmb = lerp(ambient, 0.5 * (ambient + nAmb), gSunFx.x);
    }
    float3 foamIrr = NdL * gLightColor * (1.0 / PI) + foamAmb;
    const float foamWhite = 3.0; // irradiance roll-off white point
    float foamLum = dot(foamIrr, float3(0.2126, 0.7152, 0.0722));
    float3 foamCol = 0.75 * foamIrr / (1.0 + foamLum / foamWhite);
    col = lerp(col, foamCol, foam);

    // --- Buoy lamp: small point light on nearby water ---
    if (gBuoyLightOn > 0.0)
    {
        float3 toL = gBuoyLightPos - i.rel;
        float ld = length(toL);
        toL /= max(ld, 1e-3);
        float atten = gBuoyLightOn / (1.0 + 0.4 * ld + 0.20 * ld * ld);
        float ndl = saturate(dot(N, toL));
        float3 hb = normalize(V + toL);
        float sp = pow(saturate(dot(N, hb)), 90.0) * 2.5;
        col += gBuoyLightColor * atten * (ndl * (0.05 + 0.5 * foam) + sp);
    }

    // --- Aerial perspective toward the horizon ---
    float3 horizonCol = tSky.SampleLevel(samLinearClamp, normalize(float3(E.x, 0.015, E.z)), 1.5).rgb;
    float fog = 1.0 - exp(-dist * gFogDensity);
    col = lerp(col, horizonCol, fog);

    // Whirlpool drain mouth, after fog so its core stays absolutely black.
    col *= 1.0 - WhirlDrainDarkness(i.worldXZ);

    return float4(col, 1.0);
}
