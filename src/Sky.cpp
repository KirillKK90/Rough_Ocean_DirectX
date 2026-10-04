#include "Sky.h"

#include <cstring>

using namespace DirectX;

namespace
{
    // Mirrors SkyCB in Sky.hlsl.
    struct SkyCB
    {
        XMFLOAT3 sunDir; float sunI;
        XMFLOAT3 moonDir; float moonI;
        float cloudCover; float res; uint32_t mipSrc; float pad;
        float haze; float ozone; float cloudSunlit; float pad2;
        float aloft; float multiScatter; float pad3[2];
    };
}

void Sky::Create(GpuContext& ctx, uint32_t resolution)
{
    res = resolution;
    mips = 1;
    while ((res >> mips) >= 4)
        ++mips;

    cube = ctx.CreateTexture2D(res, res, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        static_cast<uint16_t>(mips), 6, L"SkyCube");
    inSrvState = false;
    dirty = true;

    // Cube SRV, shared by sky/ocean/buoy passes.
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    sd.TextureCube.MipLevels = mips;
    ctx.Dev()->CreateShaderResourceView(cube.Get(), &sd, ctx.SrvCpu(DescSlot::SkyDraw));
    ctx.Dev()->CreateShaderResourceView(cube.Get(), &sd, ctx.SrvCpu(DescSlot::BuoyDraw));
    ctx.Dev()->CreateShaderResourceView(cube.Get(), &sd, ctx.SrvCpu(DescSlot::OceanDraw + 9));

    // Mip-generation views: per level, source SRV (as 2D array) + dest UAV.
    for (uint32_t m = 1; m < mips; ++m)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC ms = {};
        ms.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ms.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ms.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        ms.Texture2DArray.MostDetailedMip = m - 1;
        ms.Texture2DArray.MipLevels = 1;
        ms.Texture2DArray.ArraySize = 6;
        ctx.Dev()->CreateShaderResourceView(cube.Get(), &ms, ctx.SrvCpu(DescSlot::SkyMipSrv + m));

        D3D12_UNORDERED_ACCESS_VIEW_DESC mu = {};
        mu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        mu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        mu.Texture2DArray.MipSlice = m;
        mu.Texture2DArray.ArraySize = 6;
        ctx.Dev()->CreateUnorderedAccessView(cube.Get(), nullptr, &mu, ctx.SrvCpu(DescSlot::SkyMipUav + m));
    }
    // Mip 0 UAV for generation.
    D3D12_UNORDERED_ACCESS_VIEW_DESC u0 = {};
    u0.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    u0.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    u0.Texture2DArray.MipSlice = 0;
    u0.Texture2DArray.ArraySize = 6;
    ctx.Dev()->CreateUnorderedAccessView(cube.Get(), nullptr, &u0, ctx.SrvCpu(DescSlot::SkyGenUav));

    // Multiple-scattering LUT. Each view goes into both slots of its pair, so
    // a table bound at the pair's start reaches it as register 3.
    msLut = ctx.CreateTexture2D(kMsLutSize, kMsLutSize, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        1, 1, L"SkyMsLut");
    msInSrvState = false;
    for (uint32_t k = 0; k < 2; ++k)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC lu = {};
        lu.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        lu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ctx.Dev()->CreateUnorderedAccessView(msLut.Get(), nullptr, &lu, ctx.SrvCpu(DescSlot::SkyMsUav + k));

        D3D12_SHADER_RESOURCE_VIEW_DESC ls = {};
        ls.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ls.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ls.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        ls.Texture2D.MipLevels = 1;
        ctx.Dev()->CreateShaderResourceView(msLut.Get(), &ls, ctx.SrvCpu(DescSlot::SkyMsSrv + k));
    }

    if (!psoGen)
    {
        std::wstring file = ctx.ShaderPath(L"Sky.hlsl");
        psoGen = ctx.CreateComputePso(ctx.CompileShader(file, "CSSkyGen", "cs_5_0").Get(), L"SkyGen");
        psoMip = ctx.CreateComputePso(ctx.CompileShader(file, "CSSkyMip", "cs_5_0").Get(), L"SkyMip");
        psoMs = ctx.CreateComputePso(ctx.CompileShader(file, "CSMsLut", "cs_5_0").Get(), L"SkyMsLut");

        ComPtr<ID3DBlob> vs = ctx.CompileShader(file, "VSSky", "vs_5_0");
        ComPtr<ID3DBlob> ps = ctx.CompileShader(file, "PSSky", "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = ctx.DefaultPsoDesc();
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        HR(ctx.Dev()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoDraw)));
        psoDraw->SetName(L"SkyDraw");
    }
}

