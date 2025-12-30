#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>

#include "math_utils.h"

using Microsoft::WRL::ComPtr;

static constexpr uint32_t FRAME_COUNT = 2;
static constexpr uint32_t MAX_CONE_LIGHTS = 128;
static constexpr uint32_t MAX_CARS = 60;

struct ConeLight
{
    Vec3 position;
    Vec3 direction;
    Vec3 color;
    float range;
    float innerAngle;
    float outerAngle;
};

struct ConeLightGPU
{
    float position[4];
    float direction[4];
    float color[4];
};

struct Vertex
{
    float position[3];
    float normal[3];
    float uv[2];
};

struct DebugVertex
{
    float position[3];
    float color[3];
};

struct CameraConstants
{
    Mat4 viewProjection;
    Vec3 cameraPos;
    float numConeLights;
    float ambientIntensity;
    float coneLightIntensity;
    float shadowBias;
    float falloffExponent;
    float debugLightOverlap;  // 1.0 = show light overlap visualization
    float overlapMaxCount;    // Max count for heat map coloring
    float disableShadows;     // 1.0 = skip shadow map sampling
    float useHorizonMapping;  // 1.0 = use horizon mapping instead of shadow maps
    float showGrid;           // 1.0 = show grid pattern on ground
    float horizonWorldMinX;   // Horizon map world space bounds
    float horizonWorldMinZ;
    float horizonWorldSize;
};

struct AABB
{
    Vec3 min;
    Vec3 max;
};

struct D3D12Renderer
{
    // Core D3D12 objects
    ComPtr<IDXGIFactory4>           factory;
    ComPtr<ID3D12Device>            device;
    ComPtr<ID3D12CommandQueue>      commandQueue;
    ComPtr<IDXGISwapChain3>         swapChain;
    ComPtr<ID3D12DescriptorHeap>    rtvHeap;
    ComPtr<ID3D12Resource>          renderTargets[FRAME_COUNT];
    ComPtr<ID3D12CommandAllocator>  commandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> commandList;

    // Pipeline objects
    ComPtr<ID3D12RootSignature>     rootSignature;
    ComPtr<ID3D12PipelineState>     pipelineState;

    // Geometry
    ComPtr<ID3D12Resource>          vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW        vertexBufferView;
    ComPtr<ID3D12Resource>          indexBuffer;
    D3D12_INDEX_BUFFER_VIEW         indexBufferView;
    uint32_t                        indexCount;

    // Car vertex data (for dynamic updates)
    Vertex*                         carVerticesMapped = nullptr;  // Points into mapped vertex buffer
    uint32_t                        carVertexStartIndex = 0;      // First car vertex in buffer
    uint32_t                        carVertexCount = 0;           // Total car vertices

    // Constant buffer (main camera)
    ComPtr<ID3D12Resource>          constantBuffer[FRAME_COUNT];
    CameraConstants*                constantBufferMapped[FRAME_COUNT];

    // Constant buffer (shadow/top-down camera)
    ComPtr<ID3D12Resource>          shadowConstantBuffer[FRAME_COUNT];
    CameraConstants*                shadowConstantBufferMapped[FRAME_COUNT];

    // Cone lights buffer
    ComPtr<ID3D12Resource>          coneLightsBuffer[FRAME_COUNT];
    ConeLightGPU*                   coneLightsMapped[FRAME_COUNT];
    ConeLight                       coneLights[MAX_CONE_LIGHTS];
    uint32_t                        numConeLights = 0;
    int                             activeLightCount = 0;  // For debug slider

    // Depth buffer
    ComPtr<ID3D12Resource>          depthBuffer;
    ComPtr<ID3D12DescriptorHeap>    dsvHeap;

    // ImGui
    ComPtr<ID3D12DescriptorHeap>    imguiSrvHeap;

    // Synchronization objects
    ComPtr<ID3D12Fence>             fence;
    HANDLE                          fenceEvent = nullptr;
    uint64_t                        fenceValues[FRAME_COUNT] = {};

    // Frame state
    uint32_t frameIndex = 0;
    uint32_t rtvDescriptorSize = 0;

    // Window dimensions
    uint32_t width = 0;
    uint32_t height = 0;

    // Camera
    Camera camera;

    // Debug visualization
    bool showDebugLights = false;
    bool showLightOverlap = false;  // Heat map of light cone overlaps
    float overlapMaxCount = 10.0f;  // Max count for heat map (maps to red)
    ComPtr<ID3D12PipelineState>     debugPipelineState;
    ComPtr<ID3D12Resource>          debugVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW        debugVertexBufferView;
    uint32_t                        debugVertexCount = 0;

