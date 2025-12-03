#include "d3d12_renderer.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <cstdio>
#include <cmath>

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static D3D12Renderer g_Renderer = {};
static bool g_Running = true;
static HWND g_Hwnd = nullptr;

// Input state
static bool g_Keys[256] = {};
static bool g_MouseCaptured = false;
static POINT g_LastMousePos = {};

// Timing
static LARGE_INTEGER g_Frequency = {};
static LARGE_INTEGER g_LastTime = {};

// Frame time history for graph
static constexpr int FRAME_TIME_HISTORY_SIZE = 200;
static float g_FrameTimeHistory[FRAME_TIME_HISTORY_SIZE] = {};
static int g_FrameTimeIndex = 0;

static float GetDeltaTime()
{
    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);
    float deltaTime = (float)(currentTime.QuadPart - g_LastTime.QuadPart) / (float)g_Frequency.QuadPart;
    g_LastTime = currentTime;
    return deltaTime;
}

static void CaptureMouse(bool capture)
{
    g_MouseCaptured = capture;
    if (capture)
    {
        SetCapture(g_Hwnd);
        ShowCursor(FALSE);

        // Center cursor
        RECT rect;
        GetClientRect(g_Hwnd, &rect);
        POINT center = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
        ClientToScreen(g_Hwnd, &center);
        SetCursorPos(center.x, center.y);
        g_LastMousePos = center;
    }
    else
    {
        ReleaseCapture();
        ShowCursor(TRUE);
    }
}

static void UpdateCamera(float deltaTime)
{
    // Don't update camera if ImGui wants input
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard)
        return;

    Camera& cam = g_Renderer.camera;

    // Mouse look (only when captured)
    if (g_MouseCaptured)
    {
        POINT currentPos;
        GetCursorPos(&currentPos);

        float dx = (float)(currentPos.x - g_LastMousePos.x);
        float dy = (float)(currentPos.y - g_LastMousePos.y);

        cam.yaw += dx * cam.lookSpeed;
        cam.pitch -= dy * cam.lookSpeed;

        // Clamp pitch
        const float maxPitch = 1.5f; // ~85 degrees
        if (cam.pitch > maxPitch) cam.pitch = maxPitch;
        if (cam.pitch < -maxPitch) cam.pitch = -maxPitch;

        // Re-center cursor
        RECT rect;
        GetClientRect(g_Hwnd, &rect);
        POINT center = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
        ClientToScreen(g_Hwnd, &center);
        SetCursorPos(center.x, center.y);
        g_LastMousePos = center;
    }

    // Keyboard movement
    Vec3 moveDir(0, 0, 0);
    Vec3 forward = cam.getForward();
    Vec3 right = cam.getRight();

    if (g_Keys['W']) moveDir += forward;
    if (g_Keys['S']) moveDir += forward * -1.0f;
    if (g_Keys['A']) moveDir += right * -1.0f;
    if (g_Keys['D']) moveDir += right;
    if (g_Keys['E'] || g_Keys[VK_SPACE]) moveDir += Vec3(0, 1, 0);
    if (g_Keys['Q'] || g_Keys[VK_CONTROL]) moveDir += Vec3(0, -1, 0);

    // Normalize and apply speed
    float len = moveDir.length();
    if (len > 0.001f)
    {
        moveDir = moveDir * (1.0f / len);
        float speed = cam.moveSpeed;
        if (g_Keys[VK_SHIFT]) speed *= 3.0f; // Sprint
        cam.position += moveDir * speed * deltaTime;
    }
}

// HSV to RGB conversion (matching shader function)
static ImU32 HSVtoImColor(float h, float s, float v)
{
    float c = v * s;
    float hPrime = h * 6.0f;
    float x = c * (1.0f - fabsf(fmodf(hPrime, 2.0f) - 1.0f));
    float m = v - c;

    float r, g, b;
    if (hPrime < 1.0f) { r = c; g = x; b = 0.0f; }
    else if (hPrime < 2.0f) { r = x; g = c; b = 0.0f; }
    else if (hPrime < 3.0f) { r = 0.0f; g = c; b = x; }
    else if (hPrime < 4.0f) { r = 0.0f; g = x; b = c; }
    else if (hPrime < 5.0f) { r = x; g = 0.0f; b = c; }
    else { r = c; g = 0.0f; b = x; }

    return IM_COL32((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255), 255);
}

// Map intensity [0, 1] to heat color using hue [0, 0.9]
static ImU32 IntensityToHeatImColor(float intensity)
{
    float hue = intensity * 0.9f;
    return HSVtoImColor(hue, 1.0f, 1.0f);
}

