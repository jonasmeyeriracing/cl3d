#include "d3d12_renderer.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <cfloat>
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

static bool CreateShadowDepthBuffer(D3D12Renderer* renderer)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = D3D12Renderer::SHADOW_MAP_SIZE;
    depthDesc.Height = D3D12Renderer::SHADOW_MAP_SIZE;
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
        IID_PPV_ARGS(&renderer->shadowDepthBuffer))))
    {
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    // Get second descriptor in the DSV heap for shadow map
    UINT dsvDescriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = renderer->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    dsvHandle.ptr += dsvDescriptorSize;  // Second slot for shadow map
    renderer->device->CreateDepthStencilView(renderer->shadowDepthBuffer.Get(), &dsvDesc, dsvHandle);

    return true;
}

static bool CreateConeShadowMaps(D3D12Renderer* renderer)
{
    // Create Texture2DArray for all cone light shadow maps
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = D3D12Renderer::CONE_SHADOW_MAP_SIZE;
    texDesc.Height = D3D12Renderer::CONE_SHADOW_MAP_SIZE;
    texDesc.DepthOrArraySize = MAX_CONE_LIGHTS;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;  // Typeless for DSV/SRV flexibility
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    if (FAILED(renderer->device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clearValue,
        IID_PPV_ARGS(&renderer->coneShadowMaps))))
    {
        OutputDebugStringA("Failed to create cone shadow maps texture array\n");
        return false;
    }

    // Create DSV descriptor heap (one DSV per array slice)
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = MAX_CONE_LIGHTS;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

    if (FAILED(renderer->device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&renderer->coneShadowDsvHeap))))
    {
        OutputDebugStringA("Failed to create cone shadow DSV heap\n");
        return false;
    }

    // Create DSV for each array slice
    UINT dsvDescriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = renderer->coneShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < MAX_CONE_LIGHTS; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;
        dsvDesc.Texture2DArray.MipSlice = 0;

        renderer->device->CreateDepthStencilView(renderer->coneShadowMaps.Get(), &dsvDesc, dsvHandle);
        dsvHandle.ptr += dsvDescriptorSize;
    }

    // Create SRV descriptor heap (shader visible, for sampling in main pass)
    // 2 descriptors: cone shadow maps (t2) and horizon maps (t3)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 2;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(renderer->device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&renderer->coneShadowSrvHeap))))
    {
        OutputDebugStringA("Failed to create cone shadow SRV heap\n");
        return false;
    }

    // Create SRV for the entire texture array
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = MAX_CONE_LIGHTS;

    renderer->device->CreateShaderResourceView(
        renderer->coneShadowMaps.Get(),
        &srvDesc,
        renderer->coneShadowSrvHeap->GetCPUDescriptorHandleForHeapStart()
    );

    return true;
}

// Compute shader for horizon mapping - traces from light position through height map
// Stores the minimum height the light must be at to be visible from each texel
static const char* g_HorizonComputeShaderSource = R"(
// Height map from top-down rendering
Texture2D<float> heightMap : register(t0);

// Output horizon map (one slice per light) - stores required light height for visibility
RWTexture2DArray<float> horizonMaps : register(u0);

cbuffer HorizonParams : register(b0)
{
    float3 lightPos;
    float worldSize;
    float3 worldMin;
    uint lightIndex;
    uint mapSize;
    float nearPlaneY;    // World Y at depth=0
    float farPlaneY;     // World Y at depth=1
    float padding;
};

// Convert depth buffer value to world-space Y height
float DepthToWorldY(float depth)
{
    // Linear interpolation: depth=0 -> nearPlaneY, depth=1 -> farPlaneY
    return nearPlaneY + depth * (farPlaneY - nearPlaneY);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= mapSize || dispatchThreadId.y >= mapSize)
        return;

    // Convert texel to world XZ position
    float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float(mapSize);
    float2 worldXZ;
    worldXZ.x = worldMin.x + uv.x * worldSize;
    worldXZ.y = worldMin.z + uv.y * worldSize;

    // Direction from this texel toward the light (in XZ plane)
    float2 toLightXZ = float2(lightPos.x, lightPos.z) - worldXZ;
    float distToLightXZ = length(toLightXZ);

    // If light is directly above this texel, no horizon occlusion
    if (distToLightXZ < 0.001)
    {
        horizonMaps[uint3(dispatchThreadId.xy, lightIndex)] = -1000.0;  // Any height is visible
        return;
    }

    float2 dirToLight = toLightXZ / distToLightXZ;

    // Trace from this texel toward the light, find the maximum required height
    // The light must be above this height to illuminate this texel
    float maxRequiredHeight = -1000.0;  // Start very low (no occlusion)

    float2 currentTexel = float2(dispatchThreadId.xy) + 0.5;

    // Trace in texel steps toward the light
    int maxSteps = int(mapSize);
    for (int step = 1; step < maxSteps; ++step)
    {
        // Move one texel toward the light
        float2 sampleTexel = currentTexel + dirToLight * float(step);

        // Check bounds
        if (sampleTexel.x < 0 || sampleTexel.x >= float(mapSize) ||
            sampleTexel.y < 0 || sampleTexel.y >= float(mapSize))
            break;

        // Get world XZ of sample
        float2 sampleUV = sampleTexel / float(mapSize);
        float2 sampleWorldXZ = float2(worldMin.x + sampleUV.x * worldSize,
                                       worldMin.z + sampleUV.y * worldSize);

        // Distance from our texel to this sample
        float sampleDistXZ = length(sampleWorldXZ - worldXZ);

        // Have we passed the light?
        if (sampleDistXZ > distToLightXZ)
            break;

        // Sample depth and convert to world Y height
        float depthSample = heightMap.Load(int3(int2(sampleTexel), 0));
        float sampleHeight = DepthToWorldY(depthSample);

        // Calculate what height the light would need to be at to clear this obstacle
        // Using similar triangles: requiredHeight / distToLight = sampleHeight / sampleDist
        // requiredHeight = sampleHeight * distToLight / sampleDist
        if (sampleDistXZ > 0.001)
        {
            float requiredHeight = sampleHeight * distToLightXZ / sampleDistXZ;
            maxRequiredHeight = max(maxRequiredHeight, requiredHeight);
        }
    }

    // Store the minimum height the light needs to be at to illuminate this texel
    horizonMaps[uint3(dispatchThreadId.xy, lightIndex)] = maxRequiredHeight;
}
)";

static bool CreateHorizonMappingResources(D3D12Renderer* renderer)
{
    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    // Create height map texture (R32_FLOAT, will be rendered to via copy from depth buffer)
    D3D12_RESOURCE_DESC heightMapDesc = {};
    heightMapDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    heightMapDesc.Width = D3D12Renderer::HORIZON_MAP_SIZE;
    heightMapDesc.Height = D3D12Renderer::HORIZON_MAP_SIZE;
    heightMapDesc.DepthOrArraySize = 1;
    heightMapDesc.MipLevels = 1;
    heightMapDesc.Format = DXGI_FORMAT_R32_FLOAT;
    heightMapDesc.SampleDesc.Count = 1;
    heightMapDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(renderer->device->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &heightMapDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&renderer->horizonHeightMap))))
    {
        OutputDebugStringA("Failed to create horizon height map\n");
        return false;
    }

    // Create horizon maps texture array (R32_FLOAT, one per light)
    D3D12_RESOURCE_DESC horizonMapsDesc = {};
    horizonMapsDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    horizonMapsDesc.Width = D3D12Renderer::HORIZON_MAP_SIZE;
    horizonMapsDesc.Height = D3D12Renderer::HORIZON_MAP_SIZE;
    horizonMapsDesc.DepthOrArraySize = MAX_CONE_LIGHTS;
    horizonMapsDesc.MipLevels = 1;
    horizonMapsDesc.Format = DXGI_FORMAT_R32_FLOAT;
    horizonMapsDesc.SampleDesc.Count = 1;
    horizonMapsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(renderer->device->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &horizonMapsDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&renderer->horizonMaps))))
    {
        OutputDebugStringA("Failed to create horizon maps texture array\n");
        return false;
    }

    // Create descriptor heap for horizon mapping (SRV for height map, UAV for horizon maps, SRV for horizon maps)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 3;  // Height map SRV, horizon maps UAV, horizon maps SRV
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(renderer->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&renderer->horizonSrvUavHeap))))
    {
        OutputDebugStringA("Failed to create horizon SRV/UAV heap\n");
        return false;
    }

    UINT descriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE heapHandle = renderer->horizonSrvUavHeap->GetCPUDescriptorHandleForHeapStart();

    // Descriptor 0: Height map SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC heightSrvDesc = {};
    heightSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    heightSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    heightSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    heightSrvDesc.Texture2D.MipLevels = 1;
    renderer->device->CreateShaderResourceView(renderer->horizonHeightMap.Get(), &heightSrvDesc, heapHandle);

    // Descriptor 1: Horizon maps UAV
    heapHandle.ptr += descriptorSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC horizonUavDesc = {};
    horizonUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
    horizonUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    horizonUavDesc.Texture2DArray.MipSlice = 0;
    horizonUavDesc.Texture2DArray.FirstArraySlice = 0;
    horizonUavDesc.Texture2DArray.ArraySize = MAX_CONE_LIGHTS;
    renderer->device->CreateUnorderedAccessView(renderer->horizonMaps.Get(), nullptr, &horizonUavDesc, heapHandle);

    // Descriptor 2: Horizon maps SRV (for main shader sampling)
    heapHandle.ptr += descriptorSize;
    D3D12_SHADER_RESOURCE_VIEW_DESC horizonSrvDesc = {};
    horizonSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    horizonSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    horizonSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    horizonSrvDesc.Texture2DArray.MipLevels = 1;
    horizonSrvDesc.Texture2DArray.FirstArraySlice = 0;
    horizonSrvDesc.Texture2DArray.ArraySize = MAX_CONE_LIGHTS;
    renderer->device->CreateShaderResourceView(renderer->horizonMaps.Get(), &horizonSrvDesc, heapHandle);

    // Create compute root signature
    D3D12_ROOT_PARAMETER computeParams[3] = {};

    // Constant buffer with light params at b0
    computeParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParams[0].Constants.ShaderRegister = 0;
    computeParams[0].Constants.RegisterSpace = 0;
    computeParams[0].Constants.Num32BitValues = 12;  // float3 lightPos, float worldSize, float3 worldMin, uint lightIndex, uint mapSize, float3 padding
    computeParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Height map SRV at t0
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    computeParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[1].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    computeParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Horizon maps UAV at u0
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    computeParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParams[2].DescriptorTable.NumDescriptorRanges = 1;
    computeParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    computeParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeRootSigDesc = {};
    computeRootSigDesc.NumParameters = 3;
    computeRootSigDesc.pParameters = computeParams;
    computeRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&computeRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    if (FAILED(renderer->device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&renderer->horizonComputeRootSig))))
    {
        OutputDebugStringA("Failed to create horizon compute root signature\n");
        return false;
    }

    // Compile compute shader
    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> computeShader;
    if (FAILED(D3DCompile(g_HorizonComputeShaderSource, strlen(g_HorizonComputeShaderSource), "horizon.hlsl", nullptr, nullptr,
        "CSMain", "cs_5_0", compileFlags, 0, &computeShader, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    // Create compute PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
    computePsoDesc.pRootSignature = renderer->horizonComputeRootSig.Get();
    computePsoDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };

    if (FAILED(renderer->device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&renderer->horizonComputePSO))))
    {
        OutputDebugStringA("Failed to create horizon compute PSO\n");
        return false;
    }

    // Add horizon maps SRV to coneShadowSrvHeap at descriptor slot 1 for main render pass
    UINT mainHeapDescriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE mainHeapHandle = renderer->coneShadowSrvHeap->GetCPUDescriptorHandleForHeapStart();
    mainHeapHandle.ptr += mainHeapDescriptorSize;  // Skip to descriptor 1

    D3D12_SHADER_RESOURCE_VIEW_DESC horizonMainSrvDesc = {};
    horizonMainSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    horizonMainSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    horizonMainSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    horizonMainSrvDesc.Texture2DArray.MipLevels = 1;
    horizonMainSrvDesc.Texture2DArray.FirstArraySlice = 0;
    horizonMainSrvDesc.Texture2DArray.ArraySize = MAX_CONE_LIGHTS;
    renderer->device->CreateShaderResourceView(renderer->horizonMaps.Get(), &horizonMainSrvDesc, mainHeapHandle);

    OutputDebugStringA("Horizon mapping resources created successfully\n");
    return true;
}

