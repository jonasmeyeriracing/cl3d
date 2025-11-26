#include "d3d12_renderer.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static void WaitForFence(D3D12Renderer* renderer, uint64_t fenceValue)
{
    if (renderer->fence->GetCompletedValue() < fenceValue)
    {
        renderer->fence->SetEventOnCompletion(fenceValue, renderer->fenceEvent);
        WaitForSingleObject(renderer->fenceEvent, INFINITE);
    }
}

static void MoveToNextFrame(D3D12Renderer* renderer)
{
    const uint64_t currentFenceValue = renderer->fenceValues[renderer->frameIndex];
    renderer->commandQueue->Signal(renderer->fence.Get(), currentFenceValue);

    renderer->frameIndex = renderer->swapChain->GetCurrentBackBufferIndex();

    WaitForFence(renderer, renderer->fenceValues[renderer->frameIndex]);

    renderer->fenceValues[renderer->frameIndex] = currentFenceValue + 1;
}

static bool CreateDepthBuffer(D3D12Renderer* renderer)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = renderer->width;
    depthDesc.Height = renderer->height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    if (FAILED(renderer->device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&renderer->depthBuffer))))
    {
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = renderer->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    renderer->device->CreateDepthStencilView(renderer->depthBuffer.Get(), &dsvDesc, dsvHandle);

    return true;
}

static const char* g_ShaderSource = R"(
cbuffer CameraConstants : register(b0)
{
    float4x4 viewProjection;
    float3 cameraPos;
    float padding;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldPos : WORLDPOS;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.worldPos = input.position;
    output.position = mul(viewProjection, float4(input.position, 1.0));
    output.normal = input.normal;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 color;

    // Check if this is the ground (normal pointing up and y near 0)
    bool isGround = (input.normal.y > 0.9 && abs(input.worldPos.y) < 0.1);

    if (isGround)
    {
        // Grid pattern for ground
        float2 grid = frac(input.uv * 100.0);
        float lineWidth = 0.02;
        float gridLine = (grid.x < lineWidth || grid.y < lineWidth) ? 1.0 : 0.0;

        float3 baseColor = float3(0.3, 0.35, 0.3);
        float3 lineColor = float3(0.15, 0.2, 0.15);
        color = lerp(baseColor, lineColor, gridLine);
    }
    else
    {
        // Box: simple shading based on normal
        float3 lightDir = normalize(float3(0.5, 1.0, 0.3));
        float ndotl = saturate(dot(input.normal, lightDir));
        float3 boxColor = float3(0.8, 0.3, 0.2); // Reddish-orange box
        color = boxColor * (0.3 + 0.7 * ndotl);
    }

    // Simple distance fog
    float dist = length(input.worldPos - cameraPos);
    float fog = saturate(dist / 2000.0);
    float3 fogColor = float3(0.5, 0.6, 0.7);
    color = lerp(color, fogColor, fog);

    return float4(color, 1.0);
}
)";

