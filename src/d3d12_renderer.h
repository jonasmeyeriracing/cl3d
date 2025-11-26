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

struct Vertex
{
    float position[3];
    float normal[3];
    float uv[2];
};

struct CameraConstants
{
    Mat4 viewProjection;
    Vec3 cameraPos;
    float padding;
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

    // Constant buffer
    ComPtr<ID3D12Resource>          constantBuffer[FRAME_COUNT];
    CameraConstants*                constantBufferMapped[FRAME_COUNT];

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
};

bool D3D12_Init(D3D12Renderer* renderer, HWND hwnd, uint32_t width, uint32_t height);
void D3D12_Shutdown(D3D12Renderer* renderer);
void D3D12_Render(D3D12Renderer* renderer);
void D3D12_WaitForGpu(D3D12Renderer* renderer);
void D3D12_Resize(D3D12Renderer* renderer, uint32_t width, uint32_t height);
