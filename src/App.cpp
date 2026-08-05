#include "App.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>
#include <wincodec.h>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace DirectX;

namespace
{
    struct TimePreset
    {
        const char* name;
        float sunElev, sunAz;   // degrees; azimuth 0 = straight ahead (+Z)
        float moonElev, moonAz;
        float exposure;
    };
    const TimePreset kTimes[] = {
        { "Early morning", 3.5f, 14.0f, -40.0f, 0.0f, 1.35f },
        { "Morning", 24.0f, 38.0f, -40.0f, 0.0f, 0.85f },
        { "Noon", 62.0f, 20.0f, -40.0f, 0.0f, 0.75f },
        { "Afternoon", 35.0f, -26.0f, -40.0f, 0.0f, 0.85f },
        { "Evening", 8.0f, 8.0f, -40.0f, 0.0f, 1.25f },
        { "Late evening", -5.5f, 5.0f, 21.0f, -22.0f, 3.0f },
        { "Night", -30.0f, 0.0f, 43.0f, 12.0f, 5.0f },
    };
    constexpr int kNumTimes = int(std::size(kTimes));

    struct SeaPreset
    {
        const char* name;
        float U10, fetchKm, chop, foamBias, foamDecay, foamAdd, spread, ampEst;
    };
    const SeaPreset kSeas[] = {
        { "0 - Glassy calm", 0.9f, 20, 0.30f, 0.30f, 0.50f, 0.3f, 9.0f, 0.02f },
        { "1 - Rippled", 1.8f, 30, 0.45f, 0.35f, 0.50f, 0.4f, 8.0f, 0.06f },
        { "2 - Smooth wavelets", 3.2f, 45, 0.55f, 0.45f, 0.45f, 0.6f, 7.0f, 0.15f },
        { "3 - Slight", 5.0f, 65, 0.65f, 0.58f, 0.40f, 0.8f, 6.5f, 0.40f },
        { "4 - Moderate", 7.5f, 90, 0.75f, 0.70f, 0.35f, 1.0f, 6.0f, 0.80f },
        { "5 - Rough", 10.5f, 120, 0.82f, 0.78f, 0.30f, 1.2f, 5.5f, 1.40f },
        { "6 - Very rough", 13.5f, 150, 0.90f, 0.86f, 0.28f, 1.4f, 5.0f, 2.20f },
        { "7 - High seas", 17.0f, 185, 0.97f, 0.93f, 0.26f, 1.6f, 4.5f, 3.20f },
        { "8 - Gale, very high", 21.0f, 225, 1.05f, 1.00f, 0.24f, 1.8f, 4.0f, 4.50f },
        { "9 - Severe storm", 25.5f, 265, 1.15f, 1.08f, 0.22f, 2.0f, 3.5f, 6.50f },
    };

    struct LodPreset
    {
        const char* name;
        OceanQuality quality;
        uint32_t skyRes;
        bool fxaa;
        float fade1, fade2; // detail cascade visibility distances
    };
    const LodPreset kLods[] = {
        { "Low", { 128, 2, 256, 160 }, 64, false, 1500.0f, 180.0f },
        { "Medium", { 256, 3, 384, 224 }, 128, true, 2200.0f, 260.0f },
        { "High", { 256, 3, 512, 288 }, 128, true, 3200.0f, 380.0f },
        { "Ultra", { 512, 3, 640, 352 }, 256, true, 4200.0f, 500.0f },
    };
    constexpr int kNumLods = int(std::size(kLods));