    // Lighting controls
    float ambientIntensity = 0.3f;
    float coneLightIntensity = 1.0f;
    float shadowBias = 0.0f;
    float headlightRange = 30.0f;  // Range in meters (20-300)
    float headlightFalloff = 2.0f; // Distance falloff exponent (lower = less falloff)
    bool disableShadows = false;   // Skip shadow map sampling
    bool showGrid = true;          // Show grid pattern on ground

    // Car animation
    uint32_t numCars = 0;
    float carTrackProgress[MAX_CARS];  // 0-1 progress along the oval track
    float carLane[MAX_CARS];           // Lane offset (inner/outer)
    float carSpeed = 20.0f;            // Speed in meters per second
    float carSpacing = 1.0f;           // 0-1: 0=close (0.5m gap), 1=max spread

    // Track parameters
    float trackLength = 0.0f;          // Total track length in meters
    float trackStraightLength = 150.0f;
    float trackRadius = 50.0f;
    float trackLaneWidth = 3.0f;

    // Car AABB for top-down rendering
    AABB carAABB;
    Mat4 topDownViewProj;

    // Offscreen depth buffer for top-down view (1024x1024)
    static constexpr uint32_t SHADOW_MAP_SIZE = 1024;
    ComPtr<ID3D12Resource>          shadowDepthBuffer;
    ComPtr<ID3D12PipelineState>     shadowPipelineState;
    bool showShadowMapDebug = false;
    int debugShadowMapIndex = 0;  // Which cone shadow map slice to visualize

    // Fullscreen quad for depth visualization
    ComPtr<ID3D12RootSignature>     fullscreenRootSignature;
    ComPtr<ID3D12PipelineState>     fullscreenPipelineState;
    ComPtr<ID3D12DescriptorHeap>    shadowSrvHeap;

    // Cone light shadow maps (256x256 x MAX_CONE_LIGHTS)
    static constexpr uint32_t CONE_SHADOW_MAP_SIZE = 256;
    ComPtr<ID3D12Resource>          coneShadowMaps;            // Texture2DArray
    ComPtr<ID3D12DescriptorHeap>    coneShadowDsvHeap;         // DSV heap for all slices
    ComPtr<ID3D12DescriptorHeap>    coneShadowSrvHeap;         // SRV heap for shader access
    Mat4                            coneLightViewProj[MAX_CONE_LIGHTS];  // CPU-side matrices

    // Per-light view-projection matrices (uploaded to GPU)
    ComPtr<ID3D12Resource>          coneLightMatricesBuffer[FRAME_COUNT];
    Mat4*                           coneLightMatricesMapped[FRAME_COUNT];

    // Horizon Mapping shadow technique
    bool useHorizonMapping = false;
    static constexpr uint32_t HORIZON_MAP_SIZE = 1024;
    ComPtr<ID3D12Resource>          horizonHeightMap;          // R32_FLOAT top-down height map
    ComPtr<ID3D12Resource>          horizonMaps;               // Texture2DArray R32_FLOAT per-light horizon angles
    ComPtr<ID3D12DescriptorHeap>    horizonSrvUavHeap;         // SRV+UAV heap for compute
    ComPtr<ID3D12RootSignature>     horizonComputeRootSig;     // Root signature for horizon compute
    ComPtr<ID3D12PipelineState>     horizonComputePSO;         // Compute pipeline for horizon tracing
    ComPtr<ID3D12Resource>          horizonParamsBuffer;       // Per-light parameters for compute
    float                           horizonWorldSize = 0.0f;   // World space size covered by horizon map
    Vec3                            horizonWorldMin;           // World space min corner of horizon map
};

bool D3D12_Init(D3D12Renderer* renderer, HWND hwnd, uint32_t width, uint32_t height);
void D3D12_Shutdown(D3D12Renderer* renderer);
void D3D12_Update(D3D12Renderer* renderer, float deltaTime);
void D3D12_Render(D3D12Renderer* renderer);
void D3D12_WaitForGpu(D3D12Renderer* renderer);
void D3D12_Resize(D3D12Renderer* renderer, uint32_t width, uint32_t height);
bool D3D12_CaptureBackbuffer(D3D12Renderer* renderer, uint8_t** outPixels, uint32_t* outWidth, uint32_t* outHeight);
