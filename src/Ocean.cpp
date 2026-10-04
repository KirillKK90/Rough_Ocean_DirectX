#include "Ocean.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace DirectX;

namespace
{
    constexpr float kTwoPi = 6.28318530718f;

    // Mirrors SimCB in OceanSim.hlsl. gFFTDir must stay the 18th 32-bit field
    // (the FFT self-test fills the CB by raw offset).
    struct SimCB
    {
        uint32_t N; float L; float simTime; float dt;
        XMFLOAT2 windDir; float U10; float fetch;
        float amp; float spreadExp; float minK; float maxK;
        float chop; float foamBias; float foamDecay; float foamAdd;
        uint32_t seed; uint32_t fftDir; float smallCut; float swellAmp;
        XMFLOAT2 swellDir; float swellK; float swellSpread;
    };

    // Mirrors MipCB in OceanMip.hlsl.
    struct MipCB
    {
        uint32_t dstSize; uint32_t pad[3];
    };

    D3D12_GPU_VIRTUAL_ADDRESS UploadCB(GpuContext& ctx, const SimCB& data)
    {
        void* p = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS va = ctx.AllocUpload(sizeof(SimCB), &p);
        memcpy(p, &data, sizeof(SimCB));
        return va;
    }

    constexpr D3D12_RESOURCE_STATES kMapSrvState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

void Ocean::Create(GpuContext& ctx, const OceanQuality& quality)
{
    fftN = quality.fftN;
    numCascades = quality.cascades;
    for (uint32_t c = 0; c < kMaxCascades; ++c)
    {
        mapsEverSimulated[c] = false;
        buffersInSrvState[c] = false;
        h0Initialized[c] = false;
    }
    framesRecorded = 0;
    zoomFanLevel = zoomRingLevel = -1; // the mesh is rebuilt below

    // Cascade patch sizes and spectral band splits (in wavelength, meters).
    if (numCascades >= 3)
    {
        casc[0].L = 487.0f; casc[1].L = 71.0f; casc[2].L = 11.3f;
        casc[0].minK = 1e-4f;           casc[0].maxK = kTwoPi / 20.0f;
        casc[1].minK = kTwoPi / 20.0f;  casc[1].maxK = kTwoPi / 2.8f;
        casc[2].minK = kTwoPi / 2.8f;   casc[2].maxK = 1e9f;
    }
    else
    {
        casc[0].L = 430.0f; casc[1].L = 31.0f; casc[2].L = 11.3f;
        casc[0].minK = 1e-4f;           casc[0].maxK = kTwoPi / 10.0f;
        casc[1].minK = kTwoPi / 10.0f;  casc[1].maxK = 1e9f;
        casc[2].minK = 1.0f;            casc[2].maxK = 0.0f; // empty band
    }
    for (uint32_t c = 0; c < kMaxCascades; ++c)
    {
        float nyquist = 0.9f * XM_PI * float(fftN) / casc[c].L;
        casc[c].maxK = std::min(casc[c].maxK, nyquist);
    }

    // Resources. The displacement/derivative/foam maps carry full mip chains,
    // rebuilt each simulation step: sampled with aniso filtering at draw time
    // they low-pass exactly the wave detail a pixel cannot resolve, which is
    // what keeps the mid/far sea from shimmering with aliased glints.
    mipLevels = 1;
    while ((fftN >> mipLevels) > 0)
        ++mipLevels;
    const uint64_t bufBytes = uint64_t(fftN) * fftN * 16;
    for (uint32_t c = 0; c < kMaxCascades; ++c)
    {
        Cascade& C = casc[c];
        C.h0 = ctx.CreateBuffer(bufBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"OceanH0");
        C.buf0 = ctx.CreateBuffer(bufBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"OceanBuf0");
        C.buf1 = ctx.CreateBuffer(bufBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"OceanBuf1");
        C.disp = ctx.CreateTexture2D(fftN, fftN, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            uint16_t(mipLevels), 1, L"OceanDisp");
        C.deriv = ctx.CreateTexture2D(fftN, fftN, DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            uint16_t(mipLevels), 1, L"OceanDeriv");
        C.foam = ctx.CreateTexture2D(fftN, fftN, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            uint16_t(mipLevels), 1, L"OceanFoam");
    }

    // Descriptors: draw SRVs (full mip chain) and assemble UAVs (mip 0).
    for (uint32_t c = 0; c < kMaxCascades; ++c)
    {
        Cascade& C = casc[c];
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = UINT(-1);
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ctx.Dev()->CreateShaderResourceView(C.disp.Get(), &sd, ctx.SrvCpu(DescSlot::OceanDraw + 3 * c + 0));
        ctx.Dev()->CreateShaderResourceView(C.deriv.Get(), &sd, ctx.SrvCpu(DescSlot::OceanDraw + 3 * c + 1));
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        ctx.Dev()->CreateShaderResourceView(C.foam.Get(), &sd, ctx.SrvCpu(DescSlot::OceanDraw + 3 * c + 2));

        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ctx.Dev()->CreateUnorderedAccessView(C.disp.Get(), nullptr, &ud, ctx.SrvCpu(DescSlot::AssembleUav + 4 * c + 0));
        ctx.Dev()->CreateUnorderedAccessView(C.deriv.Get(), nullptr, &ud, ctx.SrvCpu(DescSlot::AssembleUav + 4 * c + 1));
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ctx.Dev()->CreateUnorderedAccessView(C.foam.Get(), nullptr, &ud, ctx.SrvCpu(DescSlot::AssembleUav + 4 * c + 2));

        // Mip generation views: per map and mip m, SRV of m-1 and UAV of m.
        ID3D12Resource* maps[3] = { C.disp.Get(), C.deriv.Get(), C.foam.Get() };
        for (uint32_t t = 0; t < 3; ++t)
        {
            DXGI_FORMAT fmt = (t == 2) ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
            for (uint32_t m = 1; m < mipLevels; ++m)
            {
                uint32_t idx = (c * 3 + t) * 16 + (m - 1);
                D3D12_SHADER_RESOURCE_VIEW_DESC ms = {};
                ms.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                ms.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                ms.Format = fmt;
                ms.Texture2D.MostDetailedMip = m - 1;
                ms.Texture2D.MipLevels = 1;
                ctx.Dev()->CreateShaderResourceView(maps[t], &ms, ctx.SrvCpu(DescSlot::OceanMipSrv + idx));

                D3D12_UNORDERED_ACCESS_VIEW_DESC mu = {};
                mu.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                mu.Format = fmt;
                mu.Texture2D.MipSlice = m;
                ctx.Dev()->CreateUnorderedAccessView(maps[t], nullptr, &mu, ctx.SrvCpu(DescSlot::OceanMipUav + idx));
            }
        }
    }

    // Readback buffers (displacement of cascades 0..1, per frame in flight).
    rbRowPitch = AlignUp<uint32_t>(fftN * 8, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    for (uint32_t f = 0; f < GpuContext::kFramesInFlight; ++f)
    {
        for (uint32_t c = 0; c < kReadbackCascades; ++c)
        {
            readback[f][c] = ctx.CreateBuffer(uint64_t(rbRowPitch) * fftN, D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"OceanReadback");
            void* p = nullptr;
            D3D12_RANGE all = { 0, uint64_t(rbRowPitch) * fftN };
            HR(readback[f][c]->Map(0, &all, &p));
            rbMapped[f][c] = static_cast<const uint8_t*>(p);
        }
    }

    // Pipelines.
    std::wstring simFile = ctx.ShaderPath(L"OceanSim.hlsl");
    psoInit = ctx.CreateComputePso(ctx.CompileShader(simFile, "CSSpectrumInit", "cs_5_0").Get(), L"OceanInit");
    psoUpdate = ctx.CreateComputePso(ctx.CompileShader(simFile, "CSSpectrumUpdate", "cs_5_0").Get(), L"OceanUpdate");
    psoAssemble = ctx.CreateComputePso(ctx.CompileShader(simFile, "CSAssemble", "cs_5_0").Get(), L"OceanAssemble");

    std::wstring mipFile = ctx.ShaderPath(L"OceanMip.hlsl");
    D3D_SHADER_MACRO mipFoam[] = { {"MIP_TYPE", "float"}, {nullptr, nullptr} };
    psoMip4 = ctx.CreateComputePso(ctx.CompileShader(mipFile, "CSMip", "cs_5_0").Get(), L"OceanMip4");
    psoMipF = ctx.CreateComputePso(ctx.CompileShader(mipFile, "CSMip", "cs_5_0", mipFoam).Get(), L"OceanMipF");

    char nStr[16], logStr[8];
    uint32_t log2N = 0;
    while ((1u << log2N) < fftN)
        ++log2N;
    snprintf(nStr, sizeof(nStr), "%u", fftN);
    snprintf(logStr, sizeof(logStr), "%u", log2N);
    D3D_SHADER_MACRO fftMacros[] = { {"FFT_SIZE", nStr}, {"FFT_LOG2", logStr}, {nullptr, nullptr} };
    psoFFT = ctx.CreateComputePso(ctx.CompileShader(simFile, "CSFFT", "cs_5_0", fftMacros).Get(), L"OceanFFT");

    std::wstring oceanFile = ctx.ShaderPath(L"Ocean.hlsl");
    ComPtr<ID3DBlob> vs = ctx.CompileShader(oceanFile, "VSOcean", "vs_5_0");
    ComPtr<ID3DBlob> ps = ctx.CompileShader(oceanFile, "PSOcean", "ps_5_0");
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = ctx.DefaultPsoDesc();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { layout, 1 };
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    HR(ctx.Dev()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoDraw)));
    psoDraw->SetName(L"OceanDraw");

