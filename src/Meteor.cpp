#include "Meteor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace DirectX;

namespace
{
    struct ObjectCB
    {
        XMFLOAT4X4 world; // pre-transposed
        XMFLOAT4 emissive;
        XMFLOAT4 misc;
    };

    // Must match the constants in Common.hlsli (ImpactWaves).
    constexpr float kG = 9.81f;
    constexpr float kK0 = 0.11f;
    constexpr float kBand = 1.65f;
    constexpr float kR0 = 45.0f;
    constexpr float kTau = 45.0f;
    constexpr float kSplashW = 14.0f;
    constexpr float kImpactMaxAge = 115.0f; // rings are below ~1 cm by then
}

void Meteor::Create(GpuContext& ctx)
{
    // Rough dark rock: a sphere with radial noise.
    struct Vertex
    {
        XMFLOAT3 pos, nrm, col;
    };
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    const int stacks = 10, slices = 14;
    for (int st = 0; st <= stacks; ++st)
    {
        float phi = XM_PI * st / stacks;
        for (int sl = 0; sl <= slices; ++sl)
        {
            float th = 2.0f * XM_PI * sl / slices;
            XMFLOAT3 n(std::sin(phi) * std::cos(th), std::cos(phi), std::sin(phi) * std::sin(th));
            float bump = 1.0f + 0.22f * std::sin(3.1f * phi + 5.0f * th) * std::sin(2.3f * th);
            Vertex v;
            v.nrm = n;
            v.pos = XMFLOAT3(n.x * bump, n.y * bump, n.z * bump);
            v.col = XMFLOAT3(0.06f, 0.05f, 0.045f);
            verts.push_back(v);
        }
    }
    for (int st = 0; st < stacks; ++st)
    {
        for (int sl = 0; sl < slices; ++sl)
        {
            uint32_t a = st * (slices + 1) + sl;
            uint32_t b = a + slices + 1;
            idx.push_back(a); idx.push_back(a + 1); idx.push_back(b);
            idx.push_back(a + 1); idx.push_back(b + 1); idx.push_back(b);
        }
    }
    indexCount = static_cast<uint32_t>(idx.size());

    uint64_t vbSize = verts.size() * sizeof(Vertex);
    uint64_t ibSize = idx.size() * sizeof(uint32_t);
    vb = ctx.CreateBuffer(vbSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, L"MeteorVB");
    ib = ctx.CreateBuffer(ibSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST, L"MeteorIB");
    ctx.UploadBufferData(vb.Get(), verts.data(), vbSize);
    ctx.UploadBufferData(ib.Get(), idx.data(), ibSize);
    vbv = { vb->GetGPUVirtualAddress(), static_cast<UINT>(vbSize), sizeof(Vertex) };
    ibv = { ib->GetGPUVirtualAddress(), static_cast<UINT>(ibSize), DXGI_FORMAT_R32_UINT };

    // Persistently mapped per-frame particle buffers + SRVs at fixed slots.
    for (uint32_t f = 0; f < GpuContext::kFramesInFlight; ++f)
    {
        particleBuf[f] = ctx.CreateBuffer(kMaxParticles * sizeof(GpuParticle),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
            D3D12_RESOURCE_STATE_GENERIC_READ, L"MeteorParticles");
        D3D12_RANGE noRead = { 0, 0 };
        HR(particleBuf[f]->Map(0, &noRead, reinterpret_cast<void**>(&particleMapped[f])));

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.Buffer.NumElements = kMaxParticles;
        sd.Buffer.StructureByteStride = sizeof(GpuParticle);
        ctx.Dev()->CreateShaderResourceView(particleBuf[f].Get(), &sd,
            ctx.SrvCpu(DescSlot::MeteorParticles + f));
    }

    // Pipelines.
    std::wstring file = ctx.ShaderPath(L"Meteor.hlsl");
    ComPtr<ID3DBlob> vs = ctx.CompileShader(file, "VSRock", "vs_5_0");
    ComPtr<ID3DBlob> ps = ctx.CompileShader(file, "PSRock", "ps_5_0");
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = ctx.DefaultPsoDesc();
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { layout, 3 };
    HR(ctx.Dev()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psoRock)));
    psoRock->SetName(L"MeteorRock");

    ComPtr<ID3DBlob> pvs = ctx.CompileShader(file, "VSParticle", "vs_5_0");
    ComPtr<ID3DBlob> pps = ctx.CompileShader(file, "PSParticle", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC qd = ctx.DefaultPsoDesc();
    qd.VS = { pvs->GetBufferPointer(), pvs->GetBufferSize() };
    qd.PS = { pps->GetBufferPointer(), pps->GetBufferSize() };
    qd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    qd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    auto& bl = qd.BlendState.RenderTarget[0];
    bl.BlendEnable = TRUE;
    bl.SrcBlend = D3D12_BLEND_ONE;
    bl.DestBlend = D3D12_BLEND_ONE;
    bl.BlendOp = D3D12_BLEND_OP_ADD;
    bl.SrcBlendAlpha = D3D12_BLEND_ONE;
    bl.DestBlendAlpha = D3D12_BLEND_ONE;
    bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    HR(ctx.Dev()->CreateGraphicsPipelineState(&qd, IID_PPV_ARGS(&psoParticle)));
    psoParticle->SetName(L"MeteorParticle");
}