    XMFLOAT3 DirFromElevAz(float elevDeg, float azDeg)
    {
        float e = XMConvertToRadians(elevDeg);
        float a = XMConvertToRadians(azDeg);
        return XMFLOAT3(std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a));
    }

    // Approximate atmospheric transmittance for a light source at elevation.
    XMFLOAT3 Transmittance(float elevDeg)
    {
        float z = std::clamp(90.0f - elevDeg, 0.0f, 98.0f);
        float m;
        if (elevDeg <= -2.0f)
            m = 42.0f;
        else
            m = 1.0f / (std::cos(XMConvertToRadians(z)) + 0.15f * std::pow(93.885f - z, -1.253f));
        const XMFLOAT3 ext(0.062f, 0.130f, 0.300f);
        return XMFLOAT3(std::exp(-m * ext.x), std::exp(-m * ext.y), std::exp(-m * ext.z));
    }

    LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
        {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (app)
            return app->HandleMsg(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------

int App::Run(HINSTANCE hInst, const LaunchOptions& options)
{
    opts = options;
    uiLod = std::clamp(opts.lod, 0, kNumLods - 1);
    uiSeaState = std::clamp(opts.seaState, 0, 9);
    uiTimeOfDay = std::clamp(opts.timeOfDay, 0, kNumTimes - 1);
    uiVsync = opts.vsync;

    HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    if (opts.selftest)
        return RunFFTSelfTest();

    InitWindow(hInst);
    InitSystems();

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    perfFreq = double(freq.QuadPart);
    lastTicks = now.QuadPart;

    MSG msg = {};
    int pendingLod = uiLod;
    while (running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running)
            break;

        if (pendingLod != uiLod)
        {
            ApplyLod(uiLod, false);
            pendingLod = uiLod;
        }

        QueryPerformanceCounter(&now);
        float dt = float(double(now.QuadPart - lastTicks) / perfFreq);
        lastTicks = now.QuadPart;
        dt = std::clamp(dt, 0.0001f, 0.1f);

        if (opts.fixedDt > 0.0f)
            dt = opts.fixedDt; // deterministic verification runs

        UpdateCameraInput(dt);
        simTime += dt * uiTimeScale;

        if (opts.meteorAt >= 0.0f && simTime >= opts.meteorAt && !meteorAutoLaunched)
        {
            meteor.Launch(camera, uiMeteorPower);
            meteorAutoLaunched = true;
        }

        RenderFrame(dt);

        // FPS display accumulation.
        fpsAccum += dt;
        ++fpsFrames;
        if (fpsAccum > 0.5)
        {
            fpsDisplay = float(fpsFrames / fpsAccum);
            fpsAccum = 0;
            fpsFrames = 0;
        }

        // Bench mode.
        ++frameCounter;
        if (opts.benchFrames > 0)
        {
            if (frameCounter > 30) // warmup
            {
                benchAccum += dt;
                ++benchFramesDone;
            }
            if (frameCounter >= opts.benchFrames)
            {
                double avg = benchFramesDone / std::max(benchAccum, 1e-6);
                LogF("Benchmark: %d frames, avg %.1f FPS (%.2f ms)\n",
                     benchFramesDone, avg, 1000.0 / std::max(avg, 1e-6));
                if (!opts.screenshotPath.empty())
                {
                    std::wstring wpath(opts.screenshotPath.begin(), opts.screenshotPath.end());
                    SaveBackbufferPng(wpath);
                }
                running = false;
            }
        }
    }

    audio.Shutdown();
    ctx.WaitIdle();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ctx.Shutdown();
    return 0;
}

void App::InitWindow(HINSTANCE hInst)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RoughOceanWnd";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, LONG(opts.width), LONG(opts.height) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Rough Open Ocean - DirectX 12",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, this);
    ShowWindow(hwnd, SW_SHOW);
}

void App::InitSystems()
{
    ctx.Init(hwnd, opts.width, opts.height);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(ctx.Dev(), GpuContext::kFramesInFlight, GpuContext::kBackbufferFormat,
        ctx.srvHeap.Get(), ctx.SrvCpu(DescSlot::ImGuiFont), ctx.SrvGpu(DescSlot::ImGuiFont));

    camera.pos = XMFLOAT3(0.0f, 12.0f, 0.0f);
    camera.yaw = 0.0f;
    camera.pitch = -0.02f;
    if (opts.hasCamera)
    {
        camera.pos = XMFLOAT3(opts.camX, opts.camY, opts.camZ);
        camera.yaw = XMConvertToRadians(opts.yawDeg);
        camera.pitch = XMConvertToRadians(opts.pitchDeg);
    }
    buoy.anchor = XMFLOAT2(0.0f, 95.0f);

    ApplyLod(uiLod, true);
    buoy.Create(ctx);
    meteor.Create(ctx);
    post.Create(ctx, ctx.width, ctx.height);
    ApplySeaState();
    UpdateLighting();

    if (opts.benchFrames == 0)
        audio.Init();
}

