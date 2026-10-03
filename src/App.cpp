#include "App.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>
#include <wincodec.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace DirectX;

namespace
{
    ImFont* hintFont = nullptr;
    ImFont* hintFontLarge = nullptr;
    ImFont* menuFontLarge = nullptr;
    HCURSOR enlargedCursor = nullptr;
    // 2x keeps ProggyClean on its pixel grid and is enough to read on a 4K display.
    constexpr float kMenuEnlarge = 2.0f;

    // A 2x copy of the system arrow. The hotspot scales with the image so clicks
    // still land on the tip.
    HCURSOR MakeEnlargedArrow(int scale)
    {
        HCURSOR src = LoadCursorW(nullptr, IDC_ARROW);
        ICONINFO ii = {};
        if (!src || !GetIconInfo(src, &ii) || !ii.hbmColor)
        {
            if (ii.hbmMask) DeleteObject(ii.hbmMask);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            int w = GetSystemMetrics(SM_CXCURSOR) * scale;
            int h = GetSystemMetrics(SM_CYCURSOR) * scale;
            return (HCURSOR)CopyImage(src, IMAGE_CURSOR, w, h, 0);
        }

        BITMAP bm = {};
        GetObject(ii.hbmColor, sizeof(bm), &bm);
        const int srcW = bm.bmWidth;
        const int srcH = bm.bmHeight;
        const int dstW = srcW * scale;
        const int dstH = srcH * scale;

        HDC hdc = GetDC(nullptr);
        BITMAPINFO srcInfo = {};
        srcInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        srcInfo.bmiHeader.biWidth = srcW;
        srcInfo.bmiHeader.biHeight = srcH; // bottom-up, matches GetDIBits
        srcInfo.bmiHeader.biPlanes = 1;
        srcInfo.bmiHeader.biBitCount = 32;
        srcInfo.bmiHeader.biCompression = BI_RGB;

        std::vector<uint32_t> srcPx(size_t(srcW) * srcH);
        int rows = GetDIBits(hdc, ii.hbmColor, 0, srcH, srcPx.data(), &srcInfo, DIB_RGB_COLORS);

        std::vector<uint32_t> dstPx(size_t(dstW) * dstH);
        if (rows > 0)
        {
            for (int y = 0; y < dstH; ++y)
            {
                int sy = std::min(y / scale, srcH - 1);
                for (int x = 0; x < dstW; ++x)
                {
                    int sx = std::min(x / scale, srcW - 1);
                    dstPx[size_t(y) * dstW + x] = srcPx[size_t(sy) * srcW + sx];
                }
            }
        }

        BITMAPINFO dstInfo = srcInfo;
        dstInfo.bmiHeader.biWidth = dstW;
        dstInfo.bmiHeader.biHeight = -dstH; // top-down DIB section; pixels are stored top-down below
        // Rebuild top-down so the DIB section matches a negative height.
        std::vector<uint32_t> topDown(dstPx.size());
        for (int y = 0; y < dstH; ++y)
            memcpy(&topDown[size_t(y) * dstW], &dstPx[size_t(dstH - 1 - y) * dstW], size_t(dstW) * 4);

        void* colorBits = nullptr;
        HBITMAP color = CreateDIBSection(hdc, &dstInfo, DIB_RGB_COLORS, &colorBits, nullptr, 0);
        if (colorBits)
            memcpy(colorBits, topDown.data(), topDown.size() * 4);

        const int maskStride = ((dstW + 31) / 32) * 4;
        std::vector<uint8_t> maskPx(size_t(maskStride) * dstH, 0xFF);
        BITMAPINFO mbi = {};
        mbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        mbi.bmiHeader.biWidth = dstW;
        mbi.bmiHeader.biHeight = -dstH;
        mbi.bmiHeader.biPlanes = 1;
        mbi.bmiHeader.biBitCount = 1;
        mbi.bmiHeader.biCompression = BI_RGB;
        void* maskBits = nullptr;
        HBITMAP mask = CreateDIBSection(hdc, &mbi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
        if (maskBits)
            memcpy(maskBits, maskPx.data(), maskPx.size());

        ICONINFO out = {};
        out.fIcon = FALSE;
        out.xHotspot = ii.xHotspot * scale;
        out.yHotspot = ii.yHotspot * scale;
        out.hbmMask = mask;
        out.hbmColor = color;
        HCURSOR cursor = (HCURSOR)CreateIconIndirect(&out);

        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        ReleaseDC(nullptr, hdc);
        return cursor;
    }

    void ApplyEnlargedCursor(HWND hwnd, bool enlarged)
    {
        static bool wasEnlarged = false;
        if (enlarged && enlargedCursor)
        {
            POINT pt;
            if (GetCursorPos(&pt) && WindowFromPoint(pt) == hwnd)
                SetCursor(enlargedCursor);
            wasEnlarged = true;
        }
        else if (wasEnlarged)
        {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            wasEnlarged = false;
        }
    }

    struct TimePreset
    {
        const char* name;
        float sunElev, sunAz;   // degrees; azimuth 0 = straight ahead (+Z)
        float moonElev, moonAz;
        float exposure;
        float haze = 1.0f;      // aerosol (Mie) turbidity multiplier
        float lowSun = 0.0f;    // horizon-sun optics: ozone, refraction, scintillation
    };
    const TimePreset kTimes[] = {
        { "Early morning", 3.5f, 14.0f, -40.0f, 0.0f, 1.35f },
        { "Morning", 24.0f, 38.0f, -40.0f, 0.0f, 0.85f },
        { "Noon", 62.0f, 20.0f, -40.0f, 0.0f, 0.75f },
        { "Afternoon", 35.0f, -26.0f, -40.0f, 0.0f, 0.85f },
        { "Evening", 8.0f, 8.0f, -40.0f, 0.0f, 1.25f },
        // Disc centre on the horizon: half the sun has set. Its light crosses
        // ~40 air masses of hazy maritime air, which is where the red comes from.
        { "Sunset", 0.0f, 3.0f, -40.0f, 0.0f, 0.8f, 3.0f, 1.0f },
        { "Late evening", -5.5f, 5.0f, 21.0f, -22.0f, 3.0f },
        { "Night", -30.0f, 0.0f, 43.0f, 12.0f, 5.0f },
    };
    constexpr int kNumTimes = int(std::size(kTimes));

    // Apparent radius of the drawn sun disc (Sky.hlsl kSunDiscR), degrees.
    // Twice the real 0.27 deg, matching the soft disc of the other presets.
    constexpr float kSunDiscRadiusDeg = 0.49f;

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

    // Extinction the clear-sky Transmittance() leaves out, integrated through
    // the same spherical atmosphere Sky.hlsl marches (so the sun and the sky
    // around it redden together): aerosol beyond the baseline haze ((haze - 1)
    // x BetaM, 1.2 km scale height, Angstrom exponent 1.3 = Sky.hlsl
    // kHazeSpectrum) and ozone's Chappuis band (tent layer at 10-40 km). Both
    // are negligible for a high sun; at the horizon the path through them is
    // hundreds of kilometres long.
    XMFLOAT3 ExtraTransmittance(float elevDeg, float haze, float ozone)
    {
        if (haze <= 1.0f && ozone <= 0.0f)
            return XMFLOAT3(1.0f, 1.0f, 1.0f);
        const double Re = 6371e3, Ra = 6431e3, Hm = 1200.0;
        const double hazeSpectrum[3] = { 0.76, 1.0, 1.34 };
        const double betaM = 4.5e-6 * 1.1 * (haze - 1.0);
        const double betaO[3] = { 0.650e-6 * ozone, 1.881e-6 * ozone, 0.085e-6 * ozone };
        double e = XMConvertToRadians(std::max(elevDeg, 0.0f));
        double dx = std::cos(e), dy = std::sin(e);
        double oy = Re + 30.0; // eye height of the sky march
        double b = oy * dy;
        double tMax = -b + std::sqrt(b * b - (oy * oy - Ra * Ra));
        const int kSteps = 2000;
        double dt = tMax / kSteps, odM = 0.0, odO = 0.0;
        for (int i = 0; i < kSteps; ++i)
        {
            double t = (i + 0.5) * dt;
            double px = dx * t, py = oy + dy * t;
            double h = std::sqrt(px * px + py * py) - Re;
            odM += std::exp(-h / Hm) * dt;
            odO += std::max(0.0, 1.0 - std::abs(h - 25000.0) / 15000.0) * dt;
        }
        return XMFLOAT3(float(std::exp(-betaM * hazeSpectrum[0] * odM - betaO[0] * odO)),
                        float(std::exp(-betaM * hazeSpectrum[1] * odM - betaO[1] * odO)),
                        float(std::exp(-betaM * hazeSpectrum[2] * odM - betaO[2] * odO)));
    }

    XMFLOAT3 Mul(XMFLOAT3 a, XMFLOAT3 b) { return XMFLOAT3(a.x * b.x, a.y * b.y, a.z * b.z); }

    // Fraction of a disc of angular radius r whose centre sits e above the horizon.
    float DiscVisibleFraction(float e, float r)
    {
        float x = std::clamp(e / r, -1.0f, 1.0f);
        return 1.0f - (std::acos(x) - x * std::sqrt(1.0f - x * x)) / XM_PI;
    }

    // Bennett's astronomical refraction for an apparent elevation h, degrees.
    float RefractionDeg(float h)
    {
        return 1.0f / (60.0f * std::tan(XMConvertToRadians(h + 7.31f / (h + 4.4f))));
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
    uiWhirl = opts.whirl;
    uiWhirlPreset = std::clamp(opts.whirlPreset, 0, Whirlpool::PresetCount() - 1);

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

        // The menu button is handled inside RenderFrame, while the backbuffer is
        // still bound. SetWindowPos would send WM_SIZE and ResizeBuffers would
        // throw, so apply the toggle here, same as F11, before the next frame.
        if (requestFullscreenToggle)
        {
            requestFullscreenToggle = false;
            ToggleFullscreen();
        }

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
            if (opts.clickMeteorX >= 0)
                LaunchMeteorAt(opts.clickMeteorX, opts.clickMeteorY);
            else
                LaunchMeteor();
            meteorAutoLaunched = true;
        }
        if (opts.whirlAt >= 0.0f && whirlAutoSpawned < opts.whirlCount
            && simTime >= opts.whirlAt + whirlAutoSpawned * opts.whirlGap)
        {
            if (opts.clickWhirlX >= 0)
                SpawnWhirlpoolAt(opts.clickWhirlX, opts.clickWhirlY);
            else
                SpawnWhirlpool();
            ++whirlAutoSpawned;
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
    if (enlargedCursor)
    {
        DestroyCursor(enlargedCursor);
        enlargedCursor = nullptr;
    }
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
    UpdateWindowTitle(opts.width, opts.height);
    ShowWindow(hwnd, SW_SHOW);
    enlargedCursor = MakeEnlargedArrow(int(kMenuEnlarge));
}

void App::UpdateWindowTitle(uint32_t w, uint32_t h)
{
    if (!hwnd || w == 0 || h == 0)
        return;
    wchar_t title[96];
    swprintf_s(title, L"Rough Open Ocean - DirectX 12 (%u x %u)", w, h);
    SetWindowTextW(hwnd, title);
}

void App::ToggleFullscreen()
{
    if (!hwnd || IsIconic(hwnd))
        return;

    if (!fullscreen)
    {
        windowedPlacement = {};
        windowedPlacement.length = sizeof(windowedPlacement);
        if (!GetWindowPlacement(hwnd, &windowedPlacement))
            return;
        windowedStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);

        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi))
            return;