void Sky::RecordGenerate(GpuContext& ctx, const Params& params)
{
    if (!dirty)
        return;
    dirty = false;

    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    if (inSrvState)
        ctx.Transition(cube.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inSrvState = false;

    SkyCB cb = {};
    cb.sunDir = params.sunDir;
    cb.sunI = params.sunIntensity;
    cb.moonDir = params.moonDir;
    cb.moonI = params.moonIntensity;
    cb.cloudCover = params.cloudCover;
    cb.res = static_cast<float>(res);
    cb.haze = params.haze;
    cb.ozone = params.ozone;
    cb.cloudSunlit = params.cloudSunlit;
    cb.aloft = params.aloft;
    cb.multiScatter = params.multiScatter;

    cmd->SetComputeRootSignature(ctx.computeRS.Get());
    void* p = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS va = ctx.AllocUpload(sizeof(SkyCB), &p);
    memcpy(p, &cb, sizeof(SkyCB));
    cmd->SetComputeRootConstantBufferView(0, va);

    // The multiple-scattering LUT depends only on the atmosphere, not on the
    // view: rebuild it first, then the cube march samples it.
    if (params.multiScatter > 0.0f)
    {
        if (msInSrvState)
            ctx.Transition(msLut.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(psoMs.Get());
        cmd->SetComputeRootDescriptorTable(6, ctx.SrvGpu(DescSlot::SkyMsUav));
        cmd->Dispatch(kMsLutSize, kMsLutSize, 1);
        ctx.Transition(msLut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        msInSrvState = true;
    }

    cmd->SetPipelineState(psoGen.Get());
    if (msInSrvState)
        cmd->SetComputeRootDescriptorTable(5, ctx.SrvGpu(DescSlot::SkyMsSrv));
    cmd->SetComputeRootDescriptorTable(6, ctx.SrvGpu(DescSlot::SkyGenUav));
    cmd->Dispatch((res + 7) / 8, (res + 7) / 8, 6);

    // Mip chain: box-filter each level from the previous one.
    cmd->SetPipelineState(psoMip.Get());
    for (uint32_t m = 1; m < mips; ++m)
    {
        // The source mip (m-1, all 6 faces) moves UAV -> SRV.
        D3D12_RESOURCE_BARRIER barriers[6];
        for (uint32_t f = 0; f < 6; ++f)
        {
            barriers[f] = {};
            barriers[f].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[f].Transition.pResource = cube.Get();
            barriers[f].Transition.Subresource = (m - 1) + f * mips;
            barriers[f].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[f].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        cmd->ResourceBarrier(6, barriers);

        uint32_t mres = std::max(res >> m, 1u);
        SkyCB mcb = cb;
        mcb.res = static_cast<float>(mres);
        D3D12_GPU_VIRTUAL_ADDRESS mva = ctx.AllocUpload(sizeof(SkyCB), &p);
        memcpy(p, &mcb, sizeof(SkyCB));
        cmd->SetComputeRootConstantBufferView(0, mva);
        cmd->SetComputeRootDescriptorTable(5, ctx.SrvGpu(DescSlot::SkyMipSrv + m));
        cmd->SetComputeRootDescriptorTable(6, ctx.SrvGpu(DescSlot::SkyMipUav + m));
        cmd->Dispatch((mres + 7) / 8, (mres + 7) / 8, 6);
    }

    // Everything to PIXEL_SHADER_RESOURCE for sampling: mips 0..mips-2 are in
    // NON_PIXEL, the last mip is still UAV.
    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(size_t(mips) * 6);
        for (uint32_t f = 0; f < 6; ++f)
        {
            for (uint32_t m = 0; m < mips; ++m)
            {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = cube.Get();
                b.Transition.Subresource = m + f * mips;
                b.Transition.StateBefore = (m == mips - 1)
                    ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barriers.push_back(b);
            }
        }
        cmd->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }
    inSrvState = true;
}

void Sky::Draw(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB)
{
    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    cmd->SetGraphicsRootSignature(ctx.graphicsRS.Get());
    cmd->SetPipelineState(psoDraw.Get());
    cmd->SetGraphicsRootConstantBufferView(0, frameCB);
    cmd->SetGraphicsRootConstantBufferView(1, frameCB);
    cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvGpu(DescSlot::SkyDraw));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