static void DrawHeatMapLegend(float maxCount)
{
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    const float margin = 10.0f;
    const float width = 20.0f;
    const float legendHeight = displaySize.y - 2 * margin;
    const float x = displaySize.x - margin - width;

    // Draw gradient as vertical strips
    const int segments = 64;

    for (int i = 0; i < segments; i++)
    {
        float t0 = (float)i / segments;
        float t1 = (float)(i + 1) / segments;

        // Bottom to top: t=0 at bottom (red), t=1 at top (magenta)
        float y0 = displaySize.y - margin - t0 * legendHeight;
        float y1 = displaySize.y - margin - t1 * legendHeight;

        ImU32 color0 = IntensityToHeatImColor(t0);
        ImU32 color1 = IntensityToHeatImColor(t1);

        drawList->AddRectFilledMultiColor(
            ImVec2(x, y1), ImVec2(x + width, y0),
            color1, color1, color0, color0);
    }

    // Draw border
    drawList->AddRect(
        ImVec2(x, margin), ImVec2(x + width, displaySize.y - margin),
        IM_COL32(255, 255, 255, 200));

    // Draw labels at 0%, 25%, 50%, 75%, 100%
    char label[32];
    const float labelOffsetX = 30.0f;
    const float labelOffsetY = 6.0f;  // Center text vertically on tick

    // 100% (top)
    snprintf(label, sizeof(label), "%.0f", maxCount);
    drawList->AddText(ImVec2(x - labelOffsetX, margin - labelOffsetY), IM_COL32(255, 255, 255, 255), label);

    // 75%
    float y75 = displaySize.y - margin - 0.75f * legendHeight;
    snprintf(label, sizeof(label), "%.0f", maxCount * 0.75f);
    drawList->AddText(ImVec2(x - labelOffsetX, y75 - labelOffsetY), IM_COL32(255, 255, 255, 255), label);

    // 50%
    float y50 = displaySize.y - margin - 0.5f * legendHeight;
    snprintf(label, sizeof(label), "%.0f", maxCount * 0.5f);
    drawList->AddText(ImVec2(x - labelOffsetX, y50 - labelOffsetY), IM_COL32(255, 255, 255, 255), label);

    // 25%
    float y25 = displaySize.y - margin - 0.25f * legendHeight;
    snprintf(label, sizeof(label), "%.0f", maxCount * 0.25f);
    drawList->AddText(ImVec2(x - labelOffsetX, y25 - labelOffsetY), IM_COL32(255, 255, 255, 255), label);

    // 0% (bottom)
    drawList->AddText(ImVec2(x - labelOffsetX + 20, displaySize.y - margin - labelOffsetY), IM_COL32(255, 255, 255, 255), "0");
}