        // Popup covering the monitor rectangle hides the caption and the taskbar.
        SetWindowLongPtrW(hwnd, GWL_STYLE, (windowedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        fullscreen = true;
    }
    else
    {
        SetWindowLongPtrW(hwnd, GWL_STYLE, windowedStyle);
        SetWindowPlacement(hwnd, &windowedPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = false;
    }
}

void App::InitSystems()
{
    ctx.Init(hwnd, opts.width, opts.height);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    ImFontConfig bigCfg;
    bigCfg.SizePixels = 13.0f * kMenuEnlarge;
    bigCfg.OversampleH = bigCfg.OversampleV = 1;
    bigCfg.PixelSnapH = true;
    menuFontLarge = io.Fonts->AddFontDefault(&bigCfg);
    // Narrow face so the two-line control hint fits the panel. The large
    // copy is the same face at the ENLARGE scale.
    hintFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\ARIALN.TTF", 16.0f);
    hintFontLarge = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\ARIALN.TTF", 16.0f * kMenuEnlarge);
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
    oceanParams.smallCut = uiSmallCutCm * 0.01f;
    oceanParams.swellAmp = uiSwellAmp;
    oceanParams.swellLambda = uiSwellLambda;
    // Swell arrives from a distant weather system, not the local wind: offset
    // its direction 30 degrees so the two wave trains visibly cross.
    float sa = XMConvertToRadians(uiWindDirDeg + 30.0f);
    oceanParams.swellDir = XMFLOAT2(std::sin(sa), std::cos(sa));
    ampEstimate = sp.ampEst * uiAmpMul + 0.6f * uiSwellAmp;
}

void App::UpdateLighting()
{
    const TimePreset& tp = kTimes[uiTimeOfDay];
    XMFLOAT3 sunDir = DirFromElevAz(tp.sunElev, tp.sunAz);
    XMFLOAT3 moonDir = DirFromElevAz(tp.moonElev, tp.moonAz);
    // The moon takes over only once the whole sun disc is below the horizon.
    bool moonPrimary = tp.sunElev < -kSunDiscRadiusDeg;

    XMFLOAT3 tSun = Mul(Transmittance(tp.sunElev), ExtraTransmittance(tp.sunElev, tp.haze, tp.lowSun));
    XMFLOAT3 tMoon = Transmittance(tp.moonElev);

    sunShimmer = 0.0f;
    sunFlatten = 1.0f;
    sunTauGrad = XMFLOAT3(0.0f, 0.0f, 0.0f);

    const float sunPower = 24.0f;
    if (!moonPrimary)
    {
        lightDir = sunDir;
        lightIsMoon = 0.0f;
        // A sun cut by the horizon lights the sea with only its visible part.
        float vis = DiscVisibleFraction(tp.sunElev, kSunDiscRadiusDeg);
        lightColor = XMFLOAT3(sunPower * vis * tSun.x, sunPower * vis * tSun.y, sunPower * vis * tSun.z);
        // At the horizon the disc is dimmed so far that it is exposed close to
        // the sky around it: a golden-orange disc, not a clipped white blob.
        float disc = 1.0f + (0.35f - 1.0f) * tp.lowSun;
        sunDiscColor = XMFLOAT3(420.0f * disc * tSun.x, 415.0f * disc * tSun.y, 405.0f * disc * tSun.z);

        if (tp.lowSun > 0.0f)
        {
            // Refraction lifts the lower limb more than the upper one: the
            // apparent disc is squashed vertically by 1 - dR/dh.
            float r = kSunDiscRadiusDeg;
            sunFlatten = 1.0f - (RefractionDeg(tp.sunElev) - RefractionDeg(tp.sunElev + r)) / r;
            // Air mass falls steeply across the disc: its upper limb shines
            // brighter and yellower than the red lower edge. Optical-depth
            // change per disc radius, upward.
            XMFLOAT3 tUp = Mul(Transmittance(tp.sunElev + r), ExtraTransmittance(tp.sunElev + r, tp.haze, tp.lowSun));
            auto dTau = [](float up, float c) { return std::log(std::max(up, 1e-30f) / std::max(c, 1e-30f)); };
            sunTauGrad = XMFLOAT3(dTau(tUp.x, tSun.x), dTau(tUp.y, tSun.y), dTau(tUp.z, tSun.z));
            sunShimmer = tp.lowSun;
        }
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
    skyParams.haze = tp.haze;
    skyParams.ozone = tp.lowSun;
    skyParams.cloudSunlit = tp.lowSun;

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

void App::LaunchMeteor()
{
    meteor.Launch(camera, uiMeteorPower);
    audio.PlayMeteor(MeteorSfx::Descent); // whoosh while it streaks down
}

// Unproject a screen pixel onto the mean water plane y = 0.
bool App::PickWater(int mouseX, int mouseY, XMFLOAT3& hit) const
{
    // Pixel -> normalized device coords.
    float ndcX = (mouseX + 0.5f) / float(ctx.width) * 2.0f - 1.0f;
    float ndcY = 1.0f - (mouseY + 0.5f) / float(ctx.height) * 2.0f;

    // World-space view ray (ViewProj is camera-relative, so the unprojected
    // point is already a direction from the eye).
    float aspect = float(ctx.width) / float(ctx.height);
    XMMATRIX ivp = XMMatrixInverse(nullptr, camera.ViewProj(aspect));
    XMVECTOR pw = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0.5f, 1.0f), ivp);
    XMFLOAT3 d;
    XMStoreFloat3(&d, XMVector3Normalize(XMVectorScale(pw, 1.0f / XMVectorGetW(pw))));

    // Intersect the mean water plane y = 0. Any ray dipping below the horizon
    // hits it; only bail if the click is at or above the horizon line. Near the
    // horizon the intersection races off to infinity, so clamp it to the
    // visible sea (still far, still near the horizon on screen).
    if (d.y >= -1e-4f)
        return false;
    float t = std::min(-camera.pos.y / d.y, 25000.0f);
    hit = XMFLOAT3(camera.pos.x + d.x * t, 0.0f, camera.pos.z + d.z * t);
    return true;
}

// Left-click the water to drop the meteorite exactly there. The incoming side
// is mirrored from the clicked screen half: click the left of the view and it
// streaks in from the right of the sky, and vice versa.
void App::LaunchMeteorAt(int mouseX, int mouseY)
{
    if (meteor.Flying())
        return;

    XMFLOAT3 target;
    if (!PickWater(mouseX, mouseY, target))
        return;
    float ndcX = (mouseX + 0.5f) / float(ctx.width) * 2.0f - 1.0f;

    // Camera "right" in the horizontal plane (points to screen-right).
    XMFLOAT3 r;
    XMStoreFloat3(&r, XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), camera.Forward())));
    float rl = std::sqrt(r.x * r.x + r.z * r.z);
    if (rl < 1e-4f)
        return;
    // Left half (ndcX < 0) -> come from the right -> travel left (-right); mirror otherwise.
    float sign = (ndcX < 0.0f) ? -1.0f : 1.0f;
    XMFLOAT3 approach(r.x / rl * sign, 0.0f, r.z / rl * sign);

    meteor.LaunchAt(target, approach, uiMeteorPower);
    audio.PlayMeteor(MeteorSfx::Descent);
}

