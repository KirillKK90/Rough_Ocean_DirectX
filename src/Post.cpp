#include "Post.h"

#include <algorithm>
#include <cstring>

void Post::Create(GpuContext& ctx, uint32_t w, uint32_t h)
{
    width = w;
    height = h;
    const float clearBlack[4] = { 0, 0, 0, 1 };

    hdr = ctx.CreateTexture2D(w, h, GpuContext::kHdrFormat,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
        1, 1, L"SceneHDR", clearBlack);
    ldr = ctx.CreateTexture2D(w, h, GpuContext::kBackbufferFormat,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
        1, 1, L"SceneLDR", clearBlack);
    ctx.Dev()->CreateRenderTargetView(hdr.Get(), nullptr, ctx.RtvCpu(RtvSlot::SceneHdr));
    ctx.Dev()->CreateRenderTargetView(ldr.Get(), nullptr, ctx.RtvCpu(RtvSlot::Ldr));

    uint32_t bw = std::max(w / 2, 8u), bh = std::max(h / 2, 8u);
    for (uint32_t i = 0; i < kBloomMips; ++i)
    {
        bloomW[i] = bw;
        bloomH[i] = bh;
        bloomA[i] = ctx.CreateTexture2D(bw, bh, GpuContext::kHdrFormat,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
            1, 1, L"BloomA", clearBlack);
        ctx.Dev()->CreateRenderTargetView(bloomA[i].Get(), nullptr, ctx.RtvCpu(RtvSlot::BloomA + i));
        if (i < kBloomMips - 1)
        {
            bloomB[i] = ctx.CreateTexture2D(bw, bh, GpuContext::kHdrFormat,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
                1, 1, L"BloomB", clearBlack);
            ctx.Dev()->CreateRenderTargetView(bloomB[i].Get(), nullptr, ctx.RtvCpu(RtvSlot::BloomB + i));
        }
        bw = std::max(bw / 2, 8u);
        bh = std::max(bh / 2, 8u);
    }

    // SRV descriptors at their fixed table slots.
    auto srv2d = [&](ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t slot)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = fmt;
        ctx.Dev()->CreateShaderResourceView(res, &sd, ctx.SrvCpu(slot));
    };
    srv2d(hdr.Get(), GpuContext::kHdrFormat, DescSlot::Tonemap);
    srv2d(bloomB[0].Get(), GpuContext::kHdrFormat, DescSlot::Tonemap + 1);
    srv2d(ldr.Get(), GpuContext::kBackbufferFormat, DescSlot::Fxaa);
    srv2d(hdr.Get(), GpuContext::kHdrFormat, DescSlot::BloomPre);
    for (uint32_t i = 1; i < kBloomMips; ++i)
        srv2d(bloomA[i - 1].Get(), GpuContext::kHdrFormat, DescSlot::BloomDown + i);
    for (uint32_t i = 0; i + 1 < kBloomMips; ++i)
    {
        ID3D12Resource* srcSmall = (i == kBloomMips - 2) ? bloomA[kBloomMips - 1].Get() : bloomB[i + 1].Get();
        srv2d(srcSmall, GpuContext::kHdrFormat, DescSlot::BloomUp + 2 * i);
        srv2d(bloomA[i].Get(), GpuContext::kHdrFormat, DescSlot::BloomUp + 2 * i + 1);
    }

    if (!psoTonemap)
    {
        std::wstring file = ctx.ShaderPath(L"Post.hlsl");
        ComPtr<ID3DBlob> vs = ctx.CompileShader(file, "VSFull", "vs_5_0");

        auto makePso = [&](const char* psEntry, DXGI_FORMAT rtvFmt) -> ComPtr<ID3D12PipelineState>
        {
            ComPtr<ID3DBlob> ps = ctx.CompileShader(file, psEntry, "ps_5_0");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = ctx.DefaultPsoDesc();
            pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            pd.DepthStencilState.DepthEnable = FALSE;
            pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
            pd.RTVFormats[0] = rtvFmt;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            ComPtr<ID3D12PipelineState> pso;
            HR(ctx.Dev()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
            return pso;
        };
        psoPrefilter = makePso("PSBloomPrefilter", GpuContext::kHdrFormat);
        psoDown = makePso("PSBloomDown", GpuContext::kHdrFormat);
        psoUp = makePso("PSBloomUp", GpuContext::kHdrFormat);
        psoTonemap = makePso("PSTonemap", GpuContext::kBackbufferFormat);
        psoFxaa = makePso("PSFxaa", GpuContext::kBackbufferFormat);
        psoCopy = makePso("PSCopy", GpuContext::kBackbufferFormat);
    }
}

