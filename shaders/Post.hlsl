// Post-processing: bloom chain, ACES tonemap, FXAA.
// Note: deliberately does NOT include Common.hlsli — post passes rebind b0
// with their own small constant buffer.

SamplerState samLinearWrap  : register(s0);
SamplerState samLinearClamp : register(s1);
SamplerState samPoint       : register(s2);

cbuffer PostCB : register(b0)
{
    float2 gTexel;   // 1/size of the SOURCE texture
    float2 gParamA;  // pass-specific
    float4 gParamB;  // pass-specific
}

Texture2D<float4> tSrc0 : register(t0);
Texture2D<float4> tSrc1 : register(t1);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSFull(uint vid : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
    o.uv = uv;
    return o;
}

// ---------------------------------------------------------------------------
// Bloom
// ---------------------------------------------------------------------------
float3 SampleBox(Texture2D<float4> t, float2 uv, float scale)
{
    float2 o = gTexel * scale;
    float3 c = 0;
    c += t.SampleLevel(samLinearClamp, uv + float2(-o.x, -o.y), 0).rgb;
    c += t.SampleLevel(samLinearClamp, uv + float2(o.x, -o.y), 0).rgb;
    c += t.SampleLevel(samLinearClamp, uv + float2(-o.x, o.y), 0).rgb;
    c += t.SampleLevel(samLinearClamp, uv + float2(o.x, o.y), 0).rgb;
    return c * 0.25;
}

// gParamA.x = threshold, gParamA.y = soft knee
float4 PSBloomPrefilter(VSOut i) : SV_Target
{
    float3 c = SampleBox(tSrc0, i.uv, 1.0);
    float thr = gParamA.x;
    float knee = max(gParamA.y, 1e-4);
    float lum = max(c.r, max(c.g, c.b));
    float soft = clamp(lum - thr + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float w = max(soft, lum - thr) / max(lum, 1e-4);
    // suppress fireflies
    c *= 1.0 / (1.0 + lum * 0.15);
    return float4(c * w, 1.0);
}

float4 PSBloomDown(VSOut i) : SV_Target
{
    return float4(SampleBox(tSrc0, i.uv, 1.0), 1.0);
}

// tSrc0 = smaller mip being upsampled, tSrc1 = same-size downsample chain tex
float4 PSBloomUp(VSOut i) : SV_Target
{
    float2 o = gTexel;
    float3 c = 0;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv + float2(-o.x, -o.y), 0).rgb;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv + float2(o.x, -o.y), 0).rgb;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv + float2(-o.x, o.y), 0).rgb;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv + float2(o.x, o.y), 0).rgb;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv, 0).rgb * 4.0;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv + float2(-o.x, 0), 0).rgb * 2.0;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv + float2(o.x, 0), 0).rgb * 2.0;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv + float2(0, -o.y), 0).rgb * 2.0;
    c += tSrc0.SampleLevel(samLinearClamp, i.uv + float2(0, o.y), 0).rgb * 2.0;
    c /= 16.0;
    return float4(c + tSrc1.SampleLevel(samLinearClamp, i.uv, 0).rgb, 1.0);
}

// ---------------------------------------------------------------------------
// Tonemap: tSrc0 = HDR scene, tSrc1 = bloom.
// gParamA.x = exposure, gParamA.y = bloom intensity, gParamB.x = vignette
// ---------------------------------------------------------------------------
float3 ACESFilmT(float3 x)
{
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSTonemap(VSOut i) : SV_Target
{
    float3 hdr = tSrc0.SampleLevel(samPoint, i.uv, 0).rgb;
    float3 bloom = tSrc1.SampleLevel(samLinearClamp, i.uv, 0).rgb;
    float3 c = (hdr + bloom * gParamA.y) * gParamA.x;

    // subtle vignette
    float2 d = i.uv - 0.5;
    c *= 1.0 - gParamB.x * dot(d, d) * 1.6;

    c = ACESFilmT(c);
    c = pow(abs(c), 1.0 / 2.2);
    return float4(c, 1.0);
}

// ---------------------------------------------------------------------------
// FXAA (compact quality version). tSrc0 = LDR (gamma) image.
// ---------------------------------------------------------------------------
float FxaaLuma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

float4 PSFxaa(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float3 rgbM = tSrc0.SampleLevel(samLinearClamp, uv, 0).rgb;
    float lumaM = FxaaLuma(rgbM);
    float lumaNW = FxaaLuma(tSrc0.SampleLevel(samLinearClamp, uv + float2(-gTexel.x, -gTexel.y), 0).rgb);
    float lumaNE = FxaaLuma(tSrc0.SampleLevel(samLinearClamp, uv + float2(gTexel.x, -gTexel.y), 0).rgb);
    float lumaSW = FxaaLuma(tSrc0.SampleLevel(samLinearClamp, uv + float2(-gTexel.x, gTexel.y), 0).rgb);
    float lumaSE = FxaaLuma(tSrc0.SampleLevel(samLinearClamp, uv + float2(gTexel.x, gTexel.y), 0).rgb);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    float range = lumaMax - lumaMin;
    if (range < max(0.0312, lumaMax * 0.125))
        return float4(rgbM, 1.0);

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * 0.25, 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * gTexel;

    float3 rgbA = 0.5 * (
        tSrc0.SampleLevel(samLinearClamp, uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
        tSrc0.SampleLevel(samLinearClamp, uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        tSrc0.SampleLevel(samLinearClamp, uv + dir * -0.5, 0).rgb +
        tSrc0.SampleLevel(samLinearClamp, uv + dir * 0.5, 0).rgb);
    float lumaB = FxaaLuma(rgbB);
    if (lumaB < lumaMin || lumaB > lumaMax)
        return float4(rgbA, 1.0);
    return float4(rgbB, 1.0);
}

// Plain copy (FXAA disabled path).
float4 PSCopy(VSOut i) : SV_Target
{
    return float4(tSrc0.SampleLevel(samPoint, i.uv, 0).rgb, 1.0);
}