static const char* g_ShaderSource = R"(
cbuffer CameraConstants : register(b0)
{
    float4x4 viewProjection;
    float3 cameraPos;
    float numConeLights;
    float ambientIntensity;
    float coneLightIntensity;
    float shadowBias;
    float falloffExponent;
    float debugLightOverlap;
    float overlapMaxCount;
    float disableShadows;
    float useHorizonMapping;
    float horizonWorldMinX;
    float horizonWorldMinZ;
    float horizonWorldSize;
};

struct ConeLight
{
    float4 positionAndRange;
    float4 directionAndCosOuter;
    float4 colorAndCosInner;
};

StructuredBuffer<ConeLight> coneLights : register(t0);
StructuredBuffer<float4x4> lightMatrices : register(t1);
Texture2DArray<float> coneShadowMaps : register(t2);
Texture2DArray<float> horizonMaps : register(t3);
SamplerComparisonState shadowSampler : register(s0);
SamplerState linearSampler : register(s1);

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

float CalculateShadow(float3 worldPos, int lightIndex)
{
    float4x4 lightVP = lightMatrices[lightIndex];
    float4 lightSpacePos = mul(lightVP, float4(worldPos, 1.0));

    // Perspective divide
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

    // Check if outside light frustum
    if (projCoords.z < 0.0 || projCoords.z > 1.0)
        return 1.0;

    // Convert XY from NDC [-1,1] to UV [0,1]
    float2 shadowUV = projCoords.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    // Check bounds
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0)
        return 1.0;

    // Sample shadow map
    int3 texCoord = int3(shadowUV * 256.0, lightIndex);
    float shadowDepth = coneShadowMaps.Load(int4(texCoord, 0));

    // DEBUG: Show colors based on comparison
    // projCoords.z is our depth, shadowDepth is stored depth
    // If projCoords.z > shadowDepth, we're behind something (shadowed)
    float bias = 0.005;

    // Return 1.0 if lit (our depth <= shadow depth), 0.0 if shadowed
    return (projCoords.z - bias) <= shadowDepth ? 1.0 : 0.0;
}

// Convert HSV to RGB (h, s, v all in [0, 1])
float3 HSVtoRGB(float h, float s, float v)
{
    float3 rgb;
    float c = v * s;
    float hPrime = h * 6.0;
    float x = c * (1.0 - abs(fmod(hPrime, 2.0) - 1.0));
    float m = v - c;

    if (hPrime < 1.0)
        rgb = float3(c, x, 0.0);
    else if (hPrime < 2.0)
        rgb = float3(x, c, 0.0);
    else if (hPrime < 3.0)
        rgb = float3(0.0, c, x);
    else if (hPrime < 4.0)
        rgb = float3(0.0, x, c);
    else if (hPrime < 5.0)
        rgb = float3(x, 0.0, c);
    else
        rgb = float3(c, 0.0, x);

    return rgb + float3(m, m, m);
}

// Map intensity [0, 1] to heat color using hue [0, 0.9]
// 0 = red (hue 0), 1 = magenta (hue 0.9)
float3 IntensityToHeatColor(float intensity)
{
    float hue = saturate(intensity) * 0.9;
    return HSVtoRGB(hue, 1.0, 1.0);
}

// Calculate horizon-based shadow using precomputed required light heights
float CalculateHorizonShadow(float3 worldPos, float3 lightPos, int lightIndex)
{
    // Convert world position to horizon map UV
    float2 uv;
    uv.x = (worldPos.x - horizonWorldMinX) / horizonWorldSize;
    uv.y = (worldPos.z - horizonWorldMinZ) / horizonWorldSize;

    // Check bounds
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;  // Outside horizon map, no shadow

    // Sample required height using bilinear filtering
    float requiredHeight = horizonMaps.SampleLevel(linearSampler, float3(uv, lightIndex), 0);

    // Soft shadow: smoothstep over a height range
    float bias = 0.1;
    float softness = 1.0;  // Height range for soft transition
    float clearance = lightPos.y - (requiredHeight + bias);
    return saturate(clearance / softness);
}

float3 CalculateConeLightContribution(float3 worldPos, float3 normal, ConeLight light, int lightIndex)
{
    float3 lightPos = light.positionAndRange.xyz;
    float range = light.positionAndRange.w;
    float3 lightDir = light.directionAndCosOuter.xyz;
    float cosOuter = light.directionAndCosOuter.w;
    float3 lightColor = light.colorAndCosInner.xyz;
    float cosInner = light.colorAndCosInner.w;

    float3 toLight = lightPos - worldPos;
    float dist = length(toLight);
    if (dist > range) return float3(0, 0, 0);

    float3 toLightNorm = toLight / dist;
    float cosAngle = dot(-toLightNorm, lightDir);
    if (cosAngle < cosOuter) return float3(0, 0, 0);

    float coneAtten = saturate((cosAngle - cosOuter) / (cosInner - cosOuter));
    float distAtten = saturate(1.0 - dist / range);
    distAtten = pow(distAtten, falloffExponent);
    float ndotl = saturate(dot(normal, toLightNorm));

    // Compute shadow (skip if disabled)
    float shadow = 1.0;
    if (disableShadows < 0.5)
    {
        if (useHorizonMapping > 0.5)
        {
            // Use horizon mapping for shadows
            shadow = CalculateHorizonShadow(worldPos, lightPos, lightIndex);
        }
        else
        {
            // Use traditional shadow mapping
            float4x4 lightVP = lightMatrices[lightIndex];
            float4 lightSpacePos = mul(lightVP, float4(worldPos, 1.0));
            float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;

            float2 shadowUV = projCoords.xy * 0.5 + 0.5;
            shadowUV.y = 1.0 - shadowUV.y;

            int3 texCoord = int3(shadowUV * 256.0, lightIndex);
            float shadowDepth = coneShadowMaps.Load(int4(texCoord, 0));

            // Shadow comparison: lit if fragment depth <= shadow depth + bias
            shadow = (projCoords.z <= shadowDepth + shadowBias) ? 1.0 : 0.0;
        }
    }

    return lightColor * ndotl * coneAtten * distAtten * shadow;
}