void Post::DrawFullscreen(GpuContext& ctx, ID3D12PipelineState* pso,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h,
    uint32_t srvSlot, const float* params)
{
    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    D3D12_VIEWPORT vp = { 0, 0, float(w), float(h), 0, 1 };
    D3D12_RECT sc = { 0, 0, LONG(w), LONG(h) };
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->SetPipelineState(pso);

    void* p = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS va = ctx.AllocUpload(sizeof(float) * 8, &p);
    memcpy(p, params, sizeof(float) * 8);
    cmd->SetGraphicsRootConstantBufferView(0, va);
    cmd->SetGraphicsRootConstantBufferView(1, va);
    cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvGpu(srvSlot));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}

void Post::Record(GpuContext& ctx, uint32_t backbufferRtvSlot, bool fxaaOn,
                  float exposure, float bloomIntensity, float bloomThreshold, float vignette,
                  float dither)
{
    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    cmd->SetGraphicsRootSignature(ctx.graphicsRS.Get());

    ctx.Transition(hdr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Bloom prefilter into A[0].
    float params[8] = { 1.0f / width, 1.0f / height, bloomThreshold, bloomThreshold * 0.6f, 0, 0, 0, 0 };
    DrawFullscreen(ctx, psoPrefilter.Get(), ctx.RtvCpu(RtvSlot::BloomA), bloomW[0], bloomH[0], DescSlot::BloomPre, params);
    ctx.Transition(bloomA[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Downsample chain.
    for (uint32_t i = 1; i < kBloomMips; ++i)
    {
        float dp[8] = { 1.0f / bloomW[i - 1], 1.0f / bloomH[i - 1], 0, 0, 0, 0, 0, 0 };
        DrawFullscreen(ctx, psoDown.Get(), ctx.RtvCpu(RtvSlot::BloomA + i), bloomW[i], bloomH[i], DescSlot::BloomDown + i, dp);
        ctx.Transition(bloomA[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // Upsample + accumulate chain into B[i].
    for (int i = kBloomMips - 2; i >= 0; --i)
    {
        uint32_t srcW = (i == kBloomMips - 2) ? bloomW[kBloomMips - 1] : bloomW[i + 1];
        uint32_t srcH = (i == kBloomMips - 2) ? bloomH[kBloomMips - 1] : bloomH[i + 1];
        float up[8] = { 1.0f / srcW, 1.0f / srcH, 0, 0, 0, 0, 0, 0 };
        DrawFullscreen(ctx, psoUp.Get(), ctx.RtvCpu(RtvSlot::BloomB + i), bloomW[i], bloomH[i], DescSlot::BloomUp + 2 * i, up);
        ctx.Transition(bloomB[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // Tonemap.
    float tm[8] = { 1.0f / width, 1.0f / height, exposure, bloomIntensity, vignette, dither, 0, 0 };
    if (fxaaOn)
    {
        DrawFullscreen(ctx, psoTonemap.Get(), ctx.RtvCpu(RtvSlot::Ldr), width, height, DescSlot::Tonemap, tm);
        ctx.Transition(ldr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        float fx[8] = { 1.0f / width, 1.0f / height, 0, 0, 0, 0, 0, 0 };
        DrawFullscreen(ctx, psoFxaa.Get(), ctx.RtvCpu(backbufferRtvSlot), width, height, DescSlot::Fxaa, fx);
        ctx.Transition(ldr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    else
    {
        DrawFullscreen(ctx, psoTonemap.Get(), ctx.RtvCpu(backbufferRtvSlot), width, height, DescSlot::Tonemap, tm);
    }

    // Restore working textures to RENDER_TARGET for next frame.
    ctx.Transition(hdr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (uint32_t i = 0; i < kBloomMips; ++i)
        ctx.Transition(bloomA[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (uint32_t i = 0; i + 1 < kBloomMips; ++i)
        ctx.Transition(bloomB[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}