void App::ApplyLod(int lod, bool firstTime)
{
    const LodPreset& lp = kLods[lod];
    if (!firstTime)
        ctx.WaitIdle();
    ocean.Create(ctx, lp.quality);
    if (firstTime || sky.Resolution() != lp.skyRes)
        sky.Create(ctx, lp.skyRes);
    sky.MarkDirty();
    uiFxaa = lp.fxaa;
    spectrumDirty = true;
}

void App::ApplySeaState()
{
    const SeaPreset& sp = kSeas[uiSeaState];
    oceanParams.U10 = sp.U10;
    oceanParams.fetch = sp.fetchKm * 1000.0f;
    oceanParams.spreadExp = sp.spread;
    oceanParams.ampScale = uiAmpMul;
    oceanParams.choppiness = sp.chop * uiChopMul;
    oceanParams.foamBias = sp.foamBias;
    oceanParams.foamDecay = sp.foamDecay;
    // The Foam slider reads 0.50x..1.00x (a clean, uncapped-looking control)
    // but maps to a gentle actual whitecap range of 0.1x..0.5x of the preset.
    float foamMul = 0.1f + (uiFoamMul - 0.5f) * 0.8f;
    oceanParams.foamAdd = sp.foamAdd * foamMul;
    float a = XMConvertToRadians(uiWindDirDeg);
    oceanParams.windDir = XMFLOAT2(std::sin(a), std::cos(a));
    ampEstimate = sp.ampEst * uiAmpMul;
}

void App::UpdateLighting()
{
    const TimePreset& tp = kTimes[uiTimeOfDay];
    XMFLOAT3 sunDir = DirFromElevAz(tp.sunElev, tp.sunAz);
    XMFLOAT3 moonDir = DirFromElevAz(tp.moonElev, tp.moonAz);
    bool moonPrimary = tp.sunElev < 1.0f;

    XMFLOAT3 tSun = Transmittance(tp.sunElev);
    XMFLOAT3 tMoon = Transmittance(tp.moonElev);

    const float sunPower = 24.0f;
    if (!moonPrimary)
    {
        lightDir = sunDir;
        lightIsMoon = 0.0f;
        lightColor = XMFLOAT3(sunPower * tSun.x, sunPower * tSun.y, sunPower * tSun.z);
        sunDiscColor = XMFLOAT3(420.0f * tSun.x, 415.0f * tSun.y, 405.0f * tSun.z);
    }
    else
    {
        lightDir = moonDir;
        lightIsMoon = 1.0f;
        const XMFLOAT3 mt(0.62f, 0.75f, 1.05f);
        const float moonPower = 0.045f;
        lightColor = XMFLOAT3(moonPower * mt.x * tMoon.x, moonPower * mt.y * tMoon.y, moonPower * mt.z * tMoon.z);
        sunDiscColor = XMFLOAT3(2.4f * tMoon.x, 2.35f * tMoon.y, 2.2f * tMoon.z);
    }

    skyParams.sunDir = sunDir;
    skyParams.sunIntensity = tp.sunElev > -12.0f ? 22.0f : 0.0f;
    skyParams.moonDir = moonDir;
    skyParams.moonIntensity = moonPrimary ? 0.9f : 0.0f;
    skyParams.cloudCover = uiCloudCover;

    starIntensity = std::clamp((-tp.sunElev - 3.0f) / 6.0f, 0.0f, 1.0f);
    exposureBase = tp.exposure;
}

void App::UpdateCameraInput(float dt)
{
    if (GetForegroundWindow() != hwnd)
        return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard)
        return;
    auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    float fwd = (key('W') || key(VK_UP) ? 1.0f : 0.0f) - (key('S') || key(VK_DOWN) ? 1.0f : 0.0f);
    float right = (key('D') || key(VK_RIGHT) ? 1.0f : 0.0f) - (key('A') || key(VK_LEFT) ? 1.0f : 0.0f);
    float up = (key('E') ? 1.0f : 0.0f) - (key('Q') ? 1.0f : 0.0f);
    float boost = key(VK_SHIFT) ? 4.0f : 1.0f;
    if (fwd != 0 || right != 0 || up != 0)
        camera.Move(fwd * boost, right * boost, up * boost, dt);
}