// Convert light count to heat map color (green -> yellow -> red)
float3 LightCountToColor(int count)
{
    // 0 = green, 60 = yellow, 120 = red
    float t = saturate(count / 120.0);

    if (t < 0.5)
    {
        // Green to Yellow (0-60 lights)
        float s = t * 2.0;  // 0 to 1
        return float3(s, 1.0, 0.0);
    }
    else
    {
        // Yellow to Red (60-120 lights)
        float s = (t - 0.5) * 2.0;  // 0 to 1
        return float3(1.0, 1.0 - s, 0.0);
    }
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // Debug mode: show light overlap heat map
    if (debugLightOverlap > 0.5)
    {
        int lightCount = (int)numConeLights;
        float overlapCount = 0.0;

        for (int i = 0; i < lightCount; i++)
        {
            float3 contribution = CalculateConeLightContribution(input.worldPos, input.normal, coneLights[i], i);
            // Count as 1.0 if any light contribution
            float total = dot(contribution, float3(1, 1, 1));
            overlapCount += step(0.000001, total);
        }

        // Convert count to heat color using hue gradient (0=red, max=magenta)
        float t = saturate(overlapCount / overlapMaxCount);
        float3 heatColor = IntensityToHeatColor(t);
        return float4(heatColor, 1.0);
    }

    // Normal rendering
    float3 color;
    bool isGround = (input.normal.y > 0.9 && abs(input.worldPos.y) < 0.1);

    if (isGround)
    {
        float2 grid = frac(input.uv * 100.0);
        float lineWidth = 0.02;
        float gridLine = (grid.x < lineWidth || grid.y < lineWidth) ? 1.0 : 0.0;
        float3 baseColor = float3(0.3, 0.3, 0.3);
        float3 lineColor = float3(0.2, 0.2, 0.2);
        color = lerp(baseColor, lineColor, gridLine) * ambientIntensity;
    }
    else
    {
        float3 lightDir = normalize(float3(0.5, 1.0, 0.3));
        float ndotl = saturate(dot(input.normal, lightDir));
        float3 boxColor = float3(0.85, 0.85, 0.85);
        color = boxColor * (ambientIntensity + (1.0 - ambientIntensity) * ndotl);
    }

    int lightCount = (int)numConeLights;
    for (int i = 0; i < lightCount; i++)
    {
        color += CalculateConeLightContribution(input.worldPos, input.normal, coneLights[i], i) * coneLightIntensity;
    }

    float dist = length(input.worldPos - cameraPos);
    float fog = saturate(dist / 2000.0);
    float3 fogColor = float3(0.5, 0.6, 0.7);
    color = lerp(color, fogColor, fog);

    return float4(color, 1.0);
}
)";

static const char* g_DebugShaderSource = R"(
cbuffer CameraConstants : register(b0)
{
    float4x4 viewProjection;
    float3 cameraPos;
    float padding;
};

struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(viewProjection, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return float4(input.color, 1.0);
}
)";

// Shadow pass vertex shader - uses root constants at b1 for view-projection
static const char* g_ShadowShaderSource = R"(
cbuffer ShadowViewProj : register(b1)
{
    float4x4 shadowViewProjection;
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
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(shadowViewProjection, float4(input.position, 1.0));
    return output;
}
)";

static const char* g_FullscreenShaderSource = R"(
Texture2DArray<float> depthTexture : register(t0);
SamplerState depthSampler : register(s0);

cbuffer SliceIndex : register(b0)
{
    int sliceIndex;
    int3 padding;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    // Generate fullscreen triangle
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv.y = 1.0 - output.uv.y;  // Flip Y for texture sampling
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float depth = depthTexture.Sample(depthSampler, float3(input.uv, sliceIndex));
    // Remap depth for better visualization
    // Depth 1.0 = far (cleared value), depth < 1.0 = geometry
    // Scale and invert for visibility: near objects = white, far = darker
    float visualDepth = saturate(1.0 - depth);
    // Boost contrast for better visibility
    visualDepth = pow(visualDepth, 0.3);
    return float4(visualDepth, visualDepth, visualDepth, 1.0);
}
)";