void Meteor::Launch(const Camera& camera, float impactPower)
{
    if (flying)
        return;
    power = impactPower;

    // Impact point ahead of the camera, with some scatter.
    float cy = std::cos(camera.yaw), sy = std::sin(camera.yaw);
    XMFLOAT2 fwd(sy, cy), perp(cy, -sy);
    float dist = 190.0f + Rand01() * 90.0f;
    float lateral = (Rand01() - 0.5f) * 80.0f;
    XMFLOAT2 target(camera.pos.x + fwd.x * dist + perp.x * lateral,
                    camera.pos.z + fwd.y * dist + perp.y * lateral);

    // Approach: ~45 degree descent, horizontal direction angled a little off
    // the view axis so the streak sweeps diagonally through the frame.
    float beta = (0.18f + 0.32f * Rand01()) * (Rand01() < 0.5f ? -1.0f : 1.0f);
    float cb = std::cos(beta), sb = std::sin(beta);
    XMFLOAT2 u(fwd.x * cb + perp.x * sb, fwd.y * cb + perp.y * sb);
    float elev = XMConvertToRadians(45.0f + (Rand01() - 0.5f) * 12.0f);
    const float speed = 260.0f, range = 650.0f;
    XMFLOAT3 dir(u.x * std::cos(elev), -std::sin(elev), u.y * std::cos(elev));
    vel = XMFLOAT3(dir.x * speed, dir.y * speed, dir.z * speed);
    pos = XMFLOAT3(target.x - dir.x * range, -dir.y * range, target.y - dir.z * range);
    tumble = Rand01() * 10.0f;
    flightTime = 0;
    emitAccum = 0;
    flying = true;
}

Meteor::Particle* Meteor::AllocParticle()
{
    for (Particle& p : particles)
        if (!p.active)
            return &p;
    return nullptr;
}

void Meteor::EmitTrail(float dt)
{
    // Emit spread along the segment flown this frame so the streak stays
    // continuous even at large timesteps.
    XMFLOAT3 prev(pos.x - vel.x * dt, pos.y - vel.y * dt, pos.z - vel.z * dt);
    emitAccum += dt * 130.0f;
    while (emitAccum >= 1.0f)
    {
        emitAccum -= 1.0f;
        Particle* p = AllocParticle();
        if (!p)
            return;
        bool fire = Rand01() < 0.55f;
        float jitter = fire ? 1.0f : 2.2f;
        float seg = Rand01();
        XMFLOAT3 base(prev.x + (pos.x - prev.x) * seg,
                      prev.y + (pos.y - prev.y) * seg,
                      prev.z + (pos.z - prev.z) * seg);
        p->active = true;
        p->pos = XMFLOAT3(base.x + (Rand01() - 0.5f) * jitter,
                          base.y + (Rand01() - 0.5f) * jitter,
                          base.z + (Rand01() - 0.5f) * jitter);
        p->vel = XMFLOAT3(vel.x * 0.04f + (Rand01() - 0.5f) * 4.0f,
                          vel.y * 0.04f + (Rand01() - 0.5f) * 4.0f,
                          vel.z * 0.04f + (Rand01() - 0.5f) * 4.0f);
        p->life = 0;
        if (fire)
        {
            p->maxLife = 0.4f + Rand01() * 0.45f;
            p->size = 2.4f + Rand01() * 1.2f;
            p->growth = 2.2f;
            p->gravity = 0.0f;
            p->drag = 1.5f;
            p->colStart = XMFLOAT3(1.8f, 0.95f, 0.45f);
            p->colEnd = XMFLOAT3(0.9f, 0.16f, 0.03f);
            p->intensity = 2.2f;
        }
        else // smoke
        {
            p->maxLife = 1.6f + Rand01() * 1.2f;
            p->size = 3.5f + Rand01() * 1.5f;
            p->growth = 3.5f;
            p->gravity = -1.0f; // buoyant
            p->drag = 1.0f;
            p->colStart = XMFLOAT3(0.50f, 0.38f, 0.28f);
            p->colEnd = XMFLOAT3(0.10f, 0.09f, 0.09f);
            p->intensity = 0.28f;
        }
    }
}