static bool CreatePipelineState(D3D12Renderer* renderer)
{
    // Create root signature
    D3D12_ROOT_PARAMETER rootParams[1] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 1;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    if (FAILED(renderer->device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&renderer->rootSignature))))
    {
        return false;
    }

    // Compile shaders from string
    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

    if (FAILED(D3DCompile(g_ShaderSource, strlen(g_ShaderSource), "shaders.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    if (FAILED(D3DCompile(g_ShaderSource, strlen(g_ShaderSource), "shaders.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    // Input layout
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    // Create PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = renderer->rootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(renderer->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderer->pipelineState))))
    {
        OutputDebugStringA("Failed to create PSO\n");
        return false;
    }

    return true;
}

static void AddBox(std::vector<Vertex>& verts, std::vector<uint32_t>& inds,
                   float cx, float cy, float cz, float sx, float sy, float sz)
{
    // Box half-sizes
    float hx = sx * 0.5f;
    float hy = sy * 0.5f;
    float hz = sz * 0.5f;

    uint32_t base = (uint32_t)verts.size();

    // Front face (Z+)
    verts.push_back({{ cx - hx, cy - hy, cz + hz }, { 0, 0, 1 }, { 0, 0 }});
    verts.push_back({{ cx + hx, cy - hy, cz + hz }, { 0, 0, 1 }, { 1, 0 }});
    verts.push_back({{ cx + hx, cy + hy, cz + hz }, { 0, 0, 1 }, { 1, 1 }});
    verts.push_back({{ cx - hx, cy + hy, cz + hz }, { 0, 0, 1 }, { 0, 1 }});

    // Back face (Z-)
    verts.push_back({{ cx + hx, cy - hy, cz - hz }, { 0, 0, -1 }, { 0, 0 }});
    verts.push_back({{ cx - hx, cy - hy, cz - hz }, { 0, 0, -1 }, { 1, 0 }});
    verts.push_back({{ cx - hx, cy + hy, cz - hz }, { 0, 0, -1 }, { 1, 1 }});
    verts.push_back({{ cx + hx, cy + hy, cz - hz }, { 0, 0, -1 }, { 0, 1 }});

    // Right face (X+)
    verts.push_back({{ cx + hx, cy - hy, cz + hz }, { 1, 0, 0 }, { 0, 0 }});
    verts.push_back({{ cx + hx, cy - hy, cz - hz }, { 1, 0, 0 }, { 1, 0 }});
    verts.push_back({{ cx + hx, cy + hy, cz - hz }, { 1, 0, 0 }, { 1, 1 }});
    verts.push_back({{ cx + hx, cy + hy, cz + hz }, { 1, 0, 0 }, { 0, 1 }});

    // Left face (X-)
    verts.push_back({{ cx - hx, cy - hy, cz - hz }, { -1, 0, 0 }, { 0, 0 }});
    verts.push_back({{ cx - hx, cy - hy, cz + hz }, { -1, 0, 0 }, { 1, 0 }});
    verts.push_back({{ cx - hx, cy + hy, cz + hz }, { -1, 0, 0 }, { 1, 1 }});
    verts.push_back({{ cx - hx, cy + hy, cz - hz }, { -1, 0, 0 }, { 0, 1 }});

    // Top face (Y+)
    verts.push_back({{ cx - hx, cy + hy, cz + hz }, { 0, 1, 0 }, { 0, 0 }});
    verts.push_back({{ cx + hx, cy + hy, cz + hz }, { 0, 1, 0 }, { 1, 0 }});
    verts.push_back({{ cx + hx, cy + hy, cz - hz }, { 0, 1, 0 }, { 1, 1 }});
    verts.push_back({{ cx - hx, cy + hy, cz - hz }, { 0, 1, 0 }, { 0, 1 }});

    // Bottom face (Y-)
    verts.push_back({{ cx - hx, cy - hy, cz - hz }, { 0, -1, 0 }, { 0, 0 }});
    verts.push_back({{ cx + hx, cy - hy, cz - hz }, { 0, -1, 0 }, { 1, 0 }});
    verts.push_back({{ cx + hx, cy - hy, cz + hz }, { 0, -1, 0 }, { 1, 1 }});
    verts.push_back({{ cx - hx, cy - hy, cz + hz }, { 0, -1, 0 }, { 0, 1 }});

    // Indices for 6 faces (2 triangles each) - counter-clockwise winding
    uint32_t faceIndices[] = {
        0, 2, 1, 0, 3, 2,       // front
        4, 6, 5, 4, 7, 6,       // back
        8, 10, 9, 8, 11, 10,    // right
        12, 14, 13, 12, 15, 14, // left
        16, 18, 17, 16, 19, 18, // top
        20, 22, 21, 20, 23, 22  // bottom
    };

    for (uint32_t i : faceIndices)
        inds.push_back(base + i);
}

static bool CreateGeometry(D3D12Renderer* renderer)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Ground plane
    const float planeSize = 1000.0f;
    const float halfPlane = planeSize * 0.5f;

    uint32_t planeBase = (uint32_t)vertices.size();
    vertices.push_back({{ -halfPlane, 0, -halfPlane }, { 0, 1, 0 }, { 0, 0 }});
    vertices.push_back({{  halfPlane, 0, -halfPlane }, { 0, 1, 0 }, { 1, 0 }});
    vertices.push_back({{  halfPlane, 0,  halfPlane }, { 0, 1, 0 }, { 1, 1 }});
    vertices.push_back({{ -halfPlane, 0,  halfPlane }, { 0, 1, 0 }, { 0, 1 }});

    indices.push_back(planeBase + 0);
    indices.push_back(planeBase + 1);
    indices.push_back(planeBase + 2);
    indices.push_back(planeBase + 0);
    indices.push_back(planeBase + 2);
    indices.push_back(planeBase + 3);

    // Car-sized boxes: 4m long (Z), 2m wide (X), 1.5m tall (Y)
    const float carLength = 4.0f;
    const float carWidth = 2.0f;
    const float carHeight = 1.5f;

    // Spacing between cars
    const float spacingX = 3.0f;  // Gap between two lanes
    const float spacingZ = 6.0f;  // Gap between cars in a row

    // 60 cars in 2 columns, 30 rows
    const int numCars = 60;
    const int carsPerRow = 2;
    const int numRows = numCars / carsPerRow;

    // Start position (in front of camera)
    const float startZ = 0.0f;

    for (int i = 0; i < numCars; i++)
    {
        int row = i / carsPerRow;
        int col = i % carsPerRow;

        float x = (col == 0) ? -spacingX : spacingX;
        float y = carHeight * 0.5f;  // Sit on ground
        float z = startZ - row * spacingZ;

        AddBox(vertices, indices, x, y, z, carWidth, carHeight, carLength);
    }

    renderer->indexCount = (uint32_t)indices.size();

    UINT vertexBufferSize = (UINT)(vertices.size() * sizeof(Vertex));
    UINT indexBufferSize = (UINT)(indices.size() * sizeof(uint32_t));

    // Create vertex buffer
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = vertexBufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(renderer->device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&renderer->vertexBuffer))))
    {
        return false;
    }

    void* mappedData;
    renderer->vertexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, vertices.data(), vertexBufferSize);
    renderer->vertexBuffer->Unmap(0, nullptr);

    renderer->vertexBufferView.BufferLocation = renderer->vertexBuffer->GetGPUVirtualAddress();
    renderer->vertexBufferView.SizeInBytes = vertexBufferSize;
    renderer->vertexBufferView.StrideInBytes = sizeof(Vertex);

    // Create index buffer
    bufferDesc.Width = indexBufferSize;

    if (FAILED(renderer->device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&renderer->indexBuffer))))
    {
        return false;
    }

    renderer->indexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, indices.data(), indexBufferSize);
    renderer->indexBuffer->Unmap(0, nullptr);

    renderer->indexBufferView.BufferLocation = renderer->indexBuffer->GetGPUVirtualAddress();
    renderer->indexBufferView.SizeInBytes = indexBufferSize;
    renderer->indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    return true;
}