// Whirlpool UI button / V key / --whirl hook: open the drain a fixed way
// ahead of the camera (deterministic, so verification runs are repeatable).
void App::SpawnWhirlpool()
{
    // Big vortices need room to read; small ones keep the close default view.
    float rc = Whirlpool::CoreRadius(uiWhirl);
    float dist = std::max(65.0f, 8.0f * rc);
    // Fan repeat presses out sideways so they overlap rather than stack: the
    // first lands dead ahead, the rest alternate left and right of it.
    uint32_t n = whirlpool.ActiveCount();
    float lateral = 0.0f;
    if (n > 0)
        lateral = ((n & 1u) ? 1.0f : -1.0f) * float((n + 1) / 2) * 2.5f * rc;

    float cy = std::cos(camera.yaw), sy = std::sin(camera.yaw);
    XMFLOAT3 target(camera.pos.x + sy * dist + cy * lateral, 0.0f,
                    camera.pos.z + cy * dist - sy * lateral);
    whirlpool.Spawn(target, uiWhirl);
}

// Left-click the water in whirlpool mode: the vortex forms exactly there.
void App::SpawnWhirlpoolAt(int mouseX, int mouseY)
{
    XMFLOAT3 target;
    if (!PickWater(mouseX, mouseY, target))
        return;
    whirlpool.Spawn(target, uiWhirl);
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
    // Scintillation: a horizon sun is seen through ~40 air masses of turbulent
    // air, so its light breathes by a few percent. Incommensurate slow tones,
    // on sim time so verification runs stay deterministic.
    float twinkle = 1.0f;
    if (sunShimmer > 0.0f)
    {
        float n = 0.5f * std::sin(2.31f * simTime + 1.3f)
                + 0.3f * std::sin(5.17f * simTime + 0.4f)
                + 0.2f * std::sin(10.7f * simTime + 2.1f);
        twinkle = 1.0f + 0.035f * sunShimmer * n;
    }
    cb.lightColor = XMFLOAT3(lightColor.x * twinkle, lightColor.y * twinkle, lightColor.z * twinkle);
    cb.roughBase = 0.055f;
    cb.sunDiscColor = XMFLOAT3(sunDiscColor.x * twinkle, sunDiscColor.y * twinkle, sunDiscColor.z * twinkle);
    cb.sunFx = XMFLOAT4(sunShimmer, sunFlatten, 0.0f, 0.0f);
    cb.sunTauGrad = XMFLOAT4(sunTauGrad.x, sunTauGrad.y, sunTauGrad.z, 0.0f);
    cb.lambda = oceanParams.choppiness;

    float wf = std::clamp(oceanParams.U10 / 12.0f, 0.05f, 1.2f);
    cb.cascade0 = XMFLOAT4(1.0f / ocean.CascadeLength(0), 1e8f, 0.0f, ampEstimate);
    cb.cascade1 = XMFLOAT4(1.0f / ocean.CascadeLength(1), lp.fade1, 0.065f * wf, float(ocean.FftN()));
    cb.cascade2 = XMFLOAT4(1.0f / ocean.CascadeLength(2), lp.fade2, 0.100f * wf, 0);

    cb.waterDeep = XMFLOAT3(0.003f, 0.013f, 0.026f);
    cb.foamAmount = 1.0f;
    cb.waterScatter = XMFLOAT3(0.010f, 0.062f, 0.080f);
    if (sunShimmer > 0.0f)
    {
        // A horizon sun shines sideways through the thin crest tops (~0.6 m of
        // water) rather than down a long scattering path, so far less of its
        // red is absorbed: pure-water absorption 0.45 / 0.064 / 0.0065 per m
        // at 680 / 550 / 440 nm. Backlit crests glow ember-red.
        const XMFLOAT3 thin(0.080f * std::exp(-0.45f * 0.6f), 0.080f * std::exp(-0.064f * 0.6f),
                            0.080f * std::exp(-0.0065f * 0.6f));
        cb.waterScatter = XMFLOAT3(cb.waterScatter.x + (thin.x - cb.waterScatter.x) * sunShimmer,
                                   cb.waterScatter.y + (thin.y - cb.waterScatter.y) * sunShimmer,
                                   cb.waterScatter.z + (thin.z - cb.waterScatter.z) * sunShimmer);
    }
    cb.sss = 0.45f;

    XMFLOAT3 lamp = buoy.LampWorldPos();
    cb.buoyLightPos = XMFLOAT3(lamp.x - camera.pos.x, lamp.y - camera.pos.y, lamp.z - camera.pos.z);
    cb.buoyLightOn = buoy.LightOn() ? 2.5f : 0.0f;
    cb.buoyLightColor = buoy.LightColor();
    cb.fogDensity = 1.0e-4f;
    cb.windDir = oceanParams.windDir;
    // Filtered slope variance now supplies most of the distance roughness
    // physically; this is only a gentle artistic floor on top.
    cb.distRough = 0.10f;
    cb.gridScale = ocean.GridScale();
    meteor.FillImpacts(cb.impacts);
    whirlpool.FillCB(cb.whirl, cb.whirl2, cb.whirl3);

    void* p = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS va = ctx.AllocUpload(sizeof(FrameCB), &p);
    memcpy(p, &cb, sizeof(FrameCB));
    return va;
}

