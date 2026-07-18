#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

#include "Util.h"

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Fixed descriptor slot layout in the single shader-visible CBV/SRV/UAV heap.
// Slots that a root-signature descriptor table reads together must be
// contiguous; each block below is one table (or a stride-spaced family).
// ---------------------------------------------------------------------------
namespace DescSlot
{
    // Ocean draw SRV table (t0..t9): per cascade {displacement, derivatives,
    // foam} x3, then sky cubemap.
    constexpr uint32_t OceanDraw = 0;      // 10 slots
    constexpr uint32_t SkyDraw = 16;       // 1 slot: sky cubemap
    constexpr uint32_t BuoyDraw = 20;      // 1 slot: sky cubemap
    constexpr uint32_t Tonemap = 24;       // 2 slots: scene HDR, bloom
    constexpr uint32_t Fxaa = 28;          // 1 slot: LDR
    constexpr uint32_t BloomPre = 32;      // 1 slot: scene HDR
    constexpr uint32_t BloomDown = 36;     // kBloomMips slots, 1 per pass
    constexpr uint32_t BloomUp = 48;       // kBloomMips*2 slots, 2 per pass
    // Compute UAV tables.
    constexpr uint32_t AssembleUav = 64;   // per cascade c: 3 slots at 64+4c
    constexpr uint32_t SkyGenUav = 80;     // 1 slot: cube mip0 as array
    constexpr uint32_t SkyMipSrv = 84;     // per mip m: SRV of mip m-1
    constexpr uint32_t SkyMipUav = 96;     // per mip m: UAV of mip m
    constexpr uint32_t ImGuiFont = 110;
    constexpr uint32_t Count = 128;
}

namespace RtvSlot
{
    constexpr uint32_t Backbuffer0 = 0; // 3 slots
    constexpr uint32_t SceneHdr = 3;
    constexpr uint32_t Ldr = 4;
    constexpr uint32_t BloomA = 5;  // kBloomMips slots
    constexpr uint32_t BloomB = 13; // kBloomMips slots
    constexpr uint32_t Count = 24;
}

class GpuContext
{
public:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kBackbuffers = 3;
    static constexpr DXGI_FORMAT kBackbufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

    void Init(HWND hwnd, uint32_t width, uint32_t height, bool needSwapchain = true);
    void Shutdown();
    void Resize(uint32_t width, uint32_t height);

    // Frame lifecycle. BeginFrame waits for the frame slot's previous work,
    // resets the allocator/list. EndFrame closes, executes, presents, signals.
    void BeginFrame();
    void EndFrame(bool vsync);
    void WaitIdle();

    // One-shot command list execution (init-time uploads etc.). Uses the main
    // list, executes and blocks until done.
    void BeginOneShot();
    void EndOneShot();

    // Per-frame upload ring allocation (CBs, dynamic data), 256-byte aligned.
    D3D12_GPU_VIRTUAL_ADDRESS AllocUpload(size_t size, void** cpuPtr);

    // Resource helpers.
    ComPtr<ID3D12Resource> CreateTexture2D(uint32_t w, uint32_t h, DXGI_FORMAT fmt,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState,
        uint16_t mips = 1, uint16_t arraySize = 1, const wchar_t* name = nullptr,
        const float* clearColor = nullptr);
    ComPtr<ID3D12Resource> CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState,
        const wchar_t* name = nullptr);
    void UploadBufferData(ID3D12Resource* dst, const void* data, uint64_t size);
    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
    void UavBarrier(ID3D12Resource* res);

    // Shader compilation (FXC, SM 5.x), throws with the error log on failure.
    ComPtr<ID3DBlob> CompileShader(const std::wstring& file, const char* entry,
        const char* target, const D3D_SHADER_MACRO* defines = nullptr);
    std::wstring ShaderPath(const wchar_t* name) const;

    // Descriptor handle helpers.
    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(uint32_t slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(uint32_t slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpu(uint32_t slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpu() const;

    // Common pipeline state helpers.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC DefaultPsoDesc() const;
    ComPtr<ID3D12PipelineState> CreateComputePso(ID3DBlob* cs, const wchar_t* name);

    ID3D12Device* Dev() const { return device.Get(); }
    ID3D12GraphicsCommandList* Cmd() const { return cmd.Get(); }

    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandAllocator> cmdAlloc[kFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    uint64_t nextFenceValue = 1;
    uint64_t frameFenceValue[kFramesInFlight] = {};
    uint32_t frameIndex = 0; // frame-in-flight slot (0/1)
    uint32_t width = 0, height = 0;
    bool tearingSupported = false;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

    ComPtr<ID3D12Resource> backbuffers[kBackbuffers];
    ComPtr<ID3D12Resource> depthBuffer;

    ComPtr<ID3D12DescriptorHeap> srvHeap; // shader-visible
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    uint32_t srvStride = 0, rtvStride = 0;

    // Shared root signatures.
    // Graphics: b0 root CBV (frame), b1 root CBV (object), SRV table t0..t15,
    // static samplers s0 wrap / s1 clamp / s2 point.
    ComPtr<ID3D12RootSignature> graphicsRS;
    // Compute: b0 root CBV, t0/t1 root SRV buffers, u0/u1 root UAV buffers,
    // SRV table t2..t9, UAV table u2..u7, same static samplers.
    ComPtr<ID3D12RootSignature> computeRS;

private:
    void CreateSwapchainResources();
    void CreateRootSignatures();

    ComPtr<ID3D12Resource> uploadRing[kFramesInFlight];
    uint8_t* uploadPtr[kFramesInFlight] = {};
    size_t uploadOffset = 0;
    static constexpr size_t kUploadRingSize = 8 * 1024 * 1024;
    std::wstring shaderDir;
};