static bool CreateConstantBuffers(D3D12Renderer* renderer)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    // Align to 256 bytes
    const UINT cbSize = (sizeof(CameraConstants) + 255) & ~255;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = cbSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(renderer->device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&renderer->constantBuffer[i]))))
        {
            return false;
        }

        renderer->constantBuffer[i]->Map(0, nullptr, (void**)&renderer->constantBufferMapped[i]);
    }

    return true;
}

bool D3D12_Init(D3D12Renderer* renderer, HWND hwnd, uint32_t width, uint32_t height)
{
    renderer->width = width;
    renderer->height = height;

    UINT dxgiFactoryFlags = 0;

#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    if (FAILED(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&renderer->factory))))
    {
        OutputDebugStringA("Failed to create DXGI factory\n");
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; renderer->factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&renderer->device))))
        {
            char adapterName[256];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
            char msg[512];
            snprintf(msg, sizeof(msg), "Using GPU: %s\n", adapterName);
            OutputDebugStringA(msg);
            break;
        }
    }

    if (!renderer->device)
    {
        OutputDebugStringA("Failed to create D3D12 device\n");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    if (FAILED(renderer->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&renderer->commandQueue))))
    {
        OutputDebugStringA("Failed to create command queue\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(renderer->factory->CreateSwapChainForHwnd(
        renderer->commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1)))
    {
        OutputDebugStringA("Failed to create swap chain\n");
        return false;
    }

    renderer->factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    swapChain1.As(&renderer->swapChain);
    renderer->frameIndex = renderer->swapChain->GetCurrentBackBufferIndex();

    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    if (FAILED(renderer->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&renderer->rtvHeap))))
    {
        OutputDebugStringA("Failed to create RTV heap\n");
        return false;
    }

    renderer->rtvDescriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // DSV heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

    if (FAILED(renderer->device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&renderer->dsvHeap))))
    {
        OutputDebugStringA("Failed to create DSV heap\n");
        return false;
    }

    // Create RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(renderer->swapChain->GetBuffer(i, IID_PPV_ARGS(&renderer->renderTargets[i]))))
        {
            OutputDebugStringA("Failed to get swap chain buffer\n");
            return false;
        }
        renderer->device->CreateRenderTargetView(renderer->renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += renderer->rtvDescriptorSize;
    }

    // Command allocators
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(renderer->device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&renderer->commandAllocators[i]))))
        {
            OutputDebugStringA("Failed to create command allocator\n");
            return false;
        }
    }

    // Command list
    if (FAILED(renderer->device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, renderer->commandAllocators[0].Get(),
        nullptr, IID_PPV_ARGS(&renderer->commandList))))
    {
        OutputDebugStringA("Failed to create command list\n");
        return false;
    }
    renderer->commandList->Close();

    // Fence
    if (FAILED(renderer->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&renderer->fence))))
    {
        OutputDebugStringA("Failed to create fence\n");
        return false;
    }

    renderer->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!renderer->fenceEvent)
    {
        OutputDebugStringA("Failed to create fence event\n");
        return false;
    }

    renderer->fenceValues[renderer->frameIndex] = 1;

    // Create depth buffer
    if (!CreateDepthBuffer(renderer))
    {
        OutputDebugStringA("Failed to create depth buffer\n");
        return false;
    }

    // Create pipeline
    if (!CreatePipelineState(renderer))
    {
        OutputDebugStringA("Failed to create pipeline state\n");
        return false;
    }

    // Create geometry
    if (!CreateGeometry(renderer))
    {
        OutputDebugStringA("Failed to create geometry\n");
        return false;
    }

    // Create constant buffers
    if (!CreateConstantBuffers(renderer))
    {
        OutputDebugStringA("Failed to create constant buffers\n");
        return false;
    }

    // Create ImGui SRV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(renderer->device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&renderer->imguiSrvHeap))))
    {
        OutputDebugStringA("Failed to create ImGui SRV heap\n");
        return false;
    }

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    // Use new ImGui DX12 init API with command queue
    ImGui_ImplDX12_InitInfo initInfo = {};
    initInfo.Device = renderer->device.Get();
    initInfo.CommandQueue = renderer->commandQueue.Get();
    initInfo.NumFramesInFlight = FRAME_COUNT;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.SrvDescriptorHeap = renderer->imguiSrvHeap.Get();
    initInfo.LegacySingleSrvCpuDescriptor = renderer->imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
    initInfo.LegacySingleSrvGpuDescriptor = renderer->imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    ImGui_ImplDX12_Init(&initInfo);

    OutputDebugStringA("D3D12 initialized successfully\n");
    return true;
}