static bool CreatePipelineState(D3D12Renderer* renderer)
{
    // Create root signature with:
    // - CBV for camera constants (b0)
    // - SRV for cone lights (t0)
    // - SRV for light matrices (t1)
    // - Descriptor table for cone shadow maps (t2)
    // - Root constants for shadow pass view-projection (b1) - 16 floats
    // - Descriptor table for horizon maps (t3)
    D3D12_ROOT_PARAMETER rootParams[6] = {};

    // Camera constants CBV at b0
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Cone lights SRV at t0
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[1].Descriptor.ShaderRegister = 0;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Light matrices SRV at t1
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParams[2].Descriptor.ShaderRegister = 1;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Cone shadow maps descriptor table at t2
    D3D12_DESCRIPTOR_RANGE shadowMapRange = {};
    shadowMapRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowMapRange.NumDescriptors = 1;
    shadowMapRange.BaseShaderRegister = 2;
    shadowMapRange.RegisterSpace = 0;
    shadowMapRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &shadowMapRange;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Root constants for shadow pass view-projection matrix at b1 (16 floats)
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[4].Constants.ShaderRegister = 1;
    rootParams[4].Constants.RegisterSpace = 0;
    rootParams[4].Constants.Num32BitValues = 16;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // Horizon maps descriptor table at t3
    D3D12_DESCRIPTOR_RANGE horizonMapRange = {};
    horizonMapRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    horizonMapRange.NumDescriptors = 1;
    horizonMapRange.BaseShaderRegister = 3;
    horizonMapRange.RegisterSpace = 0;
    horizonMapRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[5].DescriptorTable.pDescriptorRanges = &horizonMapRange;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers
    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};

    // Shadow comparison sampler at s0
    staticSamplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].RegisterSpace = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Linear sampler at s1 (for horizon map sampling)
    staticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[1].ShaderRegister = 1;
    staticSamplers[1].RegisterSpace = 0;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 6;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 2;
    rootSigDesc.pStaticSamplers = staticSamplers;
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

    // Create debug wireframe PSO
    ComPtr<ID3DBlob> debugVS, debugPS;
    if (FAILED(D3DCompile(g_DebugShaderSource, strlen(g_DebugShaderSource), "debug.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, &debugVS, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    if (FAILED(D3DCompile(g_DebugShaderSource, strlen(g_DebugShaderSource), "debug.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, &debugPS, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC debugInputLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc = {};
    debugPsoDesc.InputLayout = { debugInputLayout, _countof(debugInputLayout) };
    debugPsoDesc.pRootSignature = renderer->rootSignature.Get();
    debugPsoDesc.VS = { debugVS->GetBufferPointer(), debugVS->GetBufferSize() };
    debugPsoDesc.PS = { debugPS->GetBufferPointer(), debugPS->GetBufferSize() };
    debugPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    debugPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    debugPsoDesc.RasterizerState.DepthClipEnable = TRUE;
    debugPsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    debugPsoDesc.DepthStencilState.DepthEnable = TRUE;
    debugPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    debugPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    debugPsoDesc.SampleMask = UINT_MAX;
    debugPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    debugPsoDesc.NumRenderTargets = 1;
    debugPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    debugPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    debugPsoDesc.SampleDesc.Count = 1;

    if (FAILED(renderer->device->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&renderer->debugPipelineState))))
    {
        OutputDebugStringA("Failed to create debug PSO\n");
        return false;
    }

    // Compile shadow shader (uses root constants at b1)
    ComPtr<ID3DBlob> shadowVS;
    if (FAILED(D3DCompile(g_ShadowShaderSource, strlen(g_ShadowShaderSource), "shadow.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, &shadowVS, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    // Create shadow map PSO (depth-only, no pixel shader)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPsoDesc = {};
    shadowPsoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    shadowPsoDesc.pRootSignature = renderer->rootSignature.Get();
    shadowPsoDesc.VS = { shadowVS->GetBufferPointer(), shadowVS->GetBufferSize() };
    // No pixel shader - depth only
    shadowPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    shadowPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    shadowPsoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    shadowPsoDesc.RasterizerState.DepthClipEnable = TRUE;
    shadowPsoDesc.DepthStencilState.DepthEnable = TRUE;
    shadowPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    shadowPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    shadowPsoDesc.SampleMask = UINT_MAX;
    shadowPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadowPsoDesc.NumRenderTargets = 0;  // No render target
    shadowPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    shadowPsoDesc.SampleDesc.Count = 1;

    if (FAILED(renderer->device->CreateGraphicsPipelineState(&shadowPsoDesc, IID_PPV_ARGS(&renderer->shadowPipelineState))))
    {
        OutputDebugStringA("Failed to create shadow PSO\n");
        return false;
    }

    return true;
}

static bool CreateFullscreenPipeline(D3D12Renderer* renderer)
{
    // Create root signature for fullscreen pass (root constant + SRV descriptor table + sampler)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2] = {};

    // Root constant for slice index at b0
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 4;  // sliceIndex + padding
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // SRV descriptor table for texture
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 2;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &sampler;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    if (FAILED(renderer->device->CreateRootSignature(0, signature->GetBufferPointer(),
        signature->GetBufferSize(), IID_PPV_ARGS(&renderer->fullscreenRootSignature))))
    {
        return false;
    }

    // Compile fullscreen shaders
    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;

    if (FAILED(D3DCompile(g_FullscreenShaderSource, strlen(g_FullscreenShaderSource), "fullscreen.hlsl", nullptr, nullptr,
        "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    if (FAILED(D3DCompile(g_FullscreenShaderSource, strlen(g_FullscreenShaderSource), "fullscreen.hlsl", nullptr, nullptr,
        "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &error)))
    {
        if (error) OutputDebugStringA((char*)error->GetBufferPointer());
        return false;
    }

    // Create PSO (no input layout - using SV_VertexID)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = renderer->fullscreenRootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(renderer->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderer->fullscreenPipelineState))))
    {
        OutputDebugStringA("Failed to create fullscreen PSO\n");
        return false;
    }

    // Create SRV descriptor heap for shadow map
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(renderer->device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&renderer->shadowSrvHeap))))
    {
        OutputDebugStringA("Failed to create shadow SRV heap\n");
        return false;
    }

    // Create SRV for shadow depth buffer
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;  // Read depth as R32
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;

    renderer->device->CreateShaderResourceView(
        renderer->shadowDepthBuffer.Get(),
        &srvDesc,
        renderer->shadowSrvHeap->GetCPUDescriptorHandleForHeapStart()
    );

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

// Add a rotated box aligned to a direction (forward = direction of travel)
static void AddOrientedBox(std::vector<Vertex>& verts, std::vector<uint32_t>& inds,
                           const Vec3& center, const Vec3& forward, float sx, float sy, float sz)
{
    // Build orientation basis
    Vec3 fwd = forward.normalized();
    Vec3 up(0, 1, 0);
    Vec3 right = cross(up, fwd).normalized();  // Changed order for correct handedness

    // Box half-sizes: X=width, Y=height, Z=length (forward)
    float hx = sx * 0.5f;
    float hy = sy * 0.5f;
    float hz = sz * 0.5f;

    uint32_t base = (uint32_t)verts.size();

    // Helper to transform local position to world
    auto toWorld = [&](float lx, float ly, float lz) -> Vec3 {
        return center + right * lx + up * ly + fwd * lz;
    };

    // Helper to transform local normal to world
    auto normalToWorld = [&](float nx, float ny, float nz) -> Vec3 {
        return (right * nx + up * ny + fwd * nz).normalized();
    };

    // Front face (forward +Z local = +fwd world)
    Vec3 nFront = normalToWorld(0, 0, 1);
    Vec3 p0 = toWorld(-hx, -hy, hz); verts.push_back({{p0.x, p0.y, p0.z}, {nFront.x, nFront.y, nFront.z}, {0,0}});
    Vec3 p1 = toWorld( hx, -hy, hz); verts.push_back({{p1.x, p1.y, p1.z}, {nFront.x, nFront.y, nFront.z}, {1,0}});
    Vec3 p2 = toWorld( hx,  hy, hz); verts.push_back({{p2.x, p2.y, p2.z}, {nFront.x, nFront.y, nFront.z}, {1,1}});
    Vec3 p3 = toWorld(-hx,  hy, hz); verts.push_back({{p3.x, p3.y, p3.z}, {nFront.x, nFront.y, nFront.z}, {0,1}});

    // Back face (-Z local = -fwd world)
    Vec3 nBack = normalToWorld(0, 0, -1);
    Vec3 p4 = toWorld( hx, -hy, -hz); verts.push_back({{p4.x, p4.y, p4.z}, {nBack.x, nBack.y, nBack.z}, {0,0}});
    Vec3 p5 = toWorld(-hx, -hy, -hz); verts.push_back({{p5.x, p5.y, p5.z}, {nBack.x, nBack.y, nBack.z}, {1,0}});
    Vec3 p6 = toWorld(-hx,  hy, -hz); verts.push_back({{p6.x, p6.y, p6.z}, {nBack.x, nBack.y, nBack.z}, {1,1}});
    Vec3 p7 = toWorld( hx,  hy, -hz); verts.push_back({{p7.x, p7.y, p7.z}, {nBack.x, nBack.y, nBack.z}, {0,1}});

    // Right face (+X local = +right world)
    Vec3 nRight = normalToWorld(1, 0, 0);
    Vec3 p8  = toWorld(hx, -hy,  hz); verts.push_back({{p8.x,  p8.y,  p8.z},  {nRight.x, nRight.y, nRight.z}, {0,0}});
    Vec3 p9  = toWorld(hx, -hy, -hz); verts.push_back({{p9.x,  p9.y,  p9.z},  {nRight.x, nRight.y, nRight.z}, {1,0}});
    Vec3 p10 = toWorld(hx,  hy, -hz); verts.push_back({{p10.x, p10.y, p10.z}, {nRight.x, nRight.y, nRight.z}, {1,1}});
    Vec3 p11 = toWorld(hx,  hy,  hz); verts.push_back({{p11.x, p11.y, p11.z}, {nRight.x, nRight.y, nRight.z}, {0,1}});

    // Left face (-X local = -right world)
    Vec3 nLeft = normalToWorld(-1, 0, 0);
    Vec3 p12 = toWorld(-hx, -hy, -hz); verts.push_back({{p12.x, p12.y, p12.z}, {nLeft.x, nLeft.y, nLeft.z}, {0,0}});
    Vec3 p13 = toWorld(-hx, -hy,  hz); verts.push_back({{p13.x, p13.y, p13.z}, {nLeft.x, nLeft.y, nLeft.z}, {1,0}});
    Vec3 p14 = toWorld(-hx,  hy,  hz); verts.push_back({{p14.x, p14.y, p14.z}, {nLeft.x, nLeft.y, nLeft.z}, {1,1}});
    Vec3 p15 = toWorld(-hx,  hy, -hz); verts.push_back({{p15.x, p15.y, p15.z}, {nLeft.x, nLeft.y, nLeft.z}, {0,1}});

    // Top face (+Y local = +up world)
    Vec3 nTop = normalToWorld(0, 1, 0);
    Vec3 p16 = toWorld(-hx, hy,  hz); verts.push_back({{p16.x, p16.y, p16.z}, {nTop.x, nTop.y, nTop.z}, {0,0}});
    Vec3 p17 = toWorld( hx, hy,  hz); verts.push_back({{p17.x, p17.y, p17.z}, {nTop.x, nTop.y, nTop.z}, {1,0}});
    Vec3 p18 = toWorld( hx, hy, -hz); verts.push_back({{p18.x, p18.y, p18.z}, {nTop.x, nTop.y, nTop.z}, {1,1}});
    Vec3 p19 = toWorld(-hx, hy, -hz); verts.push_back({{p19.x, p19.y, p19.z}, {nTop.x, nTop.y, nTop.z}, {0,1}});

    // Bottom face (-Y local = -up world)
    Vec3 nBottom = normalToWorld(0, -1, 0);
    Vec3 p20 = toWorld(-hx, -hy, -hz); verts.push_back({{p20.x, p20.y, p20.z}, {nBottom.x, nBottom.y, nBottom.z}, {0,0}});
    Vec3 p21 = toWorld( hx, -hy, -hz); verts.push_back({{p21.x, p21.y, p21.z}, {nBottom.x, nBottom.y, nBottom.z}, {1,0}});
    Vec3 p22 = toWorld( hx, -hy,  hz); verts.push_back({{p22.x, p22.y, p22.z}, {nBottom.x, nBottom.y, nBottom.z}, {1,1}});
    Vec3 p23 = toWorld(-hx, -hy,  hz); verts.push_back({{p23.x, p23.y, p23.z}, {nBottom.x, nBottom.y, nBottom.z}, {0,1}});

    // Indices for 6 faces (2 triangles each)
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

// Helper function to get position and direction on the oval track
// Progress: 0-1 around the track
// Returns position and forward direction
static void GetTrackPositionAndDirection(float progress, float straightLength, float radius,
                                          Vec3& outPos, Vec3& outDir)
{
    const float PI = 3.14159265f;

    // Track layout (counterclockwise):
    // - Bottom straight: progress 0 to 0.25 (going +X)
    // - Right semicircle: progress 0.25 to 0.5 (turning around)
    // - Top straight: progress 0.5 to 0.75 (going -X)
    // - Left semicircle: progress 0.75 to 1.0 (turning around)

    float totalStraight = straightLength * 2.0f;
    float totalCurve = 2.0f * PI * radius;
    float totalLength = totalStraight + totalCurve;

    float straightFraction = totalStraight / totalLength;
    float curveFraction = totalCurve / totalLength;
    float singleStraightFrac = straightFraction * 0.5f;
    float singleCurveFrac = curveFraction * 0.5f;

    float halfStraight = straightLength * 0.5f;

    if (progress < singleStraightFrac)
    {
        // Bottom straight (going +X direction)
        float t = progress / singleStraightFrac;
        outPos = Vec3(-halfStraight + t * straightLength, 0, -radius);
        outDir = Vec3(1, 0, 0);
    }
    else if (progress < singleStraightFrac + singleCurveFrac)
    {
        // Right semicircle
        float t = (progress - singleStraightFrac) / singleCurveFrac;
        float angle = -PI * 0.5f + t * PI;  // -90 to +90 degrees
        outPos = Vec3(halfStraight + cosf(angle) * radius, 0, sinf(angle) * radius);
        outDir = Vec3(-sinf(angle), 0, cosf(angle));
    }
    else if (progress < 2.0f * singleStraightFrac + singleCurveFrac)
    {
        // Top straight (going -X direction)
        float t = (progress - singleStraightFrac - singleCurveFrac) / singleStraightFrac;
        outPos = Vec3(halfStraight - t * straightLength, 0, radius);
        outDir = Vec3(-1, 0, 0);
    }
    else
    {
        // Left semicircle
        float t = (progress - 2.0f * singleStraightFrac - singleCurveFrac) / singleCurveFrac;
        float angle = PI * 0.5f + t * PI;  // +90 to +270 degrees
        outPos = Vec3(-halfStraight + cosf(angle) * radius, 0, sinf(angle) * radius);
        outDir = Vec3(-sinf(angle), 0, cosf(angle));
    }
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

    // Car-sized boxes: 4m long, 2m wide, 1.5m tall
    const float carLength = 4.0f;
    const float carWidth = 2.0f;
    const float carHeight = 1.5f;

    // Track parameters
    const float straightLength = renderer->trackStraightLength;
    const float radius = renderer->trackRadius;
    const float laneWidth = renderer->trackLaneWidth;
    const float PI = 3.14159265f;

    // Calculate total track length
    renderer->trackLength = straightLength * 2.0f + 2.0f * PI * radius;

    // 60 cars in 2 lanes
    const int numCars = 60;
    const int carsPerLane = numCars / 2;
    renderer->numCars = numCars;

    // Record where car vertices start (after ground plane)
    renderer->carVertexStartIndex = (uint32_t)vertices.size();

    // Headlight parameters
    const float headlightHeight = 0.6f;
    const float headlightSpacing = 0.7f;
    const float headlightRange = 30.0f;
    const float headlightInnerAngle = 0.15f;
    const float headlightOuterAngle = 0.35f;
    const Vec3 headlightColor(1.5f, 1.4f, 1.2f);

    renderer->numConeLights = 0;

    // Initialize AABB for track bounds
    renderer->carAABB.min = Vec3(-straightLength * 0.5f - radius - 20.0f, 0, -radius - 20.0f);
    renderer->carAABB.max = Vec3(straightLength * 0.5f + radius + 20.0f, carHeight, radius + 20.0f);

    // Spacing between cars along track (as fraction of track length)
    float carSpacing = 1.0f / (float)carsPerLane;

    for (int i = 0; i < numCars; i++)
    {
        int lane = i % 2;  // 0 = inner lane, 1 = outer lane
        int posInLane = i / 2;

        // Initial progress along track (evenly spaced within each lane)
        float progress = (float)posInLane * carSpacing;
        renderer->carTrackProgress[i] = progress;

        // Lane offset (negative = inner, positive = outer)
        renderer->carLane[i] = (lane == 0) ? -laneWidth * 0.5f : laneWidth * 0.5f;

        // Get position and direction on track centerline
        Vec3 trackPos, trackDir;
        GetTrackPositionAndDirection(progress, straightLength, radius, trackPos, trackDir);

        // Offset by lane
        Vec3 trackRight(trackDir.z, 0, -trackDir.x);  // Perpendicular to direction
        Vec3 carPos = trackPos + trackRight * renderer->carLane[i];
        carPos.y = carHeight * 0.5f;

        // Add car box aligned to track direction
        AddOrientedBox(vertices, indices, carPos, trackDir, carWidth, carHeight, carLength);

        // Add two headlights for this car
        float frontOffset = carLength * 0.5f;
        Vec3 frontPos = carPos + trackDir * frontOffset;
        frontPos.y = headlightHeight;

        // Left headlight
        if (renderer->numConeLights < MAX_CONE_LIGHTS)
        {
            Vec3 leftOffset = trackRight * (-headlightSpacing);
            ConeLight& light = renderer->coneLights[renderer->numConeLights++];
            light.position = frontPos + leftOffset;
            light.direction = trackDir;
            light.color = headlightColor;
            light.range = headlightRange;
            light.innerAngle = headlightInnerAngle;
            light.outerAngle = headlightOuterAngle;
        }

        // Right headlight
        if (renderer->numConeLights < MAX_CONE_LIGHTS)
        {
            Vec3 rightOffset = trackRight * headlightSpacing;
            ConeLight& light = renderer->coneLights[renderer->numConeLights++];
            light.position = frontPos + rightOffset;
            light.direction = trackDir;
            light.color = headlightColor;
            light.range = headlightRange;
            light.innerAngle = headlightInnerAngle;
            light.outerAngle = headlightOuterAngle;
        }
    }

    // Calculate top-down orthographic view-projection matrix from AABB
    float padding = 20.0f;
    float halfWidth = (renderer->carAABB.max.x - renderer->carAABB.min.x) * 0.5f + padding;
    float halfDepth = (renderer->carAABB.max.z - renderer->carAABB.min.z) * 0.5f + padding;

    // Use the larger dimension for both axes to maintain 1:1 world space aspect ratio
    float halfSize = (halfWidth > halfDepth) ? halfWidth : halfDepth;

    // Create top-down view matrix (looking down from above)
    float viewHeight = renderer->carAABB.max.y + 50.0f;
    Vec3 eyePos(
        (renderer->carAABB.min.x + renderer->carAABB.max.x) * 0.5f,
        viewHeight,
        (renderer->carAABB.min.z + renderer->carAABB.max.z) * 0.5f
    );
    Vec3 targetPos(eyePos.x, 0, eyePos.z);
    Vec3 upDir(0, 0, -1);  // Z- is "up" when looking down

    Mat4 topDownView = Mat4::lookAt(eyePos, targetPos, upDir);

    // Orthographic projection bounds are in view space after lookAt transform
    // View X = world X, View Y = world -Z, View Z = world -Y (depth)
    // Use same size for both axes for 1:1 aspect ratio
    float nearZ = 0.1f;
    float farZ = viewHeight + 10.0f;  // Far enough to capture ground

    Mat4 topDownProj = Mat4::orthographic(-halfSize, halfSize, -halfSize, halfSize, nearZ, farZ);
    renderer->topDownViewProj = topDownProj * topDownView;

    // Store horizon mapping world bounds (matches the top-down view)
    renderer->horizonWorldMin = Vec3(eyePos.x - halfSize, 0, eyePos.z - halfSize);
    renderer->horizonWorldSize = halfSize * 2.0f;

    renderer->indexCount = (uint32_t)indices.size();
    renderer->carVertexCount = (uint32_t)vertices.size() - renderer->carVertexStartIndex;

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

    // Keep vertex buffer mapped for dynamic car updates
    void* mappedData;
    renderer->vertexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, vertices.data(), vertexBufferSize);
    // Store pointer to car vertices for updates (don't unmap)
    renderer->carVerticesMapped = reinterpret_cast<Vertex*>(mappedData) + renderer->carVertexStartIndex;

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

static bool CreateDebugGeometry(D3D12Renderer* renderer)
{
    std::vector<DebugVertex> debugVerts;
    const int coneSegments = 16;
    const Vec3 coneColor(1.0f, 1.0f, 0.0f);  // Yellow for cone wireframe

    for (uint32_t i = 0; i < renderer->numConeLights; ++i)
    {
        const ConeLight& light = renderer->coneLights[i];
        Vec3 pos = light.position;
        Vec3 dir = light.direction;
        float range = renderer->headlightRange;  // Use slider value
        float outerAngle = light.outerAngle;

        // Calculate basis vectors perpendicular to direction
        Vec3 up = (fabsf(dir.y) < 0.99f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        Vec3 right = cross(dir, up).normalized();
        up = cross(right, dir).normalized();

        // Cone end radius at range distance
        float endRadius = range * tanf(outerAngle);

        // Draw lines from apex to cone circle
        for (int j = 0; j < coneSegments; ++j)
        {
            float angle = (float)j / (float)coneSegments * 6.28318f;
            float nextAngle = (float)(j + 1) / (float)coneSegments * 6.28318f;

            Vec3 offset1 = right * (cosf(angle) * endRadius) + up * (sinf(angle) * endRadius);
            Vec3 offset2 = right * (cosf(nextAngle) * endRadius) + up * (sinf(nextAngle) * endRadius);

            Vec3 endPoint1 = pos + dir * range + offset1;
            Vec3 endPoint2 = pos + dir * range + offset2;

            // Line from apex to edge
            debugVerts.push_back({{pos.x, pos.y, pos.z}, {coneColor.x, coneColor.y, coneColor.z}});
            debugVerts.push_back({{endPoint1.x, endPoint1.y, endPoint1.z}, {coneColor.x, coneColor.y, coneColor.z}});

            // Line around the circle edge
            debugVerts.push_back({{endPoint1.x, endPoint1.y, endPoint1.z}, {coneColor.x, coneColor.y, coneColor.z}});
            debugVerts.push_back({{endPoint2.x, endPoint2.y, endPoint2.z}, {coneColor.x, coneColor.y, coneColor.z}});
        }

        // Direction line (center axis)
        Vec3 endCenter = pos + dir * range;
        debugVerts.push_back({{pos.x, pos.y, pos.z}, {1.0f, 0.0f, 0.0f}});  // Red for center
        debugVerts.push_back({{endCenter.x, endCenter.y, endCenter.z}, {1.0f, 0.0f, 0.0f}});
    }

    if (debugVerts.empty())
        return true;

    renderer->debugVertexCount = (uint32_t)debugVerts.size();
    UINT bufferSize = (UINT)(debugVerts.size() * sizeof(DebugVertex));

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = bufferSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(renderer->device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&renderer->debugVertexBuffer))))
    {
        return false;
    }

    void* mappedData;
    renderer->debugVertexBuffer->Map(0, nullptr, &mappedData);
    memcpy(mappedData, debugVerts.data(), bufferSize);
    renderer->debugVertexBuffer->Unmap(0, nullptr);

    renderer->debugVertexBufferView.BufferLocation = renderer->debugVertexBuffer->GetGPUVirtualAddress();
    renderer->debugVertexBufferView.SizeInBytes = bufferSize;
    renderer->debugVertexBufferView.StrideInBytes = sizeof(DebugVertex);

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
        // Main camera constant buffer
        if (FAILED(renderer->device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&renderer->constantBuffer[i]))))
        {
            return false;
        }
        renderer->constantBuffer[i]->Map(0, nullptr, (void**)&renderer->constantBufferMapped[i]);

        // Shadow/top-down camera constant buffer
        if (FAILED(renderer->device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&renderer->shadowConstantBuffer[i]))))
        {
            return false;
        }
        renderer->shadowConstantBuffer[i]->Map(0, nullptr, (void**)&renderer->shadowConstantBufferMapped[i]);
    }

    // Create cone lights buffer
    const UINT lightsBufferSize = MAX_CONE_LIGHTS * sizeof(ConeLightGPU);
    bufferDesc.Width = lightsBufferSize;

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(renderer->device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&renderer->coneLightsBuffer[i]))))
        {
            return false;
        }

        renderer->coneLightsBuffer[i]->Map(0, nullptr, (void**)&renderer->coneLightsMapped[i]);
    }

    // Create per-light view-projection matrix buffer
    const UINT matricesBufferSize = MAX_CONE_LIGHTS * sizeof(Mat4);
    bufferDesc.Width = matricesBufferSize;

    for (UINT i = 0; i < FRAME_COUNT; ++i)
    {
        if (FAILED(renderer->device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&renderer->coneLightMatricesBuffer[i]))))
        {
            return false;
        }

        renderer->coneLightMatricesBuffer[i]->Map(0, nullptr, (void**)&renderer->coneLightMatricesMapped[i]);
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

    // DSV heap (2 descriptors: main depth buffer + shadow map)
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 2;
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

    // Create shadow depth buffer (1024x1024)
    if (!CreateShadowDepthBuffer(renderer))
    {
        OutputDebugStringA("Failed to create shadow depth buffer\n");
        return false;
    }

    // Create cone light shadow maps (256x256 x 128)
    if (!CreateConeShadowMaps(renderer))
    {
        OutputDebugStringA("Failed to create cone shadow maps\n");
        return false;
    }

    // Create horizon mapping resources
    if (!CreateHorizonMappingResources(renderer))
    {
        OutputDebugStringA("Failed to create horizon mapping resources\n");
        return false;
    }

    // Create fullscreen pipeline for depth visualization (after shadow buffer)
    if (!CreateFullscreenPipeline(renderer))
    {
        OutputDebugStringA("Failed to create fullscreen pipeline\n");
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

    // Create debug geometry (after main geometry creates cone lights)
    if (!CreateDebugGeometry(renderer))
    {
        OutputDebugStringA("Failed to create debug geometry\n");
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
        if (renderer->shadowConstantBuffer[i])
            renderer->shadowConstantBuffer[i]->Unmap(0, nullptr);
        if (renderer->coneLightsBuffer[i])
            renderer->coneLightsBuffer[i]->Unmap(0, nullptr);
        if (renderer->coneLightMatricesBuffer[i])
            renderer->coneLightMatricesBuffer[i]->Unmap(0, nullptr);
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

// Update a single oriented box's vertices in place
static void UpdateOrientedBoxVertices(Vertex* verts, const Vec3& center, const Vec3& forward,
                                       float sx, float sy, float sz)
{
    // Build orientation basis
    Vec3 fwd = forward.normalized();
    Vec3 up(0, 1, 0);
    Vec3 right = cross(up, fwd).normalized();  // Changed order for correct handedness

    // Box half-sizes: X=width, Y=height, Z=length (forward)
    float hx = sx * 0.5f;
    float hy = sy * 0.5f;
    float hz = sz * 0.5f;

    // Helper to transform local position to world
    auto toWorld = [&](float lx, float ly, float lz) -> Vec3 {
        return center + right * lx + up * ly + fwd * lz;
    };

    // Helper to transform local normal to world
    auto normalToWorld = [&](float nx, float ny, float nz) -> Vec3 {
        return (right * nx + up * ny + fwd * nz).normalized();
    };

    int v = 0;

    // Front face (forward +Z local = +fwd world)
    Vec3 nFront = normalToWorld(0, 0, 1);
    Vec3 p0 = toWorld(-hx, -hy, hz); verts[v++] = {{p0.x, p0.y, p0.z}, {nFront.x, nFront.y, nFront.z}, {0,0}};
    Vec3 p1 = toWorld( hx, -hy, hz); verts[v++] = {{p1.x, p1.y, p1.z}, {nFront.x, nFront.y, nFront.z}, {1,0}};
    Vec3 p2 = toWorld( hx,  hy, hz); verts[v++] = {{p2.x, p2.y, p2.z}, {nFront.x, nFront.y, nFront.z}, {1,1}};
    Vec3 p3 = toWorld(-hx,  hy, hz); verts[v++] = {{p3.x, p3.y, p3.z}, {nFront.x, nFront.y, nFront.z}, {0,1}};

    // Back face (-Z local = -fwd world)
    Vec3 nBack = normalToWorld(0, 0, -1);
    Vec3 p4 = toWorld( hx, -hy, -hz); verts[v++] = {{p4.x, p4.y, p4.z}, {nBack.x, nBack.y, nBack.z}, {0,0}};
    Vec3 p5 = toWorld(-hx, -hy, -hz); verts[v++] = {{p5.x, p5.y, p5.z}, {nBack.x, nBack.y, nBack.z}, {1,0}};
    Vec3 p6 = toWorld(-hx,  hy, -hz); verts[v++] = {{p6.x, p6.y, p6.z}, {nBack.x, nBack.y, nBack.z}, {1,1}};
    Vec3 p7 = toWorld( hx,  hy, -hz); verts[v++] = {{p7.x, p7.y, p7.z}, {nBack.x, nBack.y, nBack.z}, {0,1}};

    // Right face (+X local = +right world)
    Vec3 nRight = normalToWorld(1, 0, 0);
    Vec3 p8  = toWorld(hx, -hy,  hz); verts[v++] = {{p8.x,  p8.y,  p8.z},  {nRight.x, nRight.y, nRight.z}, {0,0}};
    Vec3 p9  = toWorld(hx, -hy, -hz); verts[v++] = {{p9.x,  p9.y,  p9.z},  {nRight.x, nRight.y, nRight.z}, {1,0}};
    Vec3 p10 = toWorld(hx,  hy, -hz); verts[v++] = {{p10.x, p10.y, p10.z}, {nRight.x, nRight.y, nRight.z}, {1,1}};
    Vec3 p11 = toWorld(hx,  hy,  hz); verts[v++] = {{p11.x, p11.y, p11.z}, {nRight.x, nRight.y, nRight.z}, {0,1}};

    // Left face (-X local = -right world)
    Vec3 nLeft = normalToWorld(-1, 0, 0);
    Vec3 p12 = toWorld(-hx, -hy, -hz); verts[v++] = {{p12.x, p12.y, p12.z}, {nLeft.x, nLeft.y, nLeft.z}, {0,0}};
    Vec3 p13 = toWorld(-hx, -hy,  hz); verts[v++] = {{p13.x, p13.y, p13.z}, {nLeft.x, nLeft.y, nLeft.z}, {1,0}};
    Vec3 p14 = toWorld(-hx,  hy,  hz); verts[v++] = {{p14.x, p14.y, p14.z}, {nLeft.x, nLeft.y, nLeft.z}, {1,1}};
    Vec3 p15 = toWorld(-hx,  hy, -hz); verts[v++] = {{p15.x, p15.y, p15.z}, {nLeft.x, nLeft.y, nLeft.z}, {0,1}};

    // Top face (+Y local = +up world)
    Vec3 nTop = normalToWorld(0, 1, 0);
    Vec3 p16 = toWorld(-hx, hy,  hz); verts[v++] = {{p16.x, p16.y, p16.z}, {nTop.x, nTop.y, nTop.z}, {0,0}};
    Vec3 p17 = toWorld( hx, hy,  hz); verts[v++] = {{p17.x, p17.y, p17.z}, {nTop.x, nTop.y, nTop.z}, {1,0}};
    Vec3 p18 = toWorld( hx, hy, -hz); verts[v++] = {{p18.x, p18.y, p18.z}, {nTop.x, nTop.y, nTop.z}, {1,1}};
    Vec3 p19 = toWorld(-hx, hy, -hz); verts[v++] = {{p19.x, p19.y, p19.z}, {nTop.x, nTop.y, nTop.z}, {0,1}};

    // Bottom face (-Y local = -up world)
    Vec3 nBottom = normalToWorld(0, -1, 0);
    Vec3 p20 = toWorld(-hx, -hy, -hz); verts[v++] = {{p20.x, p20.y, p20.z}, {nBottom.x, nBottom.y, nBottom.z}, {0,0}};
    Vec3 p21 = toWorld( hx, -hy, -hz); verts[v++] = {{p21.x, p21.y, p21.z}, {nBottom.x, nBottom.y, nBottom.z}, {1,0}};
    Vec3 p22 = toWorld( hx, -hy,  hz); verts[v++] = {{p22.x, p22.y, p22.z}, {nBottom.x, nBottom.y, nBottom.z}, {1,1}};
    Vec3 p23 = toWorld(-hx, -hy,  hz); verts[v++] = {{p23.x, p23.y, p23.z}, {nBottom.x, nBottom.y, nBottom.z}, {0,1}};
}

// Number of vertices per oriented box (6 faces * 4 vertices)
static constexpr int VERTS_PER_BOX = 24;

void D3D12_Update(D3D12Renderer* renderer, float deltaTime)
{
    // Car dimensions
    const float carLength = 4.0f;
    const float carWidth = 2.0f;
    const float carHeight = 1.5f;
    const float headlightHeight = 0.6f;
    const float headlightSpacing = 0.7f;

    // Track parameters
    float straightLength = renderer->trackStraightLength;
    float radius = renderer->trackRadius;
    float trackLength = renderer->trackLength;

    // Calculate spacing parameters
    // At spacing=1: cars evenly spread (maxSpacing)
    // At spacing=0: cars close together (minGap = 0.5m between cars)
    const float minGap = 0.5f;
    const int carsPerLane = (int)renderer->numCars / 2;
    float maxSpacingMeters = trackLength / (float)carsPerLane;  // Max distance between cars in each lane
    float minSpacingMeters = carLength + minGap;  // Minimum: car length + 0.5m gap
    float currentSpacingMeters = minSpacingMeters + (maxSpacingMeters - minSpacingMeters) * renderer->carSpacing;
    float spacingFraction = currentSpacingMeters / trackLength;  // As fraction of track

    // Move all cars forward
    float progressDelta = (renderer->carSpeed * deltaTime) / trackLength;

    for (uint32_t i = 0; i < renderer->numCars; i++)
    {
        // Update base progress (car 0 in each lane)
        renderer->carTrackProgress[i] += progressDelta;
        if (renderer->carTrackProgress[i] >= 1.0f)
            renderer->carTrackProgress[i] -= 1.0f;

        // Calculate actual position with spacing applied
        int lane = i % 2;
        int posInLane = i / 2;
        float baseProgress = renderer->carTrackProgress[lane];  // Use lane leader's progress
        float progress = baseProgress + posInLane * spacingFraction;
        if (progress >= 1.0f) progress -= 1.0f;

        float laneOffset = renderer->carLane[i];

        // Get position and direction on track
        Vec3 trackPos, trackDir;
        GetTrackPositionAndDirection(progress, straightLength, radius, trackPos, trackDir);

        // Calculate car position with lane offset
        Vec3 trackRight(trackDir.z, 0, -trackDir.x);
        Vec3 carPos = trackPos + trackRight * laneOffset;
        carPos.y = carHeight * 0.5f;

        // Update car box vertices
        Vertex* carVerts = renderer->carVerticesMapped + (i * VERTS_PER_BOX);
        UpdateOrientedBoxVertices(carVerts, carPos, trackDir, carWidth, carHeight, carLength);

        // Update headlight positions and directions (2 lights per car)
        uint32_t lightIndex = i * 2;
        if (lightIndex + 1 < renderer->numConeLights)
        {
            float frontOffset = carLength * 0.5f;
            Vec3 frontPos = carPos + trackDir * frontOffset;
            frontPos.y = headlightHeight;

            // Left headlight
            Vec3 leftOffset = trackRight * (-headlightSpacing);
            renderer->coneLights[lightIndex].position = frontPos + leftOffset;
            renderer->coneLights[lightIndex].direction = trackDir;

            // Right headlight
            Vec3 rightOffset = trackRight * headlightSpacing;
            renderer->coneLights[lightIndex + 1].position = frontPos + rightOffset;
            renderer->coneLights[lightIndex + 1].direction = trackDir;
        }
    }

    // Update debug visualization if enabled
    if (renderer->showDebugLights)
    {
        CreateDebugGeometry(renderer);
    }
}

void D3D12_Render(D3D12Renderer* renderer)
{
    float aspect = (float)renderer->width / (float)renderer->height;

    // Use activeLightCount for rendering (debug slider)
    uint32_t lightCount = (uint32_t)renderer->activeLightCount;
    if (lightCount > renderer->numConeLights) lightCount = renderer->numConeLights;

    // Update main camera constant buffer
    CameraConstants* cb = renderer->constantBufferMapped[renderer->frameIndex];
    cb->viewProjection = renderer->camera.getViewProjectionMatrix(aspect);
    cb->cameraPos = renderer->camera.position;
    cb->numConeLights = (float)lightCount;
    cb->ambientIntensity = renderer->ambientIntensity;
    cb->coneLightIntensity = renderer->coneLightIntensity;
    cb->shadowBias = renderer->shadowBias;
    cb->falloffExponent = renderer->headlightFalloff;
    cb->debugLightOverlap = renderer->showLightOverlap ? 1.0f : 0.0f;
    cb->overlapMaxCount = renderer->overlapMaxCount;
    cb->disableShadows = renderer->disableShadows ? 1.0f : 0.0f;
    cb->useHorizonMapping = renderer->useHorizonMapping ? 1.0f : 0.0f;
    cb->horizonWorldMinX = renderer->horizonWorldMin.x;
    cb->horizonWorldMinZ = renderer->horizonWorldMin.z;
    cb->horizonWorldSize = renderer->horizonWorldSize;

    // Update shadow constant buffer with top-down view
    CameraConstants* shadowCb = renderer->shadowConstantBufferMapped[renderer->frameIndex];
    shadowCb->viewProjection = renderer->topDownViewProj;
    shadowCb->cameraPos = renderer->camera.position;
    shadowCb->numConeLights = (float)lightCount;
    shadowCb->ambientIntensity = renderer->ambientIntensity;
    shadowCb->coneLightIntensity = renderer->coneLightIntensity;
    shadowCb->shadowBias = renderer->shadowBias;
    shadowCb->falloffExponent = renderer->headlightFalloff;
    shadowCb->debugLightOverlap = 0.0f;  // Never in debug mode for shadow pass

    // Update cone lights buffer (use slider-controlled range)
    float currentRange = renderer->headlightRange;
    ConeLightGPU* lightsGPU = renderer->coneLightsMapped[renderer->frameIndex];
    for (uint32_t i = 0; i < renderer->numConeLights; ++i)
    {
        const ConeLight& light = renderer->coneLights[i];
        lightsGPU[i].position[0] = light.position.x;
        lightsGPU[i].position[1] = light.position.y;
        lightsGPU[i].position[2] = light.position.z;
        lightsGPU[i].position[3] = currentRange;  // Use slider value
        lightsGPU[i].direction[0] = light.direction.x;
        lightsGPU[i].direction[1] = light.direction.y;
        lightsGPU[i].direction[2] = light.direction.z;
        lightsGPU[i].direction[3] = cosf(light.outerAngle);
        lightsGPU[i].color[0] = light.color.x;
        lightsGPU[i].color[1] = light.color.y;
        lightsGPU[i].color[2] = light.color.z;
        lightsGPU[i].color[3] = cosf(light.innerAngle);
    }

    // Calculate and update per-light view-projection matrices
    Mat4* lightMatrices = renderer->coneLightMatricesMapped[renderer->frameIndex];
    for (uint32_t i = 0; i < renderer->numConeLights; ++i)
    {
        const ConeLight& light = renderer->coneLights[i];

        // View matrix: look from light position along light direction
        Vec3 target = light.position + light.direction * currentRange;
        Vec3 up = (fabsf(light.direction.y) < 0.99f) ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        Mat4 view = Mat4::lookAt(light.position, target, up);

        // Perspective projection using outer cone angle
        float fov = light.outerAngle * 2.0f;  // Full cone angle
        Mat4 proj = Mat4::perspective(fov, 1.0f, 0.1f, currentRange);

        renderer->coneLightViewProj[i] = proj * view;
        lightMatrices[i] = renderer->coneLightViewProj[i];
    }

    // Reset command allocator and command list
    renderer->commandAllocators[renderer->frameIndex]->Reset();
    renderer->commandList->Reset(renderer->commandAllocators[renderer->frameIndex].Get(), renderer->shadowPipelineState.Get());

    // Set root signature
    renderer->commandList->SetGraphicsRootSignature(renderer->rootSignature.Get());

    // ========== Shadow Pass (top-down depth-only) ==========
    // Set top-down view-projection as root constants
    renderer->commandList->SetGraphicsRoot32BitConstants(4, 16, renderer->topDownViewProj.m, 0);

    // Get shadow DSV handle (second slot in DSV heap)
    UINT dsvDescriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle = renderer->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    shadowDsvHandle.ptr += dsvDescriptorSize;

    // Clear shadow depth buffer
    renderer->commandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // Set render target (depth only, no color target)
    renderer->commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsvHandle);

    // Set viewport and scissor for shadow map
    D3D12_VIEWPORT shadowViewport = {};
    shadowViewport.Width = (float)D3D12Renderer::SHADOW_MAP_SIZE;
    shadowViewport.Height = (float)D3D12Renderer::SHADOW_MAP_SIZE;
    shadowViewport.MaxDepth = 1.0f;
    renderer->commandList->RSSetViewports(1, &shadowViewport);

    D3D12_RECT shadowScissorRect = { 0, 0, (LONG)D3D12Renderer::SHADOW_MAP_SIZE, (LONG)D3D12Renderer::SHADOW_MAP_SIZE };
    renderer->commandList->RSSetScissorRects(1, &shadowScissorRect);

    // Draw scene to shadow map
    renderer->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer->commandList->IASetVertexBuffers(0, 1, &renderer->vertexBufferView);
    renderer->commandList->IASetIndexBuffer(&renderer->indexBufferView);
    renderer->commandList->DrawIndexedInstanced(renderer->indexCount, 1, 0, 0, 0);

    // ========== Cone Light Shadow Maps Pass ==========
    // Render shadow map for each active cone light
    UINT coneDsvDescriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_VIEWPORT coneShadowViewport = {};
    coneShadowViewport.Width = (float)D3D12Renderer::CONE_SHADOW_MAP_SIZE;
    coneShadowViewport.Height = (float)D3D12Renderer::CONE_SHADOW_MAP_SIZE;
    coneShadowViewport.MaxDepth = 1.0f;

    D3D12_RECT coneShadowScissor = { 0, 0, (LONG)D3D12Renderer::CONE_SHADOW_MAP_SIZE, (LONG)D3D12Renderer::CONE_SHADOW_MAP_SIZE };

    for (uint32_t i = 0; i < lightCount; ++i)
    {
        // Get DSV for this array slice
        D3D12_CPU_DESCRIPTOR_HANDLE coneDsvHandle = renderer->coneShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
        coneDsvHandle.ptr += i * coneDsvDescriptorSize;

        // Clear this slice
        renderer->commandList->ClearDepthStencilView(coneDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // Set render target (depth only)
        renderer->commandList->OMSetRenderTargets(0, nullptr, FALSE, &coneDsvHandle);

        // Set viewport and scissor
        renderer->commandList->RSSetViewports(1, &coneShadowViewport);
        renderer->commandList->RSSetScissorRects(1, &coneShadowScissor);

        // Set view-projection matrix as root constants (16 floats at root parameter 4)
        renderer->commandList->SetGraphicsRoot32BitConstants(4, 16, renderer->coneLightViewProj[i].m, 0);

        // Draw scene (skip ground plane - first 6 indices - only render cars as shadow casters)
        renderer->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        renderer->commandList->IASetVertexBuffers(0, 1, &renderer->vertexBufferView);
        renderer->commandList->IASetIndexBuffer(&renderer->indexBufferView);
        // Skip first 6 indices (ground plane), render only cars
        uint32_t carIndexCount = renderer->indexCount - 6;
        renderer->commandList->DrawIndexedInstanced(carIndexCount, 1, 6, 0, 0);
    }

    // ========== Horizon Mapping Compute Pass ==========
    if (renderer->useHorizonMapping)
    {
        // Transition shadow depth buffer to copy source
        D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
        copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[0].Transition.pResource = renderer->shadowDepthBuffer.Get();
        copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[1].Transition.pResource = renderer->horizonHeightMap.Get();
        copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        renderer->commandList->ResourceBarrier(2, copyBarriers);

        // Copy shadow depth buffer to height map texture
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
        srcLoc.pResource = renderer->shadowDepthBuffer.Get();
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
        dstLoc.pResource = renderer->horizonHeightMap.Get();
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = 0;

        renderer->commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

        // Transition height map to SRV and shadow depth back to depth write
        copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        renderer->commandList->ResourceBarrier(2, copyBarriers);

        // Set compute pipeline
        renderer->commandList->SetComputeRootSignature(renderer->horizonComputeRootSig.Get());
        renderer->commandList->SetPipelineState(renderer->horizonComputePSO.Get());

        // Set descriptor heaps
        ID3D12DescriptorHeap* horizonHeaps[] = { renderer->horizonSrvUavHeap.Get() };
        renderer->commandList->SetDescriptorHeaps(1, horizonHeaps);

        // Set height map SRV (descriptor 0)
        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = renderer->horizonSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
        renderer->commandList->SetComputeRootDescriptorTable(1, srvHandle);

        // Set horizon maps UAV (descriptor 1)
        UINT descriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = srvHandle;
        uavHandle.ptr += descriptorSize;
        renderer->commandList->SetComputeRootDescriptorTable(2, uavHandle);

        // Dispatch compute for each light
        UINT dispatchX = (D3D12Renderer::HORIZON_MAP_SIZE + 15) / 16;
        UINT dispatchY = (D3D12Renderer::HORIZON_MAP_SIZE + 15) / 16;

        struct HorizonParams {
            float lightPosX, lightPosY, lightPosZ;
            float worldSize;
            float worldMinX, worldMinY, worldMinZ;
            uint32_t lightIndex;
            uint32_t mapSize;
            float nearPlaneY;    // World Y at depth=0
            float farPlaneY;     // World Y at depth=1
            float padding;
        };

        // Calculate world Y values at depth buffer extremes
        // Top-down camera is at viewHeight looking down with near=0.1, far=viewHeight+10
        float viewHeight = renderer->carAABB.max.y + 50.0f;
        float nearZ = 0.1f;
        float farZ = viewHeight + 10.0f;
        float nearPlaneY = viewHeight - nearZ;   // World Y at depth=0 (near plane)
        float farPlaneY = viewHeight - farZ;     // World Y at depth=1 (far plane) = -10

        for (uint32_t i = 0; i < lightCount; ++i)
        {
            const ConeLight& light = renderer->coneLights[i];

            HorizonParams params = {};
            params.lightPosX = light.position.x;
            params.lightPosY = light.position.y;
            params.lightPosZ = light.position.z;
            params.worldSize = renderer->horizonWorldSize;
            params.worldMinX = renderer->horizonWorldMin.x;
            params.worldMinY = renderer->horizonWorldMin.y;
            params.worldMinZ = renderer->horizonWorldMin.z;
            params.lightIndex = i;
            params.mapSize = D3D12Renderer::HORIZON_MAP_SIZE;
            params.nearPlaneY = nearPlaneY;
            params.farPlaneY = farPlaneY;

            renderer->commandList->SetComputeRoot32BitConstants(0, 12, &params, 0);
            renderer->commandList->Dispatch(dispatchX, dispatchY, 1);
        }

        // Transition horizon maps from UAV to SRV for pixel shader
        D3D12_RESOURCE_BARRIER horizonBarrier = {};
        horizonBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        horizonBarrier.Transition.pResource = renderer->horizonMaps.Get();
        horizonBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        horizonBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        horizonBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        renderer->commandList->ResourceBarrier(1, &horizonBarrier);
    }

    // ========== Main Render Pass ==========
    // Transition cone shadow maps from depth write to shader resource
    D3D12_RESOURCE_BARRIER coneShadowBarrier = {};
    coneShadowBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    coneShadowBarrier.Transition.pResource = renderer->coneShadowMaps.Get();
    coneShadowBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    coneShadowBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    coneShadowBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    renderer->commandList->ResourceBarrier(1, &coneShadowBarrier);

    renderer->commandList->SetPipelineState(renderer->pipelineState.Get());

    // Set descriptor heap for shadow map SRV
    ID3D12DescriptorHeap* shadowHeaps[] = { renderer->coneShadowSrvHeap.Get() };
    renderer->commandList->SetDescriptorHeaps(1, shadowHeaps);

    // Use main constant buffer with main camera view-projection
    renderer->commandList->SetGraphicsRootConstantBufferView(0, renderer->constantBuffer[renderer->frameIndex]->GetGPUVirtualAddress());
    renderer->commandList->SetGraphicsRootShaderResourceView(1, renderer->coneLightsBuffer[renderer->frameIndex]->GetGPUVirtualAddress());
    renderer->commandList->SetGraphicsRootShaderResourceView(2, renderer->coneLightMatricesBuffer[renderer->frameIndex]->GetGPUVirtualAddress());
    renderer->commandList->SetGraphicsRootDescriptorTable(3, renderer->coneShadowSrvHeap->GetGPUDescriptorHandleForHeapStart());

    // Bind horizon maps (descriptor 1 in the same heap)
    UINT srvDescriptorSize = renderer->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE horizonSrvHandle = renderer->coneShadowSrvHeap->GetGPUDescriptorHandleForHeapStart();
    horizonSrvHandle.ptr += srvDescriptorSize;
    renderer->commandList->SetGraphicsRootDescriptorTable(5, horizonSrvHandle);

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

    if (renderer->showShadowMapDebug)
    {
        // Cone shadow maps are already transitioned to shader resource state above
        // Draw fullscreen quad with depth visualization
        renderer->commandList->SetPipelineState(renderer->fullscreenPipelineState.Get());
        renderer->commandList->SetGraphicsRootSignature(renderer->fullscreenRootSignature.Get());

        // Set slice index as root constant
        int sliceData[4] = { renderer->debugShadowMapIndex, 0, 0, 0 };
        renderer->commandList->SetGraphicsRoot32BitConstants(0, 4, sliceData, 0);

        // Use cone shadow maps (Texture2DArray)
        ID3D12DescriptorHeap* heaps[] = { renderer->coneShadowSrvHeap.Get() };
        renderer->commandList->SetDescriptorHeaps(1, heaps);
        renderer->commandList->SetGraphicsRootDescriptorTable(1, renderer->coneShadowSrvHeap->GetGPUDescriptorHandleForHeapStart());

        renderer->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        renderer->commandList->DrawInstanced(3, 1, 0, 0);
    }
    else
    {
        // Draw scene
        renderer->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        renderer->commandList->IASetVertexBuffers(0, 1, &renderer->vertexBufferView);
        renderer->commandList->IASetIndexBuffer(&renderer->indexBufferView);
        renderer->commandList->DrawIndexedInstanced(renderer->indexCount, 1, 0, 0, 0);

        // Draw debug cone wireframes if enabled
        if (renderer->showDebugLights && renderer->debugVertexCount > 0)
        {
            renderer->commandList->SetPipelineState(renderer->debugPipelineState.Get());
            renderer->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            renderer->commandList->IASetVertexBuffers(0, 1, &renderer->debugVertexBufferView);
            renderer->commandList->DrawInstanced(renderer->debugVertexCount, 1, 0, 0);
        }
    }

    // Render ImGui
    ID3D12DescriptorHeap* descriptorHeaps[] = { renderer->imguiSrvHeap.Get() };
    renderer->commandList->SetDescriptorHeaps(1, descriptorHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), renderer->commandList.Get());

    // Transition cone shadow maps back to depth write for next frame
    coneShadowBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    coneShadowBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    renderer->commandList->ResourceBarrier(1, &coneShadowBarrier);

    // Transition horizon maps back to UAV for next frame (if horizon mapping was used)
    if (renderer->useHorizonMapping)
    {
        D3D12_RESOURCE_BARRIER horizonBarrier = {};
        horizonBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        horizonBarrier.Transition.pResource = renderer->horizonMaps.Get();
        horizonBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        horizonBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        horizonBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        renderer->commandList->ResourceBarrier(1, &horizonBarrier);
    }

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

bool D3D12_CaptureBackbuffer(D3D12Renderer* renderer, uint8_t** outPixels, uint32_t* outWidth, uint32_t* outHeight)
{
    D3D12_WaitForGpu(renderer);

    ID3D12Resource* backBuffer = renderer->renderTargets[renderer->frameIndex].Get();

    D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    uint32_t width = (uint32_t)desc.Width;
    uint32_t height = (uint32_t)desc.Height;

    // Get the footprint for the readback buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 totalBytes = 0;
    renderer->device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

    // Create a readback buffer
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readbackBuffer;
    HRESULT hr = renderer->device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuffer));
    if (FAILED(hr))
        return false;

    // Reset command allocator and command list
    renderer->commandAllocators[renderer->frameIndex]->Reset();
    renderer->commandList->Reset(renderer->commandAllocators[renderer->frameIndex].Get(), nullptr);

    // Transition backbuffer to copy source
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    renderer->commandList->ResourceBarrier(1, &barrier);

    // Copy texture to buffer
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = backBuffer;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readbackBuffer.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = footprint;

    renderer->commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition backbuffer back to present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    renderer->commandList->ResourceBarrier(1, &barrier);

    // Execute and wait
    renderer->commandList->Close();
    ID3D12CommandList* cmdLists[] = { renderer->commandList.Get() };
    renderer->commandQueue->ExecuteCommandLists(1, cmdLists);

    D3D12_WaitForGpu(renderer);

    // Map and copy data
    uint8_t* mappedData = nullptr;
    hr = readbackBuffer->Map(0, nullptr, (void**)&mappedData);
    if (FAILED(hr))
        return false;

    // Allocate output buffer (BGRA format)
    uint8_t* pixels = new uint8_t[width * height * 4];

    // Copy row by row (handle row pitch)
    for (uint32_t y = 0; y < height; y++)
    {
        memcpy(pixels + y * width * 4, mappedData + y * footprint.Footprint.RowPitch, width * 4);
    }

    readbackBuffer->Unmap(0, nullptr);

    *outPixels = pixels;
    *outWidth = width;
    *outHeight = height;

    return true;
}