// Fold every live vortex into one earful: energy-weighted timbre, summed
// loudness, stereo from where the drain sits relative to the view.
static void GatherWhirlSound(const Whirlpool& whirl, const Camera& camera,
                             float& loud, float& speed, float& radius, float& forcing,
                             float& pan, float& near01)
{
    float sumAmp = 0, sumW = 0, sumSp = 0, sumR = 0, sumF = 0, sumPan = 0, sumNear = 0;
    float cy = std::cos(camera.yaw), sy = std::sin(camera.yaw);
    float height = std::clamp(1.15f - camera.pos.y / 280.0f, 0.15f, 1.0f);
    for (uint32_t i = 0; i < Whirlpool::kMaxActive; ++i)
    {
        Whirlpool::Acoustic a;
        if (!whirl.SlotAcoustic(i, a))
            continue;
        float dx = a.x - camera.pos.x;
        float dz = a.z - camera.pos.z;
        float horiz = std::sqrt(dx * dx + dz * dz);
        float dist = std::sqrt(horiz * horiz + camera.pos.y * camera.pos.y);
        float att = 1.0f / (1.0f + dist / (50.0f + 0.25f * std::max(a.reach, 40.0f)));
        // A gentle power so the first seconds of spin-up are already audible,
        // while a fast vortex still sits well above a slow one.
        float e = std::pow(std::clamp(a.speed / 6.5f, 0.0f, 3.0f), 0.85f);
        float amp = e * att * height;
        float bearing = (horiz > 0.5f) ? (dx * cy - dz * sy) / horiz : 0.0f;
        sumAmp += amp;
        sumW += amp;
        sumSp += amp * a.speed;
        sumR += amp * a.radius;
        sumF += amp * a.forcing;
        sumPan += amp * std::clamp(bearing, -1.0f, 1.0f);
        sumNear += amp * std::clamp(att, 0.0f, 1.0f);
    }
    if (sumW < 1e-5f)
    {
        loud = 0.0f;
        speed = 0.0f;
        radius = 6.0f;
        forcing = 0.0f;
        pan = 0.0f;
        near01 = 1.0f;
        return;
    }
    loud = std::tanh(sumAmp * 1.15f);
    speed = sumSp / sumW;
    radius = sumR / sumW;
    forcing = sumF / sumW;
    pan = sumPan / sumW;
    near01 = sumNear / sumW;
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
    whirlpool.Update(dt * uiTimeScale);
    if (audio.Available())
    {
        float loud, speed, radius, forcing, pan, near01;
        GatherWhirlSound(whirlpool, camera, loud, speed, radius, forcing, pan, near01);
        audio.SetWhirl(loud, speed, radius, forcing, pan, near01, uiWhirlVolume);
    }
    // The moment the rock hits the water: splash + surge of spreading waves.
    bool flyingNow = meteor.Flying();
    if (prevMeteorFlying && !flyingNow)
    {
        audio.PlayMeteor(MeteorSfx::Impact);
        audio.PlayMeteor(MeteorSfx::Waves);
    }
    prevMeteorFlying = flyingNow;
    meteor.UploadParticles(ctx);
    buoy.Update(ctx, ocean, &meteor, &whirlpool, dt * uiTimeScale, simTime, oceanParams.choppiness);

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
        ApplyEnlargedCursor(hwnd, uiMenuEnlarged);
    }

    ctx.Transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    ctx.EndFrame(uiVsync && opts.benchFrames == 0);
}