D3D12_GPU_VIRTUAL_ADDRESS App::FillFrameCB()
{
    const LodPreset& lp = kLods[uiLod];
    float aspect = float(ctx.width) / float(ctx.height);
    XMMATRIX vp = camera.ViewProj(aspect);
    XMMATRIX ivp = XMMatrixInverse(nullptr, vp);

    FrameCB cb = {};
    XMStoreFloat4x4(&cb.viewProj, XMMatrixTranspose(vp));
    XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(ivp));
    cb.camPos = camera.pos;
    cb.time = simTime;
    XMVECTOR f = camera.Forward();
    XMVECTOR r = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), f));
    XMVECTOR u = XMVector3Cross(f, r);
    XMStoreFloat3(&cb.camRight, r);
    XMStoreFloat3(&cb.camUp, u);
    cb.exposure = exposureBase * uiExposureMul;
    cb.lightIsMoon = lightIsMoon;
    cb.lightDir = lightDir;
    cb.starIntensity = starIntensity;
    cb.lightColor = lightColor;
    cb.roughBase = 0.055f;
    cb.sunDiscColor = sunDiscColor;
    cb.lambda = oceanParams.choppiness;

    float wf = std::clamp(oceanParams.U10 / 12.0f, 0.05f, 1.2f);
    cb.cascade0 = XMFLOAT4(1.0f / ocean.CascadeLength(0), 1e8f, 0.0f, ampEstimate);
    cb.cascade1 = XMFLOAT4(1.0f / ocean.CascadeLength(1), lp.fade1, 0.065f * wf, 0);
    cb.cascade2 = XMFLOAT4(1.0f / ocean.CascadeLength(2), lp.fade2, 0.100f * wf, 0);

    cb.waterDeep = XMFLOAT3(0.003f, 0.013f, 0.026f);
    cb.foamAmount = 1.0f;
    cb.waterScatter = XMFLOAT3(0.010f, 0.062f, 0.080f);
    cb.sss = 0.45f;

    XMFLOAT3 lamp = buoy.LampWorldPos();
    cb.buoyLightPos = XMFLOAT3(lamp.x - camera.pos.x, lamp.y - camera.pos.y, lamp.z - camera.pos.z);
    cb.buoyLightOn = buoy.LightOn() ? 2.5f : 0.0f;
    cb.buoyLightColor = buoy.LightColor();
    cb.fogDensity = 1.0e-4f;
    cb.windDir = oceanParams.windDir;
    cb.distRough = 0.16f;
    meteor.FillImpacts(cb.impacts);

    void* p = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS va = ctx.AllocUpload(sizeof(FrameCB), &p);
    memcpy(p, &cb, sizeof(FrameCB));
    return va;
}

void App::RenderFrame(float dt)
{
    // Ocean sound follows the sea state, wave height and wave speed; it also
    // grows quieter as the camera climbs away from the surface.
    if (audio.Available())
    {
        float s01 = uiSeaState / 9.0f;
        float storm = std::clamp(std::pow(s01, 0.85f) * (0.7f + 0.3f * uiAmpMul), 0.0f, 1.0f);
        float heightAtt = std::clamp(1.15f - camera.pos.y / 220.0f, 0.25f, 1.0f);
        float motion = std::clamp(uiTimeScale, 0.0f, 1.0f) * heightAtt;
        audio.SetParams(storm, motion, uiVolume, SoundMode(uiSoundMode));
    }

    ctx.BeginFrame();
    uint32_t bbIdx = ctx.swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* backbuffer = ctx.backbuffers[bbIdx].Get();
    ctx.Transition(backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // Physics reads the readback slot BeginFrame just fenced.
    meteor.Update(dt * uiTimeScale);
    meteor.UploadParticles(ctx);
    buoy.Update(ctx, ocean, &meteor, dt * uiTimeScale, simTime, oceanParams.choppiness);

    sky.RecordGenerate(ctx, skyParams);
    ocean.RecordSimulation(ctx, simTime, dt * uiTimeScale, oceanParams, spectrumDirty);
    spectrumDirty = false;

    D3D12_GPU_VIRTUAL_ADDRESS frameCB = FillFrameCB();

    // --- Scene into HDR ---
    D3D12_VIEWPORT vp = { 0, 0, float(ctx.width), float(ctx.height), 0, 1 };
    D3D12_RECT sc = { 0, 0, LONG(ctx.width), LONG(ctx.height) };
    ID3D12GraphicsCommandList* cmd = ctx.Cmd();
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    D3D12_CPU_DESCRIPTOR_HANDLE hdrRtv = post.HdrRtv(ctx);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.DsvCpu();
    cmd->OMSetRenderTargets(1, &hdrRtv, FALSE, &dsv);
    const float clearCol[4] = { 0, 0, 0, 1 };
    cmd->ClearRenderTargetView(hdrRtv, clearCol, 0, nullptr);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); // reversed-Z

    buoy.Draw(ctx, frameCB, camera.pos);
    meteor.DrawRock(ctx, frameCB, camera.pos);
    ocean.Draw(ctx, frameCB);
    sky.Draw(ctx, frameCB);
    buoy.DrawGlow(ctx, frameCB, camera.pos);
    meteor.DrawTrail(ctx, frameCB);

    post.Record(ctx, RtvSlot::Backbuffer0 + bbIdx, uiFxaa,
        exposureBase * uiExposureMul, uiBloom, 1.15f, 0.32f);

    // --- UI directly on the backbuffer ---
    D3D12_CPU_DESCRIPTOR_HANDLE bbRtv = ctx.RtvCpu(RtvSlot::Backbuffer0 + bbIdx);
    cmd->OMSetRenderTargets(1, &bbRtv, FALSE, nullptr);
    if (opts.benchFrames == 0 || opts.showUi)
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        BuildUi(dt);
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    ctx.Transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ctx.EndFrame(uiVsync && opts.benchFrames == 0);
}

