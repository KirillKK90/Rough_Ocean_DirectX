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
};

float3 CascadeFades(float dist)
{
    return float3(
        exp(-dist / gCascade0.y),
        exp(-dist / gCascade1.y),
        exp(-dist / gCascade2.y));
}

VSOut VSOcean(VSIn v)
{
    VSOut o;
    float2 worldXZ = gCamPos.xz + v.off;
    float dist = length(v.off);
    float3 fades = CascadeFades(dist);

    float3 disp = 0;
    disp += fades.x * tDisp0.SampleLevel(samLinearWrap, worldXZ * gCascade0.x, 0).xyz;
    disp += fades.y * tDisp1.SampleLevel(samLinearWrap, worldXZ * gCascade1.x, 0).xyz;
    disp += fades.z * tDisp2.SampleLevel(samLinearWrap, worldXZ * gCascade2.x, 0).xyz;
    disp.xz *= gLambda;

    // Meteorite impact rings superimpose linearly on the wind sea.
    float3 impDisp;
    float2 impSlope;
    float impFoam;
    ImpactWaves(worldXZ, impDisp, impSlope, impFoam);
    disp += impDisp;

    float3 rel = float3(v.off.x + disp.x, disp.y - gCamPos.y, v.off.y + disp.z);
    o.pos = mul(float4(rel, 1.0), gViewProj);
    o.rel = rel;
    o.worldXZ = worldXZ + disp.xz;
    o.fades = fades;
    o.dispY = disp.y;
    return o;
}

float4 PSOcean(VSOut i) : SV_Target
{
    float dist = length(i.rel);
    float3 E = i.rel / dist;   // view ray (from eye)
    float3 V = -E;

    // --- Normal from analytic derivatives, cascade-faded ---
    float4 d0 = tDeriv0.Sample(samLinearWrap, i.worldXZ * gCascade0.x);
    float4 d1 = tDeriv1.Sample(samLinearWrap, i.worldXZ * gCascade1.x);
    float4 d2 = tDeriv2.Sample(samLinearWrap, i.worldXZ * gCascade2.x);
    float4 d = d0 * i.fades.x + d1 * i.fades.y + d2 * i.fades.z;

    float2 slope = float2(d.x / max(1.0 + gLambda * d.z, 0.15),
                          d.y / max(1.0 + gLambda * d.w, 0.15));

    // Meteorite impact rings: analytic slopes give crisp per-pixel normals.
    float3 impDisp;
    float2 impSlope;
    float impFoam;
    ImpactWaves(i.worldXZ, impDisp, impSlope, impFoam);
    slope += impSlope;

    float3 N = normalize(float3(-slope.x, 1.0, -slope.y));
    // keep normals from tipping past vertical toward the eye
    if (dot(N, V) < 0.0)
        N = normalize(N - 1.9 * dot(N, V) * V);

    float NdV = max(dot(N, V), 1e-3);

    // --- Foam coverage ---
    float foamAcc = tFoam0.Sample(samLinearWrap, i.worldXZ * gCascade0.x) * i.fades.x
                  + tFoam1.Sample(samLinearWrap, i.worldXZ * gCascade1.x) * i.fades.y * 0.75;
    float foamTexture = Fbm(i.worldXZ * 0.9 + float2(0.07, 0.05) * gTime, 3);
    float foam = saturate(foamAcc * gFoamAmount * (0.55 + 0.9 * foamTexture));
    foam = foam * foam * (3.0 - 2.0 * foam);
    foam = saturate(foam + impFoam * (0.4 + 0.6 * foamTexture));

    // --- Roughness: base + fading detail cascades add variance + foam ---
    float detailLoss = (1.0 - i.fades.y) * gCascade1.z + (1.0 - i.fades.z) * gCascade2.z;
    float rough = saturate(gRoughBase + detailLoss + gDistRough * saturate(dist / 2500.0) + foam * 0.35);

    // --- Sky reflection (pre-blurred + clamped against aureole fireflies) ---
    float3 R = reflect(E, N);
    R.y = abs(R.y) + 0.02;
    R = normalize(R);
    float mip = 1.0 + rough * 4.5;
    float3 env = tSky.SampleLevel(samLinearClamp, R, mip).rgb;
    env = min(env, 8.0);

    // --- Fresnel ---
    float f0 = 0.02;
    float fres = f0 + (1.0 - f0) * pow(1.0 - NdV, 5.0);
    fres = saturate(fres * (1.0 - 0.6 * rough) + 0.35 * rough * f0);

    // --- Sun / moon GGX specular ---
    float3 L = gLightDir;
    float NdL = saturate(dot(N, L));
    float3 H = normalize(V + L);
    float NdH = saturate(dot(N, H));
    float VdH = saturate(dot(V, H));
    float a = max(rough * rough, 0.0008);
    float Fs = f0 + (1.0 - f0) * pow(1.0 - VdH, 5.0);
    float3 spec = D_GGX(NdH, a) * V_SmithApprox(NdL, NdV, a) * Fs * NdL * gLightColor;
    spec = min(spec, 30.0); // tame fireflies

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
    float3 foamCol = 0.75 * (NdL * gLightColor * (1.0 / PI) + ambient);
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

    return float4(col, 1.0);
}