void D3D12_Shutdown(D3D12Renderer* renderer)
{
    D3D12_WaitForGpu(renderer);

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (renderer->constantBuffer[i])
            renderer->constantBuffer[i]->Unmap(0, nullptr);
    }

    if (renderer->fenceEvent)
    {
        CloseHandle(renderer->fenceEvent);
        renderer->fenceEvent = nullptr;
    }
}

void D3D12_WaitForGpu(D3D12Renderer* renderer)
{
    if (!renderer->commandQueue || !renderer->fence || !renderer->fenceEvent)
        return;

    const uint64_t fenceValue = renderer->fenceValues[renderer->frameIndex];
    renderer->commandQueue->Signal(renderer->fence.Get(), fenceValue);

    WaitForFence(renderer, fenceValue);

    renderer->fenceValues[renderer->frameIndex]++;
}

void D3D12_Render(D3D12Renderer* renderer)
{
    // Update constant buffer
    float aspect = (float)renderer->width / (float)renderer->height;
    CameraConstants* cb = renderer->constantBufferMapped[renderer->frameIndex];
    cb->viewProjection = renderer->camera.getViewProjectionMatrix(aspect);
    cb->cameraPos = renderer->camera.position;

    // Reset command allocator and command list
    renderer->commandAllocators[renderer->frameIndex]->Reset();
    renderer->commandList->Reset(renderer->commandAllocators[renderer->frameIndex].Get(), renderer->pipelineState.Get());

    // Set root signature
    renderer->commandList->SetGraphicsRootSignature(renderer->rootSignature.Get());
    renderer->commandList->SetGraphicsRootConstantBufferView(0, renderer->constantBuffer[renderer->frameIndex]->GetGPUVirtualAddress());

    // Transition render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = renderer->renderTargets[renderer->frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    renderer->commandList->ResourceBarrier(1, &barrier);

    // Get descriptor handles
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += renderer->frameIndex * renderer->rtvDescriptorSize;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = renderer->dsvHeap->GetCPUDescriptorHandleForHeapStart();

    // Clear
    const float clearColor[] = { 0.5f, 0.6f, 0.7f, 1.0f }; // Sky color
    renderer->commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    renderer->commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set render targets
    renderer->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // Viewport and scissor
    D3D12_VIEWPORT viewport = {};
    viewport.Width = (float)renderer->width;
    viewport.Height = (float)renderer->height;
    viewport.MaxDepth = 1.0f;
    renderer->commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissorRect = { 0, 0, (LONG)renderer->width, (LONG)renderer->height };
    renderer->commandList->RSSetScissorRects(1, &scissorRect);

    // Draw plane
    renderer->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer->commandList->IASetVertexBuffers(0, 1, &renderer->vertexBufferView);
    renderer->commandList->IASetIndexBuffer(&renderer->indexBufferView);
    renderer->commandList->DrawIndexedInstanced(renderer->indexCount, 1, 0, 0, 0);

    // Render ImGui
    ID3D12DescriptorHeap* descriptorHeaps[] = { renderer->imguiSrvHeap.Get() };
    renderer->commandList->SetDescriptorHeaps(1, descriptorHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), renderer->commandList.Get());

    // Transition to present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    renderer->commandList->ResourceBarrier(1, &barrier);

    renderer->commandList->Close();

    ID3D12CommandList* commandLists[] = { renderer->commandList.Get() };
    renderer->commandQueue->ExecuteCommandLists(1, commandLists);

    renderer->swapChain->Present(1, 0);

    MoveToNextFrame(renderer);
}

void D3D12_Resize(D3D12Renderer* renderer, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    D3D12_WaitForGpu(renderer);

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        renderer->renderTargets[i].Reset();
        renderer->fenceValues[i] = renderer->fenceValues[renderer->frameIndex];
    }

    renderer->depthBuffer.Reset();

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    renderer->swapChain->GetDesc(&swapChainDesc);
    renderer->swapChain->ResizeBuffers(FRAME_COUNT, width, height,
        swapChainDesc.BufferDesc.Format, swapChainDesc.Flags);

    renderer->frameIndex = renderer->swapChain->GetCurrentBackBufferIndex();
    renderer->width = width;
    renderer->height = height;

    // Recreate RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = renderer->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        renderer->swapChain->GetBuffer(i, IID_PPV_ARGS(&renderer->renderTargets[i]));
        renderer->device->CreateRenderTargetView(renderer->renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += renderer->rtvDescriptorSize;
    }

    // Recreate depth buffer
    CreateDepthBuffer(renderer);
}
