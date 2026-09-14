#pragma once

#include <DirectXMath.h>

#include "GpuContext.h"

struct OceanQuality
{
    uint32_t fftN = 256;
    uint32_t cascades = 3;
    uint32_t sectors = 384; // radial mesh angular resolution
    uint32_t rings = 224;   // radial mesh ring count
};

// Physical + artistic simulation parameters (App maps sea state presets here).
struct OceanParams
{
    float U10 = 7.5f;        // wind speed at 10m, m/s
    float fetch = 90000.0f;  // fetch, m
    DirectX::XMFLOAT2 windDir{ -0.17f, -0.98f };
    float spreadExp = 6.0f;
    float ampScale = 1.0f;
    float choppiness = 0.78f;
    float foamBias = 0.72f;
    float foamDecay = 0.30f;
    float foamAdd = 1.0f;
    uint32_t seed = 1234;
    // Small-wave suppression length l (m): spectrum is damped by exp(-k^2 l^2),
    // removing the sub-decimeter chop that only aliases into glint noise.
    float smallCut = 0.033f;
    // Long-crested swell from a distant storm (0 amplitude = off).
    float swellAmp = 0.7f;        // target RMS surface amplitude, m
    float swellLambda = 130.0f;   // peak wavelength, m
    DirectX::XMFLOAT2 swellDir{ -0.64f, -0.77f }; // unit, travel direction
    float swellSpread = 48.0f;    // directional lobe power (long-crested)
};

struct WaterSample
{
    float height = 0.0f;
    DirectX::XMFLOAT3 displacement{ 0, 0, 0 };
    DirectX::XMFLOAT3 normal{ 0, 1, 0 };
};

class Ocean
{
public:
    static constexpr uint32_t kMaxCascades = 3;
    static constexpr uint32_t kReadbackCascades = 2;

    void Create(GpuContext& ctx, const OceanQuality& quality);

    // Records all compute work + the readback copy for buoy physics.
    void RecordSimulation(GpuContext& ctx, float simTime, float dt,
                          const OceanParams& params, bool reinitSpectrum);
    void Draw(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB);

    // CPU-side water query at absolute world XZ (uses the readback completed
    // kFramesInFlight frames ago; call between BeginFrame and RecordSimulation).
    WaterSample Sample(GpuContext& ctx, float x, float z, float lambda) const;

    float CascadeLength(uint32_t c) const { return casc[c].L; }
    uint32_t NumCascades() const { return numCascades; }
    uint32_t FftN() const { return fftN; }
    // Radial mesh vertex spacing per meter of camera distance (for the
    // vertex shader's distance-matched displacement mip).
    float GridScale() const { return gridScale; }

private:
    struct Cascade
    {
        ComPtr<ID3D12Resource> h0;   // float4: h0(k), conj(h0(-k))
        ComPtr<ID3D12Resource> buf0; // packed spectra / FFT workspace
        ComPtr<ID3D12Resource> buf1;
        ComPtr<ID3D12Resource> disp;  // RGBA16F displacement
        ComPtr<ID3D12Resource> deriv; // RGBA16F analytic derivatives
        ComPtr<ID3D12Resource> foam;  // R32F accumulated foam
        float L = 100.0f;
        float minK = 0.0f, maxK = 100.0f;
    };

    void BuildMesh(GpuContext& ctx, const OceanQuality& quality);
    void RecordCascadeSim(GpuContext& ctx, uint32_t c, float simTime, float dt,
                          const OceanParams& params, bool reinit);
    DirectX::XMFLOAT3 SampleDispCpu(uint32_t slot, float x, float z) const;
    float SampleHeightAt(uint32_t slot, float x, float z, float lambda) const;

    Cascade casc[kMaxCascades];
    uint32_t fftN = 256;
    uint32_t numCascades = 3;
    uint32_t mipLevels = 1;       // full chain on disp/deriv/foam maps
    float gridScale = 0.05f;
    float swellAmpTexel = 0.0f;   // swellAmp / sqrt(sum G^2), cached per reinit

    ComPtr<ID3D12PipelineState> psoInit, psoUpdate, psoFFT, psoAssemble, psoDraw;
    ComPtr<ID3D12PipelineState> psoMip4, psoMipF; // mip downsample: float4 maps, float foam

    ComPtr<ID3D12Resource> vb, ib;
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    uint32_t indexCount = 0;

    ComPtr<ID3D12Resource> readback[GpuContext::kFramesInFlight][kReadbackCascades];
    const uint8_t* rbMapped[GpuContext::kFramesInFlight][kReadbackCascades] = {};
    uint32_t rbRowPitch = 0;
    bool mapsEverSimulated[kMaxCascades] = {};
    bool buffersInSrvState[kMaxCascades] = {};
    bool h0Initialized[kMaxCascades] = {};
    uint32_t framesRecorded = 0;
};
