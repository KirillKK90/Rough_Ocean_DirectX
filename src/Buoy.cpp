#include "Buoy.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "Meteor.h"
#include "Whirlpool.h"

using namespace DirectX;

namespace
{
    struct ObjectCB
    {
        XMFLOAT4X4 world; // pre-transposed
        XMFLOAT4 emissive;
        XMFLOAT4 misc;
    };

    D3D12_GPU_VIRTUAL_ADDRESS UploadObjCB(GpuContext& ctx, const ObjectCB& data)
    {
        void* p = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS va = ctx.AllocUpload(sizeof(ObjectCB), &p);
        memcpy(p, &data, sizeof(ObjectCB));
        return va;
    }
}

void Buoy::Create(GpuContext& ctx)
{
    BuildMesh(ctx);

    std::wstring file = ctx.ShaderPath(L"Buoy.hlsl");
    ComPtr<ID3DBlob> vs = ctx.CompileShader(file, "VSBuoy", "vs_5_0");
    ComPtr<ID3DBlob> ps = ctx.CompileShader(file, "PSBuoy", "ps_5_0");
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = ctx.DefaultPsoDesc();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { layout, 3 };
    HR(ctx.Dev()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoBody)));
    psoBody->SetName(L"BuoyBody");

    ComPtr<ID3DBlob> gvs = ctx.CompileShader(file, "VSGlow", "vs_5_0");
    ComPtr<ID3DBlob> gps = ctx.CompileShader(file, "PSGlow", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = ctx.DefaultPsoDesc();
    gd.VS = { gvs->GetBufferPointer(), gvs->GetBufferSize() };
    gd.PS = { gps->GetBufferPointer(), gps->GetBufferSize() };
    gd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    auto& bl = gd.BlendState.RenderTarget[0];
    bl.BlendEnable = TRUE;
    bl.SrcBlend = D3D12_BLEND_ONE;
    bl.DestBlend = D3D12_BLEND_ONE;
    bl.BlendOp = D3D12_BLEND_OP_ADD;
    bl.SrcBlendAlpha = D3D12_BLEND_ONE;
    bl.DestBlendAlpha = D3D12_BLEND_ONE;
    bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    HR(ctx.Dev()->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&psoGlow)));
    psoGlow->SetName(L"BuoyGlow");
}

