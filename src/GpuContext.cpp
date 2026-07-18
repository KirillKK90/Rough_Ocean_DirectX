#include "GpuContext.h"

#include <filesystem>

void GpuContext::Init(HWND hwnd, uint32_t w, uint32_t h, bool needSwapchain)
{
    width = w;
    height = h;

    UINT factoryFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    HR(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)));

    // Pick the highest-performance adapter that gives us the best feature
    // level, capped at 12_1 per project requirements.
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i,
             DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        for (D3D_FEATURE_LEVEL fl : levels)
        {
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), fl, IID_PPV_ARGS(&device))))
            {
                featureLevel = fl;
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                LogF("GPU: %s (feature level %X_%X)\n", name,
                     (featureLevel >> 12) & 0xF, (featureLevel >> 8) & 0xF);
                break;
            }
        }
        if (device)
            break;
    }
    if (!device)
        throw std::runtime_error("No Direct3D 12 capable GPU found.");

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));

    for (uint32_t i = 0; i < kFramesInFlight; ++i)
        HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc[i])));
    HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&cmd)));
    HR(cmd->Close());

    HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Descriptor heaps.
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = DescSlot::Count;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = RtvSlot::Count;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    HR(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap)));
    srvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Upload ring buffers.
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        uploadRing[i] = CreateBuffer(kUploadRingSize, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"UploadRing");
        D3D12_RANGE noRead = { 0, 0 };
        HR(uploadRing[i]->Map(0, &noRead, reinterpret_cast<void**>(&uploadPtr[i])));
    }

    CreateRootSignatures();

    if (needSwapchain && hwnd)
    {
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
            tearingSupported = tearing != FALSE;

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = width;
        sd.Height = height;
        sd.Format = kBackbufferFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBackbuffers;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        ComPtr<IDXGISwapChain1> sc1;
        HR(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1));
        HR(sc1.As(&swapchain));
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        CreateSwapchainResources();
    }

    // Locate the shader directory: next to the exe, or up the tree (useful
    // when running from the build directory).
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 4; ++i)
    {
        if (std::filesystem::exists(dir / L"shaders" / L"Common.hlsli"))
        {
            shaderDir = (dir / L"shaders").wstring();
            break;
        }
        dir = dir.parent_path();
    }
    if (shaderDir.empty())
        throw std::runtime_error("Could not locate the 'shaders' directory near the executable.");
}

void GpuContext::CreateSwapchainResources()
{
    for (uint32_t i = 0; i < kBackbuffers; ++i)
    {
        HR(swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i])));
        backbuffers[i]->SetName(L"Backbuffer");
        device->CreateRenderTargetView(backbuffers[i].Get(), nullptr, RtvCpu(RtvSlot::Backbuffer0 + i));
    }

    D3D12_CLEAR_VALUE dc = {};
    dc.Format = kDepthFormat;
    dc.DepthStencil.Depth = 0.0f; // reversed-Z clear
    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = width;
    dd.Height = height;
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = kDepthFormat;
    dd.SampleDesc.Count = 1;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    HR(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc, IID_PPV_ARGS(&depthBuffer)));
    depthBuffer->SetName(L"Depth");
    device->CreateDepthStencilView(depthBuffer.Get(), nullptr, DsvCpu());
}

