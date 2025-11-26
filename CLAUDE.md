# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

Build using MSBuild (Visual Studio 2022):

```bash
# Debug build
"C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" cl3d.sln -p:Configuration=Debug -p:Platform=x64 -v:minimal

# Release build
"C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe" cl3d.sln -p:Configuration=Release -p:Platform=x64 -v:minimal
```

Output binaries go to `bin/Debug/` or `bin/Release/`. Intermediate files go to `obj/`.

## Project Overview

cl3d is a minimal D3D12 renderer with ImGui integration. It renders a 3D scene with a ground plane and box geometry using a first-person fly camera.

## Architecture

**Core Components:**

- `src/main.cpp` - Win32 window creation, message loop, input handling, camera controls
- `src/d3d12_renderer.cpp/.h` - D3D12 initialization, rendering, resource management
- `src/math_utils.h` - Vec3, Mat4, Camera structs with basic math operations
- `imgui/` - Dear ImGui library with Win32 and DX12 backends

**D3D12Renderer struct** (`d3d12_renderer.h:31`) contains all D3D12 state:
- Device, command queue, swap chain (2 frames in flight)
- Root signature with single CBV for camera constants
- Pipeline state with embedded HLSL shaders
- Vertex/index buffers for geometry, constant buffers per frame
- Depth buffer and ImGui descriptor heaps

**Shader** is embedded as a string in `d3d12_renderer.cpp:75`. It implements:
- View-projection transform
- Grid pattern on ground plane
- Simple directional lighting on boxes
- Distance fog

**Camera** uses yaw/pitch for orientation. WASD+QE for movement, mouse look when captured (click to capture, ESC to release).

## Key Patterns

- Uses ComPtr (WRL) for D3D12 object lifetime management
- Double-buffered with fence-based GPU synchronization
- Constant buffers are persistently mapped
- Geometry is generated procedurally (ground plane + 60 boxes arranged in rows)