void Meteor::SpawnImpact()
{
    // Register the ring-wave system (recycle the oldest slot if all in use).
    Impact* slot = nullptr;
    for (Impact& im : impacts)
        if (!im.active)
        {
            slot = &im;
            break;
        }
    if (!slot)
    {
        slot = &impacts[0];
        for (Impact& im : impacts)
            if (im.t > slot->t)
                slot = &im;
    }
    slot->active = true;
    slot->x = pos.x;
    slot->z = pos.z;
    slot->t = 0.001f;
    slot->a0 = power;

    // Flash + upward spray burst.
    if (Particle* flash = AllocParticle())
    {
        flash->active = true;
        flash->pos = XMFLOAT3(pos.x, 4.0f, pos.z);
        flash->vel = XMFLOAT3(0, 2, 0);
        flash->gravity = 0;
        flash->drag = 0;
        flash->life = 0;
        flash->maxLife = 0.8f;
        flash->size = 18.0f;
        flash->growth = 42.0f;
        flash->colStart = XMFLOAT3(2.2f, 1.5f, 0.9f);
        flash->colEnd = XMFLOAT3(0.7f, 0.25f, 0.08f);
        flash->intensity = 3.0f;
    }
    for (int i = 0; i < 42; ++i)
    {
        Particle* p = AllocParticle();
        if (!p)
            break;
        float ang = Rand01() * 2.0f * XM_PI;
        float rad = 2.0f + Rand01() * 10.0f;
        float up = 14.0f + Rand01() * 22.0f;
        p->active = true;
        p->pos = XMFLOAT3(pos.x + std::cos(ang) * rad * 0.3f, 1.0f, pos.z + std::sin(ang) * rad * 0.3f);
        p->vel = XMFLOAT3(std::cos(ang) * rad, up, std::sin(ang) * rad);
        p->gravity = 16.0f;
        p->drag = 0.35f;
        p->life = 0;
        p->maxLife = 1.8f + Rand01() * 1.0f;
        p->size = 3.0f + Rand01() * 2.0f;
        p->growth = 4.5f;
        p->colStart = XMFLOAT3(0.80f, 0.85f, 0.88f);
        p->colEnd = XMFLOAT3(0.12f, 0.14f, 0.15f);
        p->intensity = 1.2f;
    }
}

void Meteor::Update(float simDt)
{
    if (flying)
    {
        flightTime += simDt;
        tumble += simDt * 9.0f;
        pos.x += vel.x * simDt;
        pos.y += vel.y * simDt;
        pos.z += vel.z * simDt;
        EmitTrail(simDt);
        if (pos.y <= 1.0f || flightTime > 12.0f)
        {
            if (pos.y <= 1.0f)
                SpawnImpact();
            flying = false;
        }
    }

    for (Impact& im : impacts)
    {
        if (!im.active)
            continue;
        im.t += simDt;
        if (im.t > kImpactMaxAge)
            im.active = false; // sea fully restored
    }

    for (Particle& p : particles)
    {
        if (!p.active)
            continue;
        p.life += simDt;
        if (p.life >= p.maxLife)
        {
            p.active = false;
            continue;
        }
        p.vel.y -= p.gravity * simDt;
        float damp = std::exp(-p.drag * simDt);
        p.vel.x *= damp;
        p.vel.y *= damp;
        p.vel.z *= damp;
        p.pos.x += p.vel.x * simDt;
        p.pos.y += p.vel.y * simDt;
        p.pos.z += p.vel.z * simDt;
        if (p.pos.y < 0.4f)
            p.pos.y = 0.4f; // spray settles on the surface
        p.size += p.growth * simDt;
    }
}

