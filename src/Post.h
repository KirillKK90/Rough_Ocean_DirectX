#pragma once

#include "GpuContext.h"

// HDR post chain: bloom (threshold + down/up pyramid), ACES tonemap, FXAA.
class Post
{
public:
    static constexpr uint32_t kBloomMips = 5;

    void Create(GpuContext& ctx, uint32_t width, uint32_t height);

    // Scene HDR target for the forward passes.
    D3D12_CPU_DESCRIPTOR_HANDLE HdrRtv(GpuContext& ctx) const { return ctx.RtvCpu(RtvSlot::SceneHdr); }
    ID3D12Resource* HdrTexture() const { return hdr.Get(); }

    // Runs bloom + tonemap (+ FXAA) and writes the final image into the
    // given backbuffer RTV. Expects the HDR texture in RENDER_TARGET state;
    // leaves it in RENDER_TARGET state for the next frame. dither is the
    // output noise amplitude in 8-bit steps (0 = none).
    void Record(GpuContext& ctx, uint32_t backbufferRtvSlot, bool fxaaOn,
                float exposure, float bloomIntensity, float bloomThreshold, float vignette,
                float dither);

    float exposureMul = 1.0f;

private:
    void DrawFullscreen(GpuContext& ctx, ID3D12PipelineState* pso,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h,
        uint32_t srvSlot, const float* params /* 8 floats */);

    ComPtr<ID3D12Resource> hdr, ldr;
    ComPtr<ID3D12Resource> bloomA[kBloomMips], bloomB[kBloomMips - 1];
    uint32_t bloomW[kBloomMips] = {}, bloomH[kBloomMips] = {};
    uint32_t width = 0, height = 0;

    ComPtr<ID3D12PipelineState> psoPrefilter, psoDown, psoUp, psoTonemap, psoFxaa, psoCopy;
};