void App::BuildUi(float dt)
{
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Rough Open Ocean", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("%.1f FPS  (%.2f ms)", fpsDisplay, fpsDisplay > 0 ? 1000.0f / fpsDisplay : 0.0f);
    ImGui::Separator();

    // Level of detail.
    const char* lodNames[kNumLods];
    for (int i = 0; i < kNumLods; ++i)
        lodNames[i] = kLods[i].name;
    if (ImGui::Combo("Level of detail", &uiLod, lodNames, kNumLods))
    {
        // applied at the top of the next frame
    }

    // Time of day.
    const char* timeNames[kNumTimes];
    for (int i = 0; i < kNumTimes; ++i)
        timeNames[i] = kTimes[i].name;
    if (ImGui::Combo("Time of day", &uiTimeOfDay, timeNames, kNumTimes))
    {
        UpdateLighting();
        sky.MarkDirty();
    }

    // Sea state.
    if (ImGui::SliderInt("Sea state", &uiSeaState, 0, 9, kSeas[uiSeaState].name))
    {
        ApplySeaState();
        spectrumDirty = true;
    }

    // Meteorite strike.
    {
        bool disabled = meteor.Flying();
        if (disabled)
            ImGui::BeginDisabled();
        if (ImGui::Button("Meteorite", ImVec2(-1, 0)))
            meteor.Launch(camera, uiMeteorPower);
        if (disabled)
            ImGui::EndDisabled();
        ImGui::SliderFloat("Impact power", &uiMeteorPower, 1.0f, 8.0f, "%.1f m");
    }
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Waves", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::SliderFloat("Wind direction", &uiWindDirDeg, 0.0f, 360.0f, "%.0f deg"))
        {
            ApplySeaState();
            spectrumDirty = true;
        }
        if (ImGui::SliderFloat("Wave height", &uiAmpMul, 0.2f, 2.0f, "%.2fx"))
        {
            ApplySeaState();
            spectrumDirty = true;
        }
        if (ImGui::SliderFloat("Choppiness", &uiChopMul, 0.0f, 1.6f, "%.2fx"))
            ApplySeaState();
        if (ImGui::SliderFloat("Foam", &uiFoamMul, 0.5f, 1.0f, "%.2fx"))
            ApplySeaState();
        ImGui::SliderFloat("Wave speed", &uiTimeScale, 0.0f, 2.0f, "%.2fx");
    }

    if (ImGui::CollapsingHeader("Rendering"))
    {
        ImGui::SliderFloat("Exposure", &uiExposureMul, 0.3f, 3.0f, "%.2fx");
        ImGui::SliderFloat("Bloom", &uiBloom, 0.0f, 0.3f, "%.3f");
        if (ImGui::SliderFloat("Cirrus clouds", &uiCloudCover, 0.0f, 1.0f, "%.2f"))
        {
            UpdateLighting();
            sky.MarkDirty();
        }
        ImGui::Checkbox("FXAA", &uiFxaa);
        ImGui::SameLine();
        ImGui::Checkbox("VSync", &uiVsync);
    }

    if (ImGui::CollapsingHeader("Buoy"))
    {
        ImGui::SliderFloat("Flash period", &buoy.flashPeriod, 0.6f, 6.0f, "%.1f s");
        ImGui::SliderFloat("Flash duration", &buoy.flashDuration, 0.1f, 1.5f, "%.2f s");
        ImGui::SliderFloat("Lamp intensity", &buoy.lampIntensity, 2.0f, 120.0f, "%.0f");
    }

    if (ImGui::CollapsingHeader("Sound"))
    {
        if (audio.Available())
        {
            const char* modes[] = { "Off", "Soothing (recorded)", "Realistic (synthesized)" };
            ImGui::Combo("Mode", &uiSoundMode, modes, 3);
            if (uiSoundMode == int(SoundMode::Soothing) && !audio.RecordingLoaded())
                ImGui::TextDisabled("Recording missing - using synthesized sound.");
            ImGui::SliderFloat("Volume", &uiVolume, 0.0f, 1.0f, "%.2f");
            ImGui::TextDisabled("Loudness follows the sea state.");
        }
        else
        {
            ImGui::TextDisabled("No audio output device.");
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("RMB drag: look around  |  WASD/QE: move");
    ImGui::TextDisabled("Shift: fast  |  Wheel: speed  |  M: meteorite  |  Esc: quit");
    ImGui::End();
}

LRESULT App::HandleMsg(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(wnd, msg, wp, lp))
        return 1;
    ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;

    switch (msg)
    {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && ctx.device && LOWORD(lp) > 0 && HIWORD(lp) > 0)
        {
            uint32_t w = LOWORD(lp), h = HIWORD(lp);
            if (w != ctx.width || h != ctx.height)
            {
                ctx.Resize(w, h);
                post.Create(ctx, w, h);
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (!io || !io->WantCaptureMouse)
        {
            mouseLook = true;
            SetCapture(wnd);
            lastMouse = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        }
        return 0;
    case WM_RBUTTONUP:
        mouseLook = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (mouseLook)
        {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            camera.AddLook((p.x - lastMouse.x) * 0.0032f, (p.y - lastMouse.y) * 0.0032f);
            lastMouse = p;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (!io || !io->WantCaptureMouse)
        {
            float steps = GET_WHEEL_DELTA_WPARAM(wp) / float(WHEEL_DELTA);
            camera.moveSpeed = std::clamp(camera.moveSpeed * std::pow(1.25f, steps), 0.5f, 500.0f);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            PostQuitMessage(0);
        if (wp == 'M' && (!io || !io->WantCaptureKeyboard))
            meteor.Launch(camera, uiMeteorPower);
        if (wp >= '1' && wp <= '0' + kNumTimes && (!io || !io->WantCaptureKeyboard))
        {
            uiTimeOfDay = int(wp - '1');
            UpdateLighting();
            sky.MarkDirty();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Screenshot: copy the current backbuffer to a readback buffer and encode PNG.
// ---------------------------------------------------------------------------
void App::SaveBackbufferPng(const std::wstring& path)
{
    ctx.WaitIdle();
    uint32_t bbIdx = ctx.swapchain->GetCurrentBackBufferIndex();
    // The last presented image is the previous index in the flip chain.
    bbIdx = (bbIdx + GpuContext::kBackbuffers - 1) % GpuContext::kBackbuffers;
    ID3D12Resource* src = ctx.backbuffers[bbIdx].Get();

    uint32_t rowPitch = AlignUp<uint32_t>(ctx.width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    ComPtr<ID3D12Resource> rb = ctx.CreateBuffer(uint64_t(rowPitch) * ctx.height,
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"ShotRB");

    ctx.BeginOneShot();
    ctx.Transition(src, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION s = {};
    s.pResource = src;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION d = {};
    d.pResource = rb.Get();
    d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.PlacedFootprint.Footprint.Width = ctx.width;
    d.PlacedFootprint.Footprint.Height = ctx.height;
    d.PlacedFootprint.Footprint.Depth = 1;
    d.PlacedFootprint.Footprint.RowPitch = rowPitch;
    ctx.Cmd()->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    ctx.Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    ctx.EndOneShot();

    void* data = nullptr;
    D3D12_RANGE all = { 0, uint64_t(rowPitch) * ctx.height };
    HR(rb->Map(0, &all, &data));

    // Repack tightly as BGRA (the PNG encoder's native layout; SetPixelFormat
    // may silently negotiate away from RGBA, so don't rely on it).
    std::vector<BYTE> bgra(size_t(ctx.width) * ctx.height * 4);
    for (uint32_t y = 0; y < ctx.height; ++y)
    {
        const BYTE* srcRow = static_cast<const BYTE*>(data) + size_t(y) * rowPitch;
        BYTE* dstRow = bgra.data() + size_t(y) * ctx.width * 4;
        for (uint32_t x = 0; x < ctx.width; ++x)
        {
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
            dstRow[x * 4 + 3] = 255;
        }
    }
    rb->Unmap(0, nullptr);

    ComPtr<IWICImagingFactory> wic;
    HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    ComPtr<IWICStream> stream;
    HR(wic->CreateStream(&stream));
    HR(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> enc;
    HR(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc));
    HR(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;
    HR(enc->CreateNewFrame(&frame, nullptr));
    HR(frame->Initialize(nullptr));
    HR(frame->SetSize(ctx.width, ctx.height));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    HR(frame->SetPixelFormat(&fmt));
    if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA))
        throw std::runtime_error("PNG encoder refused 32bppBGRA");
    HR(frame->WritePixels(ctx.height, ctx.width * 4, ctx.width * 4 * ctx.height, bgra.data()));
    HR(frame->Commit());
    HR(enc->Commit());
    LogF("Saved screenshot: %ls\n", path.c_str());
}

// ---------------------------------------------------------------------------
// GPU FFT self-test against a CPU reference DFT (N=64).
// ---------------------------------------------------------------------------
int App::RunFFTSelfTest()
{
    LogF("Running FFT self-test...\n");
    ctx.Init(nullptr, 64, 64, false);

    const uint32_t N = 64;
    D3D_SHADER_MACRO macros[] = { {"FFT_SIZE", "64"}, {"FFT_LOG2", "6"}, {nullptr, nullptr} };
    ComPtr<ID3D12PipelineState> pso = ctx.CreateComputePso(
        ctx.CompileShader(ctx.ShaderPath(L"OceanSim.hlsl"), "CSFFT", "cs_5_0", macros).Get(), L"TestFFT");

    // Deterministic pseudorandom input: 4 complex planes across two buffers.
    std::vector<XMFLOAT4> in0(N * N), in1(N * N);
    uint32_t rng = 22222;
    auto frand = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return (float(rng >> 8) / float(1 << 24)) * 2.0f - 1.0f;
    };
    for (uint32_t i = 0; i < N * N; ++i)
    {
        in0[i] = XMFLOAT4(frand(), frand(), frand(), frand());
        in1[i] = XMFLOAT4(frand(), frand(), frand(), frand());
    }

    const uint64_t bytes = uint64_t(N) * N * 16;
    ComPtr<ID3D12Resource> buf0 = ctx.CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, L"T0");
    ComPtr<ID3D12Resource> buf1 = ctx.CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, L"T1");
    ComPtr<ID3D12Resource> stage = ctx.CreateBuffer(bytes * 2, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"TS");
    ComPtr<ID3D12Resource> rb = ctx.CreateBuffer(bytes * 2, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"TR");

    {
        void* p = nullptr;
        D3D12_RANGE nr = { 0, 0 };
        HR(stage->Map(0, &nr, &p));
        memcpy(p, in0.data(), bytes);
        memcpy(static_cast<uint8_t*>(p) + bytes, in1.data(), bytes);
        stage->Unmap(0, nullptr);
    }

    ctx.BeginOneShot();
    ctx.Cmd()->CopyBufferRegion(buf0.Get(), 0, stage.Get(), 0, bytes);
    ctx.Cmd()->CopyBufferRegion(buf1.Get(), 0, stage.Get(), bytes, bytes);
    ctx.Transition(buf0.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Transition(buf1.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ctx.Cmd()->SetComputeRootSignature(ctx.computeRS.Get());
    ctx.Cmd()->SetPipelineState(pso.Get());
    ctx.Cmd()->SetComputeRootUnorderedAccessView(3, buf0->GetGPUVirtualAddress());
    ctx.Cmd()->SetComputeRootUnorderedAccessView(4, buf1->GetGPUVirtualAddress());
    for (uint32_t dir = 0; dir < 2; ++dir)
    {
        struct { uint32_t stuff[20]; } cbData = {};
        memset(&cbData, 0, sizeof(cbData));
        cbData.stuff[17] = dir; // SimCB.gFFTDir is the 18th 32-bit field
        void* p = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS va = ctx.AllocUpload(sizeof(cbData), &p);
        memcpy(p, &cbData, sizeof(cbData));
        ctx.Cmd()->SetComputeRootConstantBufferView(0, va);
        ctx.Cmd()->Dispatch(N, 2, 1);
        ctx.UavBarrier(buf0.Get());
        ctx.UavBarrier(buf1.Get());
    }
    ctx.Transition(buf0.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.Transition(buf1.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ctx.Cmd()->CopyBufferRegion(rb.Get(), 0, buf0.Get(), 0, bytes);
    ctx.Cmd()->CopyBufferRegion(rb.Get(), bytes, buf1.Get(), 0, bytes);
    ctx.EndOneShot();

    // CPU reference: unnormalized inverse DFT (e^{+i}) on each complex plane,
    // rows then columns.
    using cplx = std::complex<double>;
    auto dft2d = [&](std::vector<cplx>& a)
    {
        std::vector<cplx> tmp(N);
        for (uint32_t row = 0; row < N; ++row)
        {
            for (uint32_t n = 0; n < N; ++n)
            {
                cplx acc = 0;
                for (uint32_t k = 0; k < N; ++k)
                    acc += a[row * N + k] * std::exp(cplx(0, 2.0 * 3.14159265358979323846 * k * n / N));
                tmp[n] = acc;
            }
            for (uint32_t n = 0; n < N; ++n)
                a[row * N + n] = tmp[n];
        }
        for (uint32_t col = 0; col < N; ++col)
        {
            for (uint32_t n = 0; n < N; ++n)
            {
                cplx acc = 0;
                for (uint32_t k = 0; k < N; ++k)
                    acc += a[k * N + col] * std::exp(cplx(0, 2.0 * 3.14159265358979323846 * k * n / N));
                tmp[n] = acc;
            }
            for (uint32_t n = 0; n < N; ++n)
                a[n * N + col] = tmp[n];
        }
    };

    std::vector<cplx> planes[4];
    for (int pl = 0; pl < 4; ++pl)
        planes[pl].resize(N * N);
    for (uint32_t i = 0; i < N * N; ++i)
    {
        planes[0][i] = cplx(in0[i].x, in0[i].y);
        planes[1][i] = cplx(in0[i].z, in0[i].w);
        planes[2][i] = cplx(in1[i].x, in1[i].y);
        planes[3][i] = cplx(in1[i].z, in1[i].w);
    }
    for (auto& pl : planes)
        dft2d(pl);

    const XMFLOAT4* out;
    void* mapped = nullptr;
    D3D12_RANGE all = { 0, bytes * 2 };
    HR(rb->Map(0, &all, &mapped));
    out = static_cast<const XMFLOAT4*>(mapped);

    double maxErr = 0, maxMag = 0;
    for (uint32_t i = 0; i < N * N; ++i)
    {
        double vals[8] = {
            out[i].x, out[i].y, out[i].z, out[i].w,
            out[N * N + i].x, out[N * N + i].y, out[N * N + i].z, out[N * N + i].w,
        };
        double refs[8] = {
            planes[0][i].real(), planes[0][i].imag(), planes[1][i].real(), planes[1][i].imag(),
            planes[2][i].real(), planes[2][i].imag(), planes[3][i].real(), planes[3][i].imag(),
        };
        for (int k = 0; k < 8; ++k)
        {
            maxErr = std::max(maxErr, std::abs(vals[k] - refs[k]));
            maxMag = std::max(maxMag, std::abs(refs[k]));
        }
    }
    rb->Unmap(0, nullptr);

    double rel = maxErr / std::max(maxMag, 1e-9);
    bool pass = rel < 2e-4;
    LogF("FFT self-test: max abs err %.3g (rel %.3g, max magnitude %.3g) -> %s\n",
         maxErr, rel, maxMag, pass ? "PASS" : "FAIL");
    ctx.Shutdown();
    return pass ? 0 : 1;
}