void Buoy::BuildMesh(GpuContext& ctx)
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    const int SEG = 28;

    // Surface of revolution from a profile of (y, radius, color) knots.
    struct Knot { float y, r; XMFLOAT3 col; };
    auto lathe = [&](const std::vector<Knot>& prof)
    {
        uint32_t base = static_cast<uint32_t>(verts.size());
        size_t rows = prof.size();
        for (size_t i = 0; i < rows; ++i)
        {
            // Normal from the profile slope.
            float dy, dr;
            if (i == 0) { dy = prof[1].y - prof[0].y; dr = prof[1].r - prof[0].r; }
            else if (i == rows - 1) { dy = prof[i].y - prof[i - 1].y; dr = prof[i].r - prof[i - 1].r; }
            else { dy = prof[i + 1].y - prof[i - 1].y; dr = prof[i + 1].r - prof[i - 1].r; }
            float len = std::sqrt(dy * dy + dr * dr);
            float nr = len > 1e-5f ? dy / len : 1.0f;   // radial component
            float ny = len > 1e-5f ? -dr / len : 0.0f;  // vertical component
            for (int s = 0; s <= SEG; ++s)
            {
                float a = 2.0f * XM_PI * s / SEG;
                float ca = std::cos(a), sa = std::sin(a);
                Vertex v;
                v.pos = XMFLOAT3(prof[i].r * ca, prof[i].y, prof[i].r * sa);
                v.nrm = XMFLOAT3(nr * ca, ny, nr * sa);
                v.col = prof[i].col;
                verts.push_back(v);
            }
        }
        for (size_t i = 0; i + 1 < rows; ++i)
        {
            for (int s = 0; s < SEG; ++s)
            {
                uint32_t a = base + static_cast<uint32_t>(i) * (SEG + 1) + s;
                uint32_t b = a + SEG + 1;
                idx.push_back(a); idx.push_back(b); idx.push_back(a + 1);
                idx.push_back(a + 1); idx.push_back(b); idx.push_back(b + 1);
            }
        }
    };

    auto sphere = [&](XMFLOAT3 c, float r, XMFLOAT3 col, int stacks, int slices)
    {
        uint32_t base = static_cast<uint32_t>(verts.size());
        for (int st = 0; st <= stacks; ++st)
        {
            float phi = XM_PI * st / stacks;
            for (int sl = 0; sl <= slices; ++sl)
            {
                float th = 2.0f * XM_PI * sl / slices;
                XMFLOAT3 n(std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th));
                Vertex v;
                v.nrm = n;
                v.pos = XMFLOAT3(c.x + n.x * r, c.y + n.y * r, c.z + n.z * r);
                v.col = col;
                verts.push_back(v);
            }
        }
        for (int st = 0; st < stacks; ++st)
        {
            for (int sl = 0; sl < slices; ++sl)
            {
                uint32_t a = base + st * (slices + 1) + sl;
                uint32_t b = a + slices + 1;
                idx.push_back(a); idx.push_back(a + 1); idx.push_back(b);
                idx.push_back(a + 1); idx.push_back(b + 1); idx.push_back(b);
            }
        }
    };

    const XMFLOAT3 red(0.48f, 0.05f, 0.04f);
    const XMFLOAT3 white(0.75f, 0.74f, 0.70f);
    const XMFLOAT3 dark(0.10f, 0.10f, 0.11f);
    const XMFLOAT3 gray(0.35f, 0.36f, 0.38f);

    // Hull: cone bottom -> flared body -> waterline band -> deck.
    lathe({
        { -1.9f, 0.02f, dark },
        { -1.1f, 0.75f, red },
        { -0.30f, 1.15f, red },
        { 0.10f, 1.10f, red },
        { 0.35f, 1.00f, white },
        { 0.60f, 0.85f, red },
        { 0.75f, 0.55f, red },
        { 0.80f, 0.0f, red },
    });
    // Tower mast.
    lathe({
        { 0.75f, 0.16f, gray },
        { 2.45f, 0.13f, gray },
        { 2.50f, 0.0f, gray },
    });
    // Lamp platform.
    lathe({
        { 2.45f, 0.02f, dark },
        { 2.50f, 0.42f, dark },
        { 2.58f, 0.42f, dark },
        { 2.60f, 0.20f, dark },
        { 2.95f, 0.17f, dark },
        { 3.00f, 0.0f, dark },
    });
    bodyIndexCount = static_cast<uint32_t>(idx.size());

    // Lamp: emissive sphere (separate draw range).
    lampIndexStart = bodyIndexCount;
    sphere(XMFLOAT3(0, kLampHeight, 0), 0.26f, XMFLOAT3(0.9f, 0.9f, 0.85f), 10, 14);
    lampIndexCount = static_cast<uint32_t>(idx.size()) - lampIndexStart;

    uint64_t vbSize = verts.size() * sizeof(Vertex);
    uint64_t ibSize = idx.size() * sizeof(uint32_t);
    vb = ctx.CreateBuffer(vbSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, L"BuoyVB");
    ib = ctx.CreateBuffer(ibSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, L"BuoyIB");
    ctx.UploadBufferData(vb.Get(), verts.data(), vbSize);
    ctx.UploadBufferData(ib.Get(), idx.data(), ibSize);
    vbv = { vb->GetGPUVirtualAddress(), static_cast<UINT>(vbSize), sizeof(Vertex) };
    ibv = { ib->GetGPUVirtualAddress(), static_cast<UINT>(ibSize), DXGI_FORMAT_R32_UINT };
}

void Buoy::Update(GpuContext& ctx, Ocean& ocean, const Meteor* meteor, const Whirlpool* whirl,
                  float dt, float simTime, float lambda)
{
    dt = std::min(dt, 0.05f);
    WaterSample ws = ocean.Sample(ctx, anchor.x, anchor.y, lambda);

    // Event waves (meteorite impact rings, whirlpool funnel): add their height
    // and blend their slope into the sampled normal so the buoy rides them
    // like any other wave.
    bool meteorOn = meteor && meteor->AnyImpactActive();
    bool whirlOn = whirl && whirl->Active();
    if (meteorOn || whirlOn)
    {
        auto eventH = [&](float px, float pz)
        {
            float h = 0.0f;
            if (meteorOn)
                h += meteor->HeightAt(px, pz);
            if (whirlOn)
                h += whirl->HeightAt(px, pz);
            return h;
        };
        ws.height += eventH(anchor.x, anchor.y);
        const float e = 2.0f;
        float sx = (eventH(anchor.x + e, anchor.y) - eventH(anchor.x - e, anchor.y)) / (2 * e);
        float sz = (eventH(anchor.x, anchor.y + e) - eventH(anchor.x, anchor.y - e)) / (2 * e);
        float nsx = -ws.normal.x / std::max(ws.normal.y, 0.2f) + sx;
        float nsz = -ws.normal.z / std::max(ws.normal.y, 0.2f) + sz;
        XMVECTOR n = XMVector3Normalize(XMVectorSet(-nsx, 1.0f, -nsz, 0));
        XMStoreFloat3(&ws.normal, n);
    }

    // Heave: buoyancy spring toward the water surface with drag.
    float depth = ws.height - heaveY;
    heaveV += depth * 8.0f * dt;
    heaveV *= std::exp(-1.9f * dt);
    heaveY += heaveV * dt;
    heaveY = std::clamp(heaveY, ws.height - 2.2f, ws.height + 2.2f);

    // Tilt toward the (reduced) wave normal with inertia.
    XMVECTOR target = XMVector3Normalize(XMVectorLerp(
        XMVectorSet(0, 1, 0, 0), XMLoadFloat3(&ws.normal), 0.75f));
    XMVECTOR cur = XMLoadFloat3(&up);
    float blend = 1.0f - std::exp(-2.5f * dt);
    XMStoreFloat3(&up, XMVector3Normalize(XMVectorLerp(cur, target, blend)));

    // Anchored sway: follow a fraction of the water's horizontal displacement.
    sway.x += ((ws.displacement.x * 0.8f) - sway.x) * (1.0f - std::exp(-1.5f * dt));
    sway.y += ((ws.displacement.z * 0.8f) - sway.y) * (1.0f - std::exp(-1.5f * dt));
    rollPhase = simTime;

    // Flash pattern: red pulse, dark, green pulse, dark.
    float t = std::fmod(simTime, flashPeriod);
    float half = flashPeriod * 0.5f;
    lightOn = (t < flashDuration) || (t >= half && t < half + flashDuration);
    lightColor = (t < half) ? XMFLOAT3(1.0f, 0.04f, 0.02f) : XMFLOAT3(0.05f, 1.0f, 0.10f);

    XMVECTOR lamp = XMVector3Transform(XMVectorSet(0, kLampHeight, 0, 1), WorldMatrix());
    XMStoreFloat3(&lampWorldPos, lamp);
}

