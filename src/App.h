#pragma once

#include <DirectXMath.h>
#include <string>

#include "Audio.h"
#include "Buoy.h"
#include "Camera.h"
#include "GpuContext.h"
#include "Meteor.h"
#include "Ocean.h"
#include "Post.h"
#include "Sky.h"
#include "Whirlpool.h"

struct LaunchOptions
{
    uint32_t width = 1600, height = 900;
    int lod = 2;      // 0 Low, 1 Medium, 2 High, 3 Ultra
    int seaState = 4; // 0..9
    int timeOfDay = 4; // index into time presets (4 = Evening)
    bool vsync = true;
    int benchFrames = 0;          // if > 0: render N frames, report FPS, exit
    std::string screenshotPath;   // optional PNG capture in bench mode
    bool selftest = false;
    bool showUi = false;   // keep the UI visible in bench mode
    float fixedDt = 0;     // deterministic timestep for verification runs
    float meteorAt = -1;   // auto-launch a meteorite at this sim time
    int clickMeteorX = -1, clickMeteorY = -1; // if set, place that meteor at a screen pixel (test hook)
    float whirlAt = -1;    // auto-spawn a whirlpool at this sim time
    int clickWhirlX = -1, clickWhirlY = -1; // if set, place that whirlpool at a screen pixel (test hook)
    WhirlpoolParams whirl; // shape of CLI-spawned whirlpools (--whirldepth etc.)
    int whirlPreset = kWhirlDefaultPreset; // --whirlstrength 0..6
    int whirlCount = 1;      // how many to spawn (overlap test hook)
    float whirlGap = 1.2f;   // seconds between them
    // Optional camera override (verification shots).
    bool hasCamera = false;
    float camX = 0, camY = 12, camZ = 0;
    float yawDeg = 0, pitchDeg = -1.0f;
};

class App
{
public:
    int Run(HINSTANCE hInst, const LaunchOptions& options);
    LRESULT HandleMsg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    // Mirrors FrameCB in Common.hlsli.
    struct FrameCB
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4X4 invViewProj;
        DirectX::XMFLOAT3 camPos; float time;
        DirectX::XMFLOAT3 camRight; float exposure;
        DirectX::XMFLOAT3 camUp; float lightIsMoon;
        DirectX::XMFLOAT3 lightDir; float starIntensity;
        DirectX::XMFLOAT3 lightColor; float roughBase;
        DirectX::XMFLOAT3 sunDiscColor; float lambda;
        DirectX::XMFLOAT4 cascade0;
        DirectX::XMFLOAT4 cascade1;
        DirectX::XMFLOAT4 cascade2;
        DirectX::XMFLOAT3 waterDeep; float foamAmount;
        DirectX::XMFLOAT3 waterScatter; float sss;
        DirectX::XMFLOAT3 buoyLightPos; float buoyLightOn;
        DirectX::XMFLOAT3 buoyLightColor; float fogDensity;
        DirectX::XMFLOAT2 windDir; float distRough; float gridScale;
        DirectX::XMFLOAT4 impacts[Meteor::kMaxImpacts];
        DirectX::XMFLOAT4 whirl[Whirlpool::kMaxActive];
        DirectX::XMFLOAT4 whirl2[Whirlpool::kMaxActive];
        DirectX::XMFLOAT4 whirl3[Whirlpool::kMaxActive];
    };

    void InitWindow(HINSTANCE hInst);
    void ToggleFullscreen();
    void InitSystems();
    void ApplyLod(int lod, bool firstTime);
    void ApplySeaState();
    void UpdateLighting();
    void UpdateCameraInput(float dt);
    void LaunchMeteor(); // fires the rock + its descent sound
    void LaunchMeteorAt(int mouseX, int mouseY); // click-to-place impact from the sky
    void SpawnWhirlpool();                       // vortex a fixed way ahead of the camera
    void SpawnWhirlpoolAt(int mouseX, int mouseY); // click-to-place vortex
    // Unproject a screen pixel onto the mean water plane y = 0.
    bool PickWater(int mouseX, int mouseY, DirectX::XMFLOAT3& hit) const;
    void RenderFrame(float dt);
    void BuildUi(float dt);
    D3D12_GPU_VIRTUAL_ADDRESS FillFrameCB();
    void SaveBackbufferPng(const std::wstring& path);
    int RunFFTSelfTest();

    LaunchOptions opts;
    HWND hwnd = nullptr;
    GpuContext ctx;
    Camera camera;
    Ocean ocean;
    Sky sky;
    Buoy buoy;
    Post post;
    OceanAudio audio;
    Meteor meteor;
    Whirlpool whirlpool;

    OceanParams oceanParams;
    Sky::Params skyParams;

    // UI-adjustable state.
    int uiLod = 2;
    int uiSeaState = 4;
    int uiTimeOfDay = 4;
    float uiWindDirDeg = 190.0f;
    float uiChopMul = 1.0f;
    float uiFoamMul = 0.5f; // Foam slider: shown 0.50x..1.00x, maps to 0.1x..0.5x actual
    float uiAmpMul = 1.0f;
    float uiSwellAmp = 0.7f;      // long-crested background swell height, m
    float uiSwellLambda = 130.0f; // swell wavelength, m
    float uiSmallCutCm = 3.3f;    // small-wave suppression length, cm
    float uiExposureMul = 1.0f;
    float uiCloudCover = 0.25f;
    float uiTimeScale = 1.0f;
    float uiBloom = 0.06f;
    bool uiVsync = true;
    bool uiFxaa = true;
    int uiSoundMode = int(SoundMode::Soothing);
    float uiVolume = 0.8f;
    float uiWhirlVolume = 1.0f; // drain layer, independent of the sea volume
    bool uiMenuEnlarged = false;
    float uiMeteorPower = 4.0f;
    int uiClickMode = 1;        // left-click event: 0 = meteorite, 1 = whirlpool
    WhirlpoolParams uiWhirl;    // shape of the next whirlpool
    int uiWhirlPreset = kWhirlDefaultPreset; // strength preset driving uiWhirl
    bool spectrumDirty = true;
    bool meteorAutoLaunched = false;
    int whirlAutoSpawned = 0;
    bool prevMeteorFlying = false; // edge-detects the water impact for its sound

    // Derived lighting.
    DirectX::XMFLOAT3 lightDir{ 0, 0.5f, 0.87f };
    DirectX::XMFLOAT3 lightColor{ 1, 1, 1 };
    DirectX::XMFLOAT3 sunDiscColor{ 100, 100, 95 };
    float lightIsMoon = 0.0f;
    float starIntensity = 0.0f;
    float exposureBase = 1.0f;
    float ampEstimate = 0.8f;

    float simTime = 0.0f;
    double perfFreq = 0.0;
    int64_t lastTicks = 0;
    bool running = true;
    bool fullscreen = false;
    WINDOWPLACEMENT windowedPlacement{};
    LONG_PTR windowedStyle = 0;
    bool mouseLook = false;
    POINT lastMouse = {};

    // Bench / FPS statistics.
    int frameCounter = 0;
    double fpsAccum = 0.0;
    int fpsFrames = 0;
    float fpsDisplay = 0.0f;
    double benchAccum = 0.0;
    int benchFramesDone = 0;
};