    BuildMesh(ctx, quality);

    // Simulate every cascade once so all textures have defined (zero for the
    // inactive band) contents before the first frame samples them.
    OceanParams zero;
    zero.ampScale = 0.0f;
    ctx.BeginOneShot();
    for (uint32_t c = 0; c < kMaxCascades; ++c)
        RecordCascadeSim(ctx, c, 0.0f, 0.0f, zero, true);
    ctx.EndOneShot();
}

void Ocean::BuildMesh(GpuContext& ctx, const OceanQuality& quality)
{
    const uint32_t S = quality.sectors;
    const uint32_t R = quality.rings;
    const float r0 = kMeshR0;
    const float rMax = kMeshRMax;
    sectors = S;
    rings = R;
    // Geometric ring growth: radial vertex spacing at distance d is
    // d * ln(rMax/r0)/(R-1). The vertex shader matches its displacement mip
    // to this so far geometry doesn't point-sample fine waves.
    gridScale = std::log(rMax / r0) / float(R - 1);

    std::vector<XMFLOAT2> verts;
    verts.reserve(1 + size_t(S) * R);
    verts.push_back({ 0.0f, 0.0f });
    for (uint32_t i = 0; i < R; ++i)
    {
        float t = float(i) / float(R - 1);
        float r = r0 * std::pow(rMax / r0, t);
        for (uint32_t s = 0; s < S; ++s)
        {
            float a = kTwoPi * float(s) / float(S);
            verts.push_back({ r * std::sin(a), r * std::cos(a) });
        }
    }

    std::vector<uint32_t> idx;
    idx.reserve(size_t(S) * 3 + size_t(R - 1) * S * 6);
    for (uint32_t s = 0; s < S; ++s)
    {
        idx.push_back(0);
        idx.push_back(1 + s);
        idx.push_back(1 + (s + 1) % S);
    }
    for (uint32_t i = 0; i + 1 < R; ++i)
    {
        uint32_t ringA = 1 + i * S;
        uint32_t ringB = 1 + (i + 1) * S;
        for (uint32_t s = 0; s < S; ++s)
        {
            uint32_t sn = (s + 1) % S;
            idx.push_back(ringA + s);
            idx.push_back(ringB + s);
            idx.push_back(ringA + sn);
            idx.push_back(ringA + sn);
            idx.push_back(ringB + s);
            idx.push_back(ringB + sn);
        }
    }
    indexCount = static_cast<uint32_t>(idx.size());

    uint64_t vbSize = verts.size() * sizeof(XMFLOAT2);
    uint64_t ibSize = idx.size() * sizeof(uint32_t);
    vb = ctx.CreateBuffer(vbSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, L"OceanVB");
    ib = ctx.CreateBuffer(ibSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, L"OceanIB");
    ctx.UploadBufferData(vb.Get(), verts.data(), vbSize);
    ctx.UploadBufferData(ib.Get(), idx.data(), ibSize);

    vbv = { vb->GetGPUVirtualAddress(), static_cast<UINT>(vbSize), sizeof(XMFLOAT2) };
    ibv = { ib->GetGPUVirtualAddress(), static_cast<UINT>(ibSize), DXGI_FORMAT_R32_UINT };
}

