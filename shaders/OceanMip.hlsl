// 2x2 box downsample for the ocean cascade mip chains.
//
// The FFT cascade maps used to be single-mip: any pixel whose footprint
// covered more than one texel (all of the mid and far field) undersampled the
// wave slopes and turned them into glinting per-pixel noise. Each map now
// carries a full mip pyramid, rebuilt every simulation step by this pass, and
// the draw shaders sample it with anisotropic filtering - a low-pass over
// exactly the frequencies a pixel cannot resolve.
//
// Compiled twice: MIP_TYPE=float4 for displacement/derivatives (RGBA16F) and
// MIP_TYPE=float for foam (R32F), so the declared UAV type matches the view.

cbuffer MipCB : register(b0)
{
    uint gDstSize; // destination mip edge length, texels
    uint3 gPad;
}

#ifndef MIP_TYPE
#define MIP_TYPE float4
#endif

Texture2D<MIP_TYPE>   MipSrc : register(t2); // mip m-1
RWTexture2D<MIP_TYPE> MipDst : register(u2); // mip m

[numthreads(8, 8, 1)]
void CSMip(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDstSize || id.y >= gDstSize)
        return;
    uint2 s = id.xy * 2;
    MIP_TYPE v = MipSrc[s] + MipSrc[s + uint2(1, 0)]
               + MipSrc[s + uint2(0, 1)] + MipSrc[s + uint2(1, 1)];
    MipDst[id.xy] = v * 0.25;
}