static void DrawImGui(float deltaTime)
{
    // Store frame time
    float frameTimeMs = deltaTime * 1000.0f;
    g_FrameTimeHistory[g_FrameTimeIndex] = frameTimeMs;
    g_FrameTimeIndex = (g_FrameTimeIndex + 1) % FRAME_TIME_HISTORY_SIZE;

    // Calculate stats
    float avgFrameTime = 0.0f;
    float maxFrameTime = 0.0f;
    float minFrameTime = FLT_MAX;
    for (int i = 0; i < FRAME_TIME_HISTORY_SIZE; i++)
    {
        avgFrameTime += g_FrameTimeHistory[i];
        if (g_FrameTimeHistory[i] > maxFrameTime) maxFrameTime = g_FrameTimeHistory[i];
        if (g_FrameTimeHistory[i] < minFrameTime && g_FrameTimeHistory[i] > 0) minFrameTime = g_FrameTimeHistory[i];
    }
    avgFrameTime /= FRAME_TIME_HISTORY_SIZE;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 150), ImGuiCond_FirstUseEver);

    ImGui::Begin("Frame Statistics");

    ImGui::Text("Frame Time: %.3f ms (%.1f FPS)", frameTimeMs, 1000.0f / frameTimeMs);
    ImGui::Text("Avg: %.3f ms | Min: %.3f ms | Max: %.3f ms", avgFrameTime, minFrameTime, maxFrameTime);

    // Plot frame times as a graph
    // Reorder the array so it displays correctly (oldest to newest)
    float plotData[FRAME_TIME_HISTORY_SIZE];
    for (int i = 0; i < FRAME_TIME_HISTORY_SIZE; i++)
    {
        plotData[i] = g_FrameTimeHistory[(g_FrameTimeIndex + i) % FRAME_TIME_HISTORY_SIZE];
    }

    ImGui::PlotLines("##FrameTime", plotData, FRAME_TIME_HISTORY_SIZE, 0, nullptr, 0.0f, maxFrameTime * 1.2f, ImVec2(0, 60));

    ImGui::Separator();
    ImGui::Text("Lighting");
    ImGui::SliderFloat("Ambient", &g_Renderer.ambientIntensity, 0.0f, 1.0f);
    ImGui::SliderFloat("Headlight Intensity", &g_Renderer.coneLightIntensity, 0.0f, 100.0f);
    ImGui::SliderFloat("Headlight Range", &g_Renderer.headlightRange, 20.0f, 300.0f);
    ImGui::SliderFloat("Headlight Falloff", &g_Renderer.headlightFalloff, 0.0f, 4.0f);
    ImGui::SliderFloat("Shadow Bias", &g_Renderer.shadowBias, -0.5f, 0.5f);
    ImGui::Checkbox("Disable Shadows", &g_Renderer.disableShadows);
    ImGui::Checkbox("Use Horizon Mapping", &g_Renderer.useHorizonMapping);

    ImGui::Separator();
    ImGui::Text("Animation");
    ImGui::SliderFloat("Car Speed (m/s)", &g_Renderer.carSpeed, 0.0f, 100.0f);
    ImGui::SliderFloat("Car Spacing", &g_Renderer.carSpacing, 0.0f, 1.0f);

    ImGui::Separator();
    ImGui::Checkbox("Show Headlight Debug", &g_Renderer.showDebugLights);
    ImGui::Checkbox("Show Light Overlap", &g_Renderer.showLightOverlap);
    if (g_Renderer.showLightOverlap)
    {
        ImGui::SliderFloat("Overlap Max", &g_Renderer.overlapMaxCount, 1.0f, 120.0f);
        DrawHeatMapLegend(g_Renderer.overlapMaxCount);
    }
    ImGui::Text("Cone Lights: %u", g_Renderer.numConeLights);
    if (g_Renderer.activeLightCount == 0)
        g_Renderer.activeLightCount = (int)g_Renderer.numConeLights;
    ImGui::SliderInt("Active Lights", &g_Renderer.activeLightCount, 0, (int)g_Renderer.numConeLights);

    ImGui::Separator();
    ImGui::Checkbox("Show Cone Shadow Map", &g_Renderer.showShadowMapDebug);
    if (g_Renderer.showShadowMapDebug)
    {
        ImGui::SliderInt("Shadow Map Index", &g_Renderer.debugShadowMapIndex, 0, (int)g_Renderer.numConeLights - 1);
    }

    ImGui::End();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Let ImGui handle messages first
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_Renderer.device && wParam != SIZE_MINIMIZED)
        {
            D3D12_Resize(&g_Renderer, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam < 256)
            g_Keys[wParam] = true;

        if (wParam == VK_ESCAPE)
        {
            if (g_MouseCaptured)
                CaptureMouse(false);
            else
                g_Running = false;
        }
        return 0;

    case WM_KEYUP:
        if (wParam < 256)
            g_Keys[wParam] = false;
        return 0;

    case WM_LBUTTONDOWN:
        // Only capture if not clicking on ImGui
        if (!g_MouseCaptured && !ImGui::GetIO().WantCaptureMouse)
            CaptureMouse(true);
        return 0;

    case WM_KILLFOCUS:
        // Release mouse when window loses focus
        if (g_MouseCaptured)
            CaptureMouse(false);
        memset(g_Keys, 0, sizeof(g_Keys));
        return 0;

    case WM_DESTROY:
        g_Running = false;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    // Initialize timing
    QueryPerformanceFrequency(&g_Frequency);
    QueryPerformanceCounter(&g_LastTime);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CL3DWindowClass";

    if (!RegisterClassExW(&wc))
    {
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Calculate window size for desired client area
    const uint32_t clientWidth = 1280;
    const uint32_t clientHeight = 720;

    RECT windowRect = { 0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight) };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;

    // Create window
    g_Hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"CL3D - D3D12 Renderer (Click to capture mouse, ESC to release/quit)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowWidth, windowHeight,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!g_Hwnd)
    {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize D3D12
    if (!D3D12_Init(&g_Renderer, g_Hwnd, clientWidth, clientHeight))
    {
        MessageBoxW(nullptr, L"Failed to initialize D3D12", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_Hwnd, nCmdShow);

    // Main loop
    while (g_Running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_Running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (g_Running)
        {
            float deltaTime = GetDeltaTime();
            UpdateCamera(deltaTime);

            // Update car animation
            D3D12_Update(&g_Renderer, deltaTime);

            // Start ImGui frame
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Draw ImGui UI
            DrawImGui(deltaTime);

            // Render ImGui
            ImGui::Render();

            // Render scene + ImGui
            D3D12_Render(&g_Renderer);
        }
    }

    // Cleanup
    D3D12_Shutdown(&g_Renderer);

    return 0;
}