void Ocean::RecordCascadeSim(GpuContext& ctx, uint32_t c, float simTime, float dt,
                             const OceanParams& params, bool reinit)
{
    Cascade& C = casc[c];
    ID3D12GraphicsCommandList* cmd = ctx.Cmd();

    SimCB cb = {};
    cb.N = fftN;
    cb.L = C.L;
    cb.simTime = simTime;
    cb.dt = dt;
    cb.windDir = params.windDir;
    cb.U10 = std::max(params.U10, 0.1f);
    cb.fetch = std::max(params.fetch, 1000.0f);
    cb.amp = params.ampScale;
    cb.spreadExp = params.spreadExp;
    cb.minK = C.minK;
    cb.maxK = C.maxK;
    cb.chop = params.choppiness;
    cb.foamBias = params.foamBias;
    cb.foamDecay = params.foamDecay;
    cb.foamAdd = params.foamAdd;
    cb.seed = params.seed + c * 7919;
    cb.smallCut = params.smallCut;
    cb.swellDir = params.swellDir;
    cb.swellK = (params.swellLambda > 1.0f) ? kTwoPi / params.swellLambda : 0.0f;
    cb.swellSpread = params.swellSpread;

    if ((reinit || !h0Initialized[c]) && c == 0)
    {
        // Normalize the swell ridge over this cascade's k-grid (mirror of
        // SwellShape in OceanSim.hlsl; band-edge softening is ~1 there). The
        // per-texel amplitude is A / sqrt(sum G^2), so the swell's total
        // energy is independent of patch size and FFT resolution.
        swellAmpTexel = 0.0f;
        if (params.swellAmp > 0.0f && cb.swellK > 0.0f)
        {
            double sum = 0.0;
            for (uint32_t j = 0; j < fftN; ++j)
                for (uint32_t i = 0; i < fftN; ++i)
                {
                    float kx = (float(i) - float(fftN) * 0.5f) * kTwoPi / C.L;
                    float kz = (float(j) - float(fftN) * 0.5f) * kTwoPi / C.L;
                    float klen = std::sqrt(kx * kx + kz * kz);
                    if (klen < C.minK || klen >= C.maxK || klen < 1e-5f)
                        continue;
                    float x = (klen - cb.swellK) / (0.25f * cb.swellK);
                    float d = (kx * params.swellDir.x + kz * params.swellDir.y) / klen;
                    if (d <= 0.0f || x * x > 30.0f)
                        continue;
                    double g = std::exp(double(-x * x)) * std::pow(double(d), double(params.swellSpread));
                    sum += g * g;
                }
            if (sum > 1e-12)
                swellAmpTexel = params.swellAmp / float(std::sqrt(sum));
        }
    }
    cb.swellAmp = swellAmpTexel; // out-of-band for cascades 1/2 anyway

    if (mapsEverSimulated[c])
    {
        ctx.Transition(C.disp.Get(), kMapSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Transition(C.deriv.Get(), kMapSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Transition(C.foam.Get(), kMapSrvState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (buffersInSrvState[c])
    {
        ctx.Transition(C.buf0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ctx.Transition(C.buf1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        buffersInSrvState[c] = false;
    }

    cmd->SetComputeRootSignature(ctx.computeRS.Get());
    const uint32_t groups = (fftN + 7) / 8;

    if (reinit || !h0Initialized[c])
    {
        if (h0Initialized[c])
            ctx.Transition(C.h0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(psoInit.Get());
        cmd->SetComputeRootConstantBufferView(0, UploadCB(ctx, cb));
        cmd->SetComputeRootUnorderedAccessView(3, C.h0->GetGPUVirtualAddress());
        cmd->Dispatch(groups, groups, 1);
        ctx.UavBarrier(C.h0.Get());
        ctx.Transition(C.h0.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        h0Initialized[c] = true;
    }

    // Spectrum update.
    cmd->SetPipelineState(psoUpdate.Get());
    cmd->SetComputeRootConstantBufferView(0, UploadCB(ctx, cb));
    cmd->SetComputeRootShaderResourceView(1, C.h0->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, C.buf0->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, C.buf1->GetGPUVirtualAddress());
    cmd->Dispatch(groups, groups, 1);
    ctx.UavBarrier(C.buf0.Get());
    ctx.UavBarrier(C.buf1.Get());

    // Inverse FFT: rows, then columns; both packed buffers per dispatch.
    cmd->SetPipelineState(psoFFT.Get());
    for (uint32_t dir = 0; dir < 2; ++dir)
    {
        SimCB fftCb = cb;
        fftCb.fftDir = dir;
        cmd->SetComputeRootConstantBufferView(0, UploadCB(ctx, fftCb));
        cmd->Dispatch(fftN, 2, 1);
        ctx.UavBarrier(C.buf0.Get());
        ctx.UavBarrier(C.buf1.Get());
    }

    ctx.Transition(C.buf0.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Transition(C.buf1.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    buffersInSrvState[c] = true;

    // Unpack into textures + foam accumulation.
    cmd->SetPipelineState(psoAssemble.Get());
    cmd->SetComputeRootConstantBufferView(0, UploadCB(ctx, cb));
    cmd->SetComputeRootShaderResourceView(1, C.buf0->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(2, C.buf1->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(6, ctx.SrvGpu(DescSlot::AssembleUav + 4 * c));
    cmd->Dispatch(groups, groups, 1);

    // Rebuild the mip pyramids: box-filter each level from the previous one
    // (the source mip moves UAV -> SRV per level, sky-cubemap pattern).
    ID3D12Resource* maps[3] = { C.disp.Get(), C.deriv.Get(), C.foam.Get() };
    for (uint32_t m = 1; m < mipLevels; ++m)
    {
        D3D12_RESOURCE_BARRIER bar[3] = {};
        for (uint32_t t = 0; t < 3; ++t)
        {
            bar[t].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bar[t].Transition.pResource = maps[t];
            bar[t].Transition.Subresource = m - 1;
            bar[t].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            bar[t].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        cmd->ResourceBarrier(3, bar);

        uint32_t mres = std::max(fftN >> m, 1u);
        MipCB mcb = { mres, {} };
        void* mp = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS mva = ctx.AllocUpload(sizeof(MipCB), &mp);
        memcpy(mp, &mcb, sizeof(MipCB));
        for (uint32_t t = 0; t < 3; ++t)
        {
            uint32_t idx = (c * 3 + t) * 16 + (m - 1);
            cmd->SetPipelineState(t == 2 ? psoMipF.Get() : psoMip4.Get());
            cmd->SetComputeRootConstantBufferView(0, mva);
            cmd->SetComputeRootDescriptorTable(5, ctx.SrvGpu(DescSlot::OceanMipSrv + idx));
            cmd->SetComputeRootDescriptorTable(6, ctx.SrvGpu(DescSlot::OceanMipUav + idx));
            cmd->Dispatch((mres + 7) / 8, (mres + 7) / 8, 1);
        }
    }

    // Everything to the draw SRV state: mips 0..n-2 sit in NON_PIXEL after
    // serving as downsample sources, the last mip is still UAV.
    {
        std::vector<D3D12_RESOURCE_BARRIER> done;
        done.reserve(size_t(3) * mipLevels);
        for (uint32_t t = 0; t < 3; ++t)
            for (uint32_t m = 0; m < mipLevels; ++m)
            {
                D3D12_RESOURCE_BARRIER b = {};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = maps[t];
                b.Transition.Subresource = m;
                b.Transition.StateBefore = (m == mipLevels - 1)
                    ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                b.Transition.StateAfter = kMapSrvState;
                done.push_back(b);
            }
        cmd->ResourceBarrier(UINT(done.size()), done.data());
    }
    mapsEverSimulated[c] = true;
}

void Ocean::RecordSimulation(GpuContext& ctx, float simTime, float dt,
                             const OceanParams& params, bool reinitSpectrum)
{
    for (uint32_t c = 0; c < numCascades; ++c)
        RecordCascadeSim(ctx, c, simTime, dt, params, reinitSpectrum);

    // Copy displacement maps to the readback ring for CPU buoy physics.
    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    for (uint32_t c = 0; c < kReadbackCascades && c < numCascades; ++c)
    {
        Cascade& C = casc[c];
        ctx.Transition(C.disp.Get(), kMapSrvState, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = C.disp.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback[ctx.frameIndex][c].Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst.PlacedFootprint.Footprint.Width = fftN;
        dst.PlacedFootprint.Footprint.Height = fftN;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rbRowPitch;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        ctx.Transition(C.disp.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kMapSrvState);
    }
    ++framesRecorded;
}

void Ocean::Draw(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB)
{
    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    cmd->SetGraphicsRootSignature(ctx.graphicsRS.Get());
    cmd->SetPipelineState(psoDraw.Get());
    cmd->SetGraphicsRootConstantBufferView(0, frameCB);
    cmd->SetGraphicsRootConstantBufferView(1, frameCB);
    cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvGpu(DescSlot::OceanDraw));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// CPU-side water sampling from the readback displacement maps.
// ---------------------------------------------------------------------------
XMFLOAT3 Ocean::SampleDispCpu(uint32_t slot, float x, float z) const
{
    using DirectX::PackedVector::XMConvertHalfToFloat;
    XMFLOAT3 sum{ 0, 0, 0 };
    uint32_t count = std::min(numCascades, kReadbackCascades);
    for (uint32_t c = 0; c < count; ++c)
    {
        const uint8_t* base = rbMapped[slot][c];
        if (!base)
            continue;
        float fx = x / casc[c].L * float(fftN);
        float fz = z / casc[c].L * float(fftN);
        fx -= std::floor(fx / fftN) * fftN;
        fz -= std::floor(fz / fftN) * fftN;
        int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
        float tx = fx - ix, tz = fz - iz;
        int ix1 = (ix + 1) % fftN, iz1 = (iz + 1) % fftN;

        auto texel = [&](int px, int pz, float* out)
        {
            const uint16_t* h = reinterpret_cast<const uint16_t*>(base + size_t(pz) * rbRowPitch + size_t(px) * 8);
            out[0] = XMConvertHalfToFloat(h[0]);
            out[1] = XMConvertHalfToFloat(h[1]);
            out[2] = XMConvertHalfToFloat(h[2]);
        };
        float t00[3], t10[3], t01[3], t11[3];
        texel(ix, iz, t00);
        texel(ix1, iz, t10);
        texel(ix, iz1, t01);
        texel(ix1, iz1, t11);
        for (int k = 0; k < 3; ++k)
        {
            float a = t00[k] + (t10[k] - t00[k]) * tx;
            float b = t01[k] + (t11[k] - t01[k]) * tx;
            float v = a + (b - a) * tz;
            (&sum.x)[k] += v;
        }
    }
    return sum;
}

float Ocean::SampleHeightAt(uint32_t slot, float x, float z, float lambda) const
{
    // Invert the choppy horizontal displacement to find the water column at
    // the fixed world position (x, z).
    float px = x, pz = z;
    XMFLOAT3 d{ 0, 0, 0 };
    for (int it = 0; it < 3; ++it)
    {
        d = SampleDispCpu(slot, px, pz);
        px = x - lambda * d.x;
        pz = z - lambda * d.z;
    }
    return d.y;
}

float Ocean::RoughHeight(GpuContext& ctx, float x, float z) const
{
    if (framesRecorded < GpuContext::kFramesInFlight)
        return 0.0f;
    return SampleDispCpu(ctx.frameIndex, x, z).y;
}

OceanZoomGrid Ocean::PlainConstants() const
{
    OceanZoomGrid g;
    g.fan = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f); // sectors = 0: the shader keeps the plain mesh
    g.rings = XMFLOAT4(std::log(kMeshR0), 1.0f / gridScale, kMeshR0, gridScale);
    g.skirt = XMFLOAT4(float(rings), 0.0f, kMeshR0, kMeshRMax);
    return g;
}

OceanZoomGrid Ocean::PlainZoomGrid()
{
    zoomFanLevel = zoomRingLevel = -1;
    zoomFanWait = zoomRingWait = 0.0f;
    return PlainConstants();
}

// Picks a lattice level for a span that must fit `need` (in plain-mesh
// steps, with two steps of slack for anchoring) within `slots` steps. Level k
// divides the plain step by 2^k, so a refinement keeps every other vertex
// where it was. The finest level that fits is the goal: a span the window has
// outgrown coarsens at once, while a finer level is only taken once it has
// fitted for a moment, so a passing wave cannot make the mesh flicker between
// two levels. Returns false if even the plain step cannot span it.
static bool FitLattice(double need, double slots, float dt, int& level, int64_t& anchor, float& wait)
{
    int fit = int(std::floor(std::log2((slots - 2.0) / std::max(need, 1e-12))));
    if (fit < 0)
    {
        level = -1;
        return false;
    }
    fit = std::min(fit, 16);
    const float kRefineDelay = 0.25f; // s
    wait = (level >= 0 && fit > level) ? wait + dt : 0.0f;
    if (level < 0 || level > fit || wait >= kRefineDelay)
    {
        level = fit;
        anchor = INT64_MIN; // re-anchor below
        wait = 0.0f;
    }
    return true;
}

// Anchors a span of `slots` lattice steps over [lo, hi] (in steps of its
// level): kept while it still covers them, so panning slides the window over
// a lattice that stays put; otherwise centred on them.
static void AnchorLattice(double lo, double hi, double slots, int64_t& anchor)
{
    if (anchor == INT64_MIN || lo < double(anchor) || hi > double(anchor) + slots)
        anchor = std::llround(0.5 * (lo + hi - slots));
}

OceanZoomGrid Ocean::FitZoomGrid(float yaw, float halfAz, float rhoNear, float rhoFar, float dt)
{
    const double kTwoPiD = 6.283185307179586;
    OceanZoomGrid g = PlainConstants();
    const double S = sectors;

    // --- Sectors: a front fan across the view; the back sectors close the
    // circle behind the camera. Plain layout if the view needs nearly all of it.
    const double front = S - kZoomBackSectors;
    double fanNeed = 2.0 * halfAz / (kTwoPiD / S);
    double fanStep = kTwoPiD / S, fanFirst = 0.0, fanFront = S;
    if (FitLattice(fanNeed, front, dt, zoomFanLevel, zoomFanAnchor, zoomFanWait))
    {
        fanStep = kTwoPiD / std::ldexp(S, zoomFanLevel);
        AnchorLattice((yaw - halfAz) / fanStep, (yaw + halfAz) / fanStep, front, zoomFanAnchor);
        fanFirst = double(zoomFanAnchor) * fanStep;
        fanFront = front;
    }
    // The shader turns the fan by the yaw itself: only the small offset from
    // the view direction goes through float, so vertices hold still at 100x.
    g.fan = XMFLOAT4(float(S), float(fanFront), float(fanFirst - double(yaw)), float(fanStep));

    // --- Rings: the window lattice between a few coarse skirt rings, inward
    // to r0 and outward to the sea's edge. Plain rings if the window is wider
    // than any lattice holds.
    const double P = kZoomSkirtRings;
    const double W = double(rings) - 1.0 - 2.0 * P; // window steps
    const double lnEdge = std::log(double(kMeshRMax));
    double n0 = std::log(std::clamp(double(rhoNear), 0.05, 0.99 * double(kMeshRMax)));
    double n1 = std::clamp(std::log(std::max(double(rhoFar), 1e-3)), n0 + 1e-3, lnEdge);
    if (FitLattice((n1 - n0) / gridScale, W, dt, zoomRingLevel, zoomRingAnchor, zoomRingWait))
    {
        double step = gridScale / std::ldexp(1.0, zoomRingLevel);
        double lo = n0 / step, hi = n1 / step;
        AnchorLattice(lo, hi, W, zoomRingAnchor);
        // Never past the sea's edge, where the outer rings would collapse.
        int64_t edge = int64_t(std::ceil(lnEdge / step - W));
        if (zoomRingAnchor > edge)
            zoomRingAnchor = std::max(edge, int64_t(std::ceil(hi - W)));
        g.rings.z = float(std::exp(double(zoomRingAnchor) * step));
        g.rings.w = float(step);
        g.skirt.y = float(P);
    }
    return g;
}

WaterSample Ocean::Sample(GpuContext& ctx, float x, float z, float lambda) const
{
    WaterSample ws;
    if (framesRecorded < GpuContext::kFramesInFlight)
        return ws;
    uint32_t slot = ctx.frameIndex;

    float px = x, pz = z;
    XMFLOAT3 d{ 0, 0, 0 };
    for (int it = 0; it < 3; ++it)
    {
        d = SampleDispCpu(slot, px, pz);
        px = x - lambda * d.x;
        pz = z - lambda * d.z;
    }
    ws.height = d.y;
    ws.displacement = XMFLOAT3(lambda * d.x, d.y, lambda * d.z);

    const float e = 0.9f;
    float hx0 = SampleHeightAt(slot, x - e, z, lambda);
    float hx1 = SampleHeightAt(slot, x + e, z, lambda);
    float hz0 = SampleHeightAt(slot, x, z - e, lambda);
    float hz1 = SampleHeightAt(slot, x, z + e, lambda);
    XMVECTOR n = XMVector3Normalize(XMVectorSet(-(hx1 - hx0) / (2 * e), 1.0f, -(hz1 - hz0) / (2 * e), 0));
    XMStoreFloat3(&ws.normal, n);
    return ws;
}