XMMATRIX Buoy::WorldMatrix() const
{
    // Basis from the smoothed up vector, tiny slow yaw for life.
    XMVECTOR u = XMVector3Normalize(XMLoadFloat3(&up));
    XMVECTOR f = XMVector3Normalize(XMVector3Cross(XMVectorSet(1, 0, 0, 0), u));
    XMVECTOR r = XMVector3Cross(u, f);
    float yaw = 0.12f * std::sin(rollPhase * 0.31f);
    XMMATRIX rot = XMMatrixRotationY(yaw);
    XMMATRIX basis(r, u, f, XMVectorSet(0, 0, 0, 1));
    XMMATRIX trans = XMMatrixTranslation(anchor.x + sway.x, heaveY, anchor.y + sway.y);
    return XMMatrixScaling(1.3f, 1.3f, 1.3f) * rot * basis * trans;
}

void Buoy::Draw(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB, const XMFLOAT3& camPos)
{
    XMMATRIX world = WorldMatrix();
    world = world * XMMatrixTranslation(-camPos.x, -camPos.y, -camPos.z); // camera-relative

    ObjectCB body = {};
    XMStoreFloat4x4(&body.world, XMMatrixTranspose(world));
    body.emissive = XMFLOAT4(0, 0, 0, 0);
    ObjectCB lampCb = body;
    if (lightOn)
        lampCb.emissive = XMFLOAT4(lightColor.x, lightColor.y, lightColor.z, lampIntensity);
    else
        lampCb.emissive = XMFLOAT4(0.02f, 0.02f, 0.02f, 1.0f);

    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    cmd->SetGraphicsRootSignature(ctx.graphicsRS.Get());
    cmd->SetPipelineState(psoBody.Get());
    cmd->SetGraphicsRootConstantBufferView(0, frameCB);
    cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvGpu(DescSlot::BuoyDraw));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);

    cmd->SetGraphicsRootConstantBufferView(1, UploadObjCB(ctx, body));
    cmd->DrawIndexedInstanced(bodyIndexCount, 1, 0, 0, 0);
    cmd->SetGraphicsRootConstantBufferView(1, UploadObjCB(ctx, lampCb));
    cmd->DrawIndexedInstanced(lampIndexCount, 1, lampIndexStart, 0, 0);
}

void Buoy::DrawGlow(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB, const XMFLOAT3& camPos)
{
    if (!lightOn)
        return;
    ObjectCB cb = {};
    cb.emissive = XMFLOAT4(lightColor.x, lightColor.y, lightColor.z, lampIntensity * 0.35f);
    cb.misc = XMFLOAT4(lampWorldPos.x - camPos.x, lampWorldPos.y - camPos.y,
                       lampWorldPos.z - camPos.z, 1.6f);

    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    cmd->SetGraphicsRootSignature(ctx.graphicsRS.Get());
    cmd->SetPipelineState(psoGlow.Get());
    cmd->SetGraphicsRootConstantBufferView(0, frameCB);
    cmd->SetGraphicsRootConstantBufferView(1, UploadObjCB(ctx, cb));
    cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvGpu(DescSlot::BuoyDraw));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cmd->DrawInstanced(4, 1, 0, 0);
}