void GpuContext::Resize(uint32_t w, uint32_t h)
{
    if (!swapchain || w == 0 || h == 0 || (w == width && h == height))
        return;
    WaitIdle();
    for (auto& bb : backbuffers)
        bb.Reset();
    depthBuffer.Reset();
    width = w;
    height = h;
    HR(swapchain->ResizeBuffers(kBackbuffers, width, height, kBackbufferFormat,
        tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    CreateSwapchainResources();
}

void GpuContext::Shutdown()
{
    if (device)
        WaitIdle();
    if (fenceEvent)
    {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
}

void GpuContext::BeginFrame()
{
    // Wait until the GPU finished the frame that previously used this slot.
    uint64_t waitFor = frameFenceValue[frameIndex];
    if (waitFor && fence->GetCompletedValue() < waitFor)
    {
        HR(fence->SetEventOnCompletion(waitFor, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    HR(cmdAlloc[frameIndex]->Reset());
    HR(cmd->Reset(cmdAlloc[frameIndex].Get(), nullptr));
    uploadOffset = 0;

    ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
}

void GpuContext::EndFrame(bool vsync)
{
    HR(cmd->Close());
    ID3D12CommandList* lists[] = { cmd.Get() };
    queue->ExecuteCommandLists(1, lists);

    UINT flags = (!vsync && tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HR(swapchain->Present(vsync ? 1 : 0, flags));

    frameFenceValue[frameIndex] = nextFenceValue;
    HR(queue->Signal(fence.Get(), nextFenceValue));
    ++nextFenceValue;
    frameIndex = (frameIndex + 1) % kFramesInFlight;
}

void GpuContext::WaitIdle()
{
    HR(queue->Signal(fence.Get(), nextFenceValue));
    if (fence->GetCompletedValue() < nextFenceValue)
    {
        HR(fence->SetEventOnCompletion(nextFenceValue, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    ++nextFenceValue;
}

void GpuContext::BeginOneShot()
{
    HR(cmdAlloc[frameIndex]->Reset());
    HR(cmd->Reset(cmdAlloc[frameIndex].Get(), nullptr));
    ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
}

void GpuContext::EndOneShot()
{
    HR(cmd->Close());
    ID3D12CommandList* lists[] = { cmd.Get() };
    queue->ExecuteCommandLists(1, lists);
    WaitIdle();
}

D3D12_GPU_VIRTUAL_ADDRESS GpuContext::AllocUpload(size_t size, void** cpuPtr)
{
    uploadOffset = AlignUp<size_t>(uploadOffset, 256);
    if (uploadOffset + size > kUploadRingSize)
        throw std::runtime_error("Upload ring exhausted");
    *cpuPtr = uploadPtr[frameIndex] + uploadOffset;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = uploadRing[frameIndex]->GetGPUVirtualAddress() + uploadOffset;
    uploadOffset += size;
    return gpu;
}

ComPtr<ID3D12Resource> GpuContext::CreateTexture2D(uint32_t w, uint32_t h, DXGI_FORMAT fmt,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState,
    uint16_t mips, uint16_t arraySize, const wchar_t* name, const float* clearColor)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = arraySize;
    d.MipLevels = mips;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_CLEAR_VALUE cv = {};
    const D3D12_CLEAR_VALUE* pcv = nullptr;
    if (clearColor && (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
    {
        cv.Format = fmt;
        memcpy(cv.Color, clearColor, sizeof(float) * 4);
        pcv = &cv;
    }
    ComPtr<ID3D12Resource> res;
    HR(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initState, pcv, IID_PPV_ARGS(&res)));
    if (name)
        res->SetName(name);
    return res;
}

ComPtr<ID3D12Resource> GpuContext::CreateBuffer(uint64_t size, D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState, const wchar_t* name)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp = { heap };
    ComPtr<ID3D12Resource> res;
    HR(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initState, nullptr, IID_PPV_ARGS(&res)));
    if (name)
        res->SetName(name);
    return res;
}

void GpuContext::UploadBufferData(ID3D12Resource* dst, const void* data, uint64_t size)
{
    // Init-time helper: stage through a transient upload buffer, copy, wait.
    ComPtr<ID3D12Resource> staging = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, L"Staging");
    void* p = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    HR(staging->Map(0, &noRead, &p));
    memcpy(p, data, size);
    staging->Unmap(0, nullptr);

    BeginOneShot();
    cmd->CopyBufferRegion(dst, 0, staging.Get(), 0, size);
    Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    EndOneShot();
}

void GpuContext::Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}

void GpuContext::UavBarrier(ID3D12Resource* res)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    cmd->ResourceBarrier(1, &b);
}

ComPtr<ID3DBlob> GpuContext::CompileShader(const std::wstring& file, const char* entry,
    const char* target, const D3D_SHADER_MACRO* defines)
{
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> blob, errors;
    HRESULT hr = D3DCompileFromFile(file.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, target, flags, 0, &blob, &errors);
    if (FAILED(hr))
    {
        std::string msg = "Shader compile failed: ";
        char nf[260];
        WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, nf, sizeof(nf), nullptr, nullptr);
        msg += nf;
        msg += " / ";
        msg += entry;
        if (errors)
            msg += std::string("\n") + static_cast<const char*>(errors->GetBufferPointer());
        LogF("%s\n", msg.c_str());
        throw std::runtime_error(msg);
    }
    return blob;
}

std::wstring GpuContext::ShaderPath(const wchar_t* name) const
{
    return shaderDir + L"\\" + name;
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuContext::SrvCpu(uint32_t slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * srvStride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuContext::SrvGpu(uint32_t slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(slot) * srvStride;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuContext::RtvCpu(uint32_t slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * rtvStride;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuContext::DsvCpu() const
{
    return dsvHeap->GetCPUDescriptorHandleForHeapStart();
}

static D3D12_STATIC_SAMPLER_DESC MakeSampler(UINT reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE addr)
{
    D3D12_STATIC_SAMPLER_DESC s = {};
    s.Filter = filter;
    s.AddressU = s.AddressV = s.AddressW = addr;
    s.MaxLOD = D3D12_FLOAT32_MAX;
    s.ShaderRegister = reg;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    return s;
}

void GpuContext::CreateRootSignatures()
{
    D3D12_STATIC_SAMPLER_DESC samplers[3] = {
        MakeSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        MakeSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        MakeSampler(2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
    };

    // --- Graphics ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 16;
        srvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 1;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &srvRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rd = {};
        rd.NumParameters = 3;
        rd.pParameters = params;
        rd.NumStaticSamplers = 3;
        rd.pStaticSamplers = samplers;
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> blob, err;
        HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
        HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&graphicsRS)));
        graphicsRS->SetName(L"GraphicsRS");
    }

    // --- Compute ---
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 8;
        srvRange.BaseShaderRegister = 2;

        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 6;
        uavRange.BaseShaderRegister = 2;

        D3D12_ROOT_PARAMETER params[7] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 1;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].Descriptor.ShaderRegister = 0;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[4].Descriptor.ShaderRegister = 1;
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[5].DescriptorTable.NumDescriptorRanges = 1;
        params[5].DescriptorTable.pDescriptorRanges = &srvRange;
        params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[6].DescriptorTable.NumDescriptorRanges = 1;
        params[6].DescriptorTable.pDescriptorRanges = &uavRange;
        for (auto& p : params)
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rd = {};
        rd.NumParameters = 7;
        rd.pParameters = params;
        rd.NumStaticSamplers = 3;
        rd.pStaticSamplers = samplers;

        ComPtr<ID3DBlob> blob, err;
        HR(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
        HR(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&computeRS)));
        computeRS->SetName(L"ComputeRS");
    }
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC GpuContext::DefaultPsoDesc() const
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = graphicsRS.Get();
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    d.RasterizerState.FrontCounterClockwise = FALSE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.DepthStencilState.DepthEnable = TRUE;
    d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; // reversed-Z
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = kHdrFormat;
    d.DSVFormat = kDepthFormat;
    d.SampleDesc.Count = 1;
    return d;
}

ComPtr<ID3D12PipelineState> GpuContext::CreateComputePso(ID3DBlob* cs, const wchar_t* name)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = computeRS.Get();
    d.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ComPtr<ID3D12PipelineState> pso;
    HR(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)));
    if (name)
        pso->SetName(name);
    return pso;
}