void Meteor::UploadParticles(GpuContext& ctx)
{
    GpuParticle* dst = particleMapped[ctx.frameIndex];
    uint32_t n = 0;
    for (const Particle& p : particles)
    {
        if (!p.active || n >= kMaxParticles)
            continue;
        float f = p.life / p.maxLife;
        float fade = (1.0f - f) * (1.0f - f);
        XMFLOAT3 col(p.colStart.x + (p.colEnd.x - p.colStart.x) * f,
                     p.colStart.y + (p.colEnd.y - p.colStart.y) * f,
                     p.colStart.z + (p.colEnd.z - p.colStart.z) * f);
        dst[n].posSize = XMFLOAT4(p.pos.x, p.pos.y, p.pos.z, p.size);
        dst[n].color = XMFLOAT4(col.x, col.y, col.z, p.intensity * fade);
        ++n;
    }
    liveParticleCount = n;
}

void Meteor::DrawRock(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB, const XMFLOAT3& camPos)
{
    if (!flying)
        return;
    XMMATRIX world = XMMatrixScaling(2.2f, 2.2f, 2.2f)
        * XMMatrixRotationRollPitchYaw(tumble, tumble * 0.63f, tumble * 0.31f)
        * XMMatrixTranslation(pos.x - camPos.x, pos.y - camPos.y, pos.z - camPos.z);
    ObjectCB cb = {};
    XMStoreFloat4x4(&cb.world, XMMatrixTranspose(world));
    float flicker = 0.75f + 0.25f * std::sin(flightTime * 41.0f) * std::sin(flightTime * 17.3f);
    cb.emissive = XMFLOAT4(1.6f, 0.75f, 0.32f, 22.0f * flicker);

    void* p = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS objVa = ctx.AllocUpload(sizeof(ObjectCB), &p);
    memcpy(p, &cb, sizeof(ObjectCB));

    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    cmd->SetGraphicsRootSignature(ctx.graphicsRS.Get());
    cmd->SetPipelineState(psoRock.Get());
    cmd->SetGraphicsRootConstantBufferView(0, frameCB);
    cmd->SetGraphicsRootConstantBufferView(1, objVa);
    cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvGpu(DescSlot::BuoyDraw));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
}

void Meteor::DrawTrail(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB)
{
    if (liveParticleCount == 0)
        return;
    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    cmd->SetGraphicsRootSignature(ctx.graphicsRS.Get());
    cmd->SetPipelineState(psoParticle.Get());
    cmd->SetGraphicsRootConstantBufferView(0, frameCB);
    cmd->SetGraphicsRootConstantBufferView(1, frameCB);
    cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvGpu(DescSlot::MeteorParticles + ctx.frameIndex));
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    cmd->DrawInstanced(4, liveParticleCount, 0, 0);
}

void Meteor::FillImpacts(XMFLOAT4 out[kMaxImpacts]) const
{
    for (uint32_t i = 0; i < kMaxImpacts; ++i)
    {
        if (impacts[i].active)
            out[i] = XMFLOAT4(impacts[i].x, impacts[i].z, impacts[i].t, impacts[i].a0);
        else
            out[i] = XMFLOAT4(0, 0, -1.0f, 0);
    }
}

bool Meteor::AnyImpactActive() const
{
    for (const Impact& im : impacts)
        if (im.active)
            return true;
    return false;
}

float Meteor::HeightAt(float x, float z) const
{
    // CPU mirror of ImpactWaves (height only) for buoy physics.
    float h = 0;
    for (const Impact& im : impacts)
    {
        if (!im.active)
            continue;
        float dx = x - im.x, dz = z - im.z;
        float r = std::max(std::sqrt(dx * dx + dz * dz), 2.0f);
        float t = im.t;

        float kloc = kG * t * t / (4.0f * r * r);
        float phase = -kG * t * t / (4.0f * r);
        float lx = std::log(kloc / kK0);
        float env = std::exp(-lx * lx * kBand);
        float A = im.a0 * std::pow(kR0 / (kR0 + r), 0.75f) * std::exp(-t / kTau) * env;

        float sedge = std::exp(-r * r / (kSplashW * kSplashW));
        float spulse = im.a0 * 1.8f * std::cos(2.2f * t) * std::exp(-t / 2.8f);
        h += A * std::cos(phase) - spulse * sedge;
    }
    return h;
}