// Scale every metric ScaleAllSizes touches, pushed so the global style stays
// at the default size. Call before Begin so the title bar and padding match.
static int PushMenuMetrics(float scale)
{
    ImGuiStyle scaled = ImGui::GetStyle();
    scaled.ScaleAllSizes(scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, scaled.WindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, scaled.WindowRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, scaled.WindowBorderSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, scaled.WindowMinSize);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, scaled.FramePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, scaled.FrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, scaled.FrameBorderSize);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, scaled.ItemSpacing);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, scaled.ItemInnerSpacing);
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, scaled.IndentSpacing);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, scaled.ScrollbarSize);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, scaled.ScrollbarRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, scaled.GrabMinSize);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, scaled.GrabRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, scaled.PopupRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, scaled.PopupBorderSize);
    ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextPadding, scaled.SeparatorTextPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, scaled.CellPadding);
    return 18;
}

void App::BuildUi(float dt)
{
    int styleVars = 0;
    bool menuFontPushed = false;
    if (uiMenuEnlarged)
    {
        styleVars = PushMenuMetrics(kMenuEnlarge);
        if (menuFontLarge)
            ImGui::SetCurrentFont(menuFontLarge); // title bar is measured inside Begin
    }

    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Rough Open Ocean", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (uiMenuEnlarged && menuFontLarge)
    {
        ImGui::PushFont(menuFontLarge);
        menuFontPushed = true;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.1f FPS  (%.2f ms)", fpsDisplay, fpsDisplay > 0 ? 1000.0f / fpsDisplay : 0.0f);
    // One tab after "ms)", then the buttons on the same line.
    ImGui::SameLine(0.0f, ImGui::CalcTextSize("    ").x);
    bool collapseAll = false;
    if (ImGui::Button("Collapse_ALL"))
        collapseAll = true;
    ImGui::SameLine();
    // Stay in the held-down color while the enlarged scale is on.
    const bool enlargePushed = uiMenuEnlarged;
    if (enlargePushed)
    {
        ImVec4 down = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, down);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, down);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, down);
    }
    if (ImGui::Button("ENLARGE"))
        uiMenuEnlarged = !uiMenuEnlarged;
    if (enlargePushed)
        ImGui::PopStyleColor(3);

    // Next row, under the FPS readout. Stay held down while the window covers the monitor.
    const bool fullscreenPushed = fullscreen;
    if (fullscreenPushed)
    {
        ImVec4 down = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, down);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, down);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, down);
    }
    if (ImGui::Button("FullScreen"))
        requestFullscreenToggle = true;
    if (fullscreenPushed)
        ImGui::PopStyleColor(3);
    ImGui::Separator();

    // Applied to every section header below, this frame only.
    auto sectionOpen = [&](const char* label, ImGuiTreeNodeFlags flags = 0)
    {
        if (collapseAll)
            ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        return ImGui::CollapsingHeader(label, flags);
    };

    // Everyday view controls. Open on launch; Collapse_ALL can close it.
    if (sectionOpen("BASICs", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* lodNames[kNumLods];
        for (int i = 0; i < kNumLods; ++i)
            lodNames[i] = kLods[i].name;
        if (ImGui::Combo("Level of detail", &uiLod, lodNames, kNumLods))
        {
            // applied at the top of the next frame
        }

        const char* timeNames[kNumTimes];
        for (int i = 0; i < kNumTimes; ++i)
            timeNames[i] = kTimes[i].name;
        if (ImGui::Combo("Time of day", &uiTimeOfDay, timeNames, kNumTimes))
        {
            UpdateLighting();
            sky.MarkDirty();
        }

        if (ImGui::SliderInt("Sea state", &uiSeaState, 0, 9, kSeas[uiSeaState].name))
        {
            ApplySeaState();
            spectrumDirty = true;
        }

        // What a left-click on the water does. The radio labels carry "##click"
        // so they do not share an ID with the Meteorite / Whirlpool headers.
        ImGui::TextUnformatted("Left-click on water:");
        ImGui::SameLine();
        ImGui::RadioButton("Meteorite##click", &uiClickMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Whirlpool##click", &uiClickMode, 1);
    }

    if (sectionOpen("Meteorite"))
    {
        ImGui::SliderFloat("Impact power", &uiMeteorPower, 1.0f, 8.0f, "%.1f m");
    }

    // Whirlpool shape. Everything here is captured when a vortex spawns, so
    // editing a slider never snaps one that is already spinning.
    if (sectionOpen("Whirlpool"))
    {
        auto hint = [](const char* text)
        {
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", text);
        };

        // Master strength: one step sets the whole pack below. The label picks
        // up "(edited)" once any individual slider departs from the preset.
        // Pre-formatted, because ImGui feeds the format string an int.
        char strengthLabel[64];
        bool onPreset = Whirlpool::MatchesPreset(uiWhirl, uiWhirlPreset);
        snprintf(strengthLabel, sizeof(strengthLabel), "%s%s",
                 Whirlpool::PresetName(uiWhirlPreset), onPreset ? "" : " (edited)");
        if (ImGui::SliderInt("Strength", &uiWhirlPreset, 0, Whirlpool::PresetCount() - 1,
                             strengthLabel))
        {
            bool cw = uiWhirl.clockwise; // spin direction is the user's own choice
            uiWhirl = Whirlpool::Preset(uiWhirlPreset);
            uiWhirl.clockwise = cw;
        }
        hint("Overall size and force, from a bathtub drain to a maelstrom.\n"
             "Sets every slider below; tweak any of them afterwards to taste.");
        ImGui::Spacing();

        // What the sliders add up to, in physical terms.
        ImGui::Text("Peak swirl %.1f m/s at r = %.1f m",
                    Whirlpool::PeakSwirlSpeed(uiWhirl), Whirlpool::CoreRadius(uiWhirl));
        ImGui::Text("Circulation %.0f m^2/s  |  %.1f turns  |  %.0f s event",
                    Whirlpool::Circulation(uiWhirl), Whirlpool::WindTurns(uiWhirl),
                    Whirlpool::Lifetime(uiWhirl));
        // Live vortices, one bar each: they coexist and interact.
        if (whirlpool.AnyActive())
        {
            for (uint32_t i = 0; i < Whirlpool::kMaxActive; ++i)
            {
                if (!whirlpool.SlotActive(i))
                    continue;
                float age = whirlpool.SlotAge(i), life = whirlpool.SlotLifetime(i);
                char buf[64];
                snprintf(buf, sizeof(buf), "%.1f / %.0f s", age, life);
                ImGui::ProgressBar(age / std::max(life, 1e-3f), ImVec2(-1, 0), buf);
            }
            ImGui::TextDisabled("%u of %u live - click again to add more.",
                                whirlpool.ActiveCount(), Whirlpool::kMaxActive);
        }
        else
        {
            ImGui::TextDisabled("Idle - click the water to open a drain.");
        }

        ImGui::SliderFloat("Funnel depth", &uiWhirl.depth, 0.5f, 30.0f, "%.1f m");
        hint("How deep the funnel is pulled at full spin-up. This sets the\n"
             "circulation too: the throat and the swirl speed grow with it.");
        ImGui::SliderFloat("Vortex width", &uiWhirl.sizeMul, 0.4f, 3.0f, "%.2fx");
        hint("Width of the throat, relative to the depth-matched default.\n"
             "Narrow = a tight deep drain, wide = a broad slow maelstrom.");
        ImGui::SliderFloat("Spin-up time", &uiWhirl.grow, 1.0f, 20.0f, "%.1f s");
        hint("How long the force keeps pulling. The whirl deepens and widens\n"
             "for this long, then the plug is released.");
        ImGui::SliderFloat("Decay time", &uiWhirl.tau, 0.2f, 8.0f, "%.1f s");
        hint("How fast it subsides once the force stops. The funnel collapses\n"
             "over roughly this long, and the spiral unwinds over ~2x it.");
        ImGui::SliderFloat("Swirl winding", &uiWhirl.gain, 0.0f, 8.0f, "%.2f");
        hint("How far the surrounding waves are wound around the vortex.\n"
             "0 = a pure sinking funnel with no spiral.");
        ImGui::SliderFloat("Draw-in", &uiWhirl.sink, 0.0f, 0.40f, "%.3f");
        hint("How strongly the surrounding water is sucked toward the drain.");
        ImGui::SliderFloat("Reach", &uiWhirl.reach, 40.0f, 600.0f, "%.0f m");
        hint("Distance scale over which the vortex disturbs the sea.");

        int dir = uiWhirl.clockwise ? 1 : 0;
        const char* dirs[] = { "Counter-clockwise", "Clockwise" };
        if (ImGui::Combo("Rotation", &dir, dirs, 2))
            uiWhirl.clockwise = (dir == 1);
        hint("Spin direction seen from above.");

        if (ImGui::Button("Reset whirlpool", ImVec2(-1, 0)))
        {
            uiWhirlPreset = kWhirlDefaultPreset;
            uiWhirl = Whirlpool::Preset(uiWhirlPreset);
        }
    }

    if (sectionOpen("Waves"))
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
        if (ImGui::SliderFloat("Swell height", &uiSwellAmp, 0.0f, 2.5f, "%.2f m"))
        {
            ApplySeaState();
            spectrumDirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Long rolling waves from a distant storm, crossing\n"
                              "the local wind sea at an angle. 0 = off.");
        if (ImGui::SliderFloat("Swell length", &uiSwellLambda, 60.0f, 300.0f, "%.0f m"))
        {
            ApplySeaState();
            spectrumDirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Crest-to-crest wavelength of the swell.");
        if (ImGui::SliderFloat("Ripple cutoff", &uiSmallCutCm, 0.5f, 8.0f, "%.1f cm"))
        {
            ApplySeaState();
            spectrumDirty = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Waves shorter than this are removed from the spectrum.\n"
                              "Higher = calmer, cleaner surface with less glinting.");
    }

    if (sectionOpen("Rendering"))
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

    if (sectionOpen("Buoy"))
    {
        ImGui::SliderFloat("Flash period", &buoy.flashPeriod, 0.6f, 6.0f, "%.1f s");
        ImGui::SliderFloat("Flash duration", &buoy.flashDuration, 0.1f, 1.5f, "%.2f s");
        ImGui::SliderFloat("Lamp intensity", &buoy.lampIntensity, 2.0f, 120.0f, "%.0f");
    }

    if (sectionOpen("Sound"))
    {
        if (audio.Available())
        {
            const char* modes[] = { "Off", "Soothing (recorded)", "Realistic (synthesized)" };
            ImGui::Combo("Mode", &uiSoundMode, modes, 3);
            if (uiSoundMode == int(SoundMode::Soothing) && !audio.RecordingLoaded())
                ImGui::TextDisabled("Recording missing - using synthesized sound.");
            ImGui::SliderFloat("Volume", &uiVolume, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Whirlpool##sfx", &uiWhirlVolume, 0.0f, 1.5f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Loudness of a live drain. Independent of the sea.\n"
                                  "1 is the default; turn it up if the vortex is hard to hear.");
            ImGui::TextDisabled("Sea loudness follows the sea state.");
        }
        else
        {
            ImGui::TextDisabled("No audio output device.");
        }
    }

    ImGui::Separator();
    // Arial Narrow keeps these two lines inside the panel. Proggy at full size
    // would stretch it, so fall back to a smaller scale if that face is missing.
    // The large face is the same text at the ENLARGE scale.
    ImFont* hint = (uiMenuEnlarged && hintFontLarge) ? hintFontLarge : hintFont;
    const ImVec4 hintCol(0.91f, 0.18f, 0.34f, 1.0f);
    if (hint)
        ImGui::PushFont(hint);
    else
        ImGui::SetWindowFontScale(0.72f);
    ImGui::PushStyleColor(ImGuiCol_Text, hintCol);
    ImGui::TextUnformatted("RMB drag: look around  |  WASD/QE: move  |  F11: fullscreen");
    ImGui::TextUnformatted("LMB: water event  |  Shift: fast  |  Wheel: speed  |  Esc: quit");
    ImGui::PopStyleColor();
    if (hint)
        ImGui::PopFont();
    else
        ImGui::SetWindowFontScale(1.0f);
    if (menuFontPushed)
        ImGui::PopFont();
    ImGui::End();

    if (styleVars > 0)
        ImGui::PopStyleVar(styleVars);
}

LRESULT App::HandleMsg(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Take the cursor before Dear ImGui paints the normal-size arrow.
    if (msg == WM_SETCURSOR && uiMenuEnlarged && enlargedCursor && LOWORD(lp) == HTCLIENT)
    {
        SetCursor(enlargedCursor);
        return TRUE;
    }
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
                UpdateWindowTitle(w, h);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (!io || !io->WantCaptureMouse)
        {
            if (uiClickMode == 1)
                SpawnWhirlpoolAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            else
                LaunchMeteorAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
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
    case WM_SYSKEYDOWN:
        // Alt+Enter. Bit 29 is the context code (Alt held); bit 30 is the previous state.
        if (wp == VK_RETURN && (lp & (1 << 29)) && (lp & (1 << 30)) == 0)
        {
            ToggleFullscreen();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            PostQuitMessage(0);
        if (wp == VK_F11 && (lp & (1 << 30)) == 0)
            ToggleFullscreen();
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
