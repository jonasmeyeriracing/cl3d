#include "d3d12_renderer.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <shellapi.h>

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

// Test mode
static bool g_TestMode = false;
static std::string g_TestConfigFile;
static std::string g_TestOutputFile;
static int g_TestFrameCount = 0;
static constexpr int TEST_FRAME_WAIT = 30;

// Generate reference mode
static bool g_GenerateRefMode = false;
static std::string g_GenerateRefConfigFile;

// Serialize all settings to a string
static std::string SerializeState(const D3D12Renderer& renderer)
{
    std::ostringstream ss;
    ss << std::setprecision(8);

    // Version for future compatibility
    ss << "version=1\n";

    // Camera position and orientation
    ss << "camera.position.x=" << renderer.camera.position.x << "\n";
    ss << "camera.position.y=" << renderer.camera.position.y << "\n";
    ss << "camera.position.z=" << renderer.camera.position.z << "\n";
    ss << "camera.yaw=" << renderer.camera.yaw << "\n";
    ss << "camera.pitch=" << renderer.camera.pitch << "\n";

    // Lighting settings
    ss << "ambientIntensity=" << renderer.ambientIntensity << "\n";
    ss << "coneLightIntensity=" << renderer.coneLightIntensity << "\n";
    ss << "headlightRange=" << renderer.headlightRange << "\n";
    ss << "headlightFalloff=" << renderer.headlightFalloff << "\n";
    ss << "shadowBias=" << renderer.shadowBias << "\n";
    ss << "disableShadows=" << (renderer.disableShadows ? 1 : 0) << "\n";
    ss << "useHorizonMapping=" << (renderer.useHorizonMapping ? 1 : 0) << "\n";
    ss << "showGrid=" << (renderer.showGrid ? 1 : 0) << "\n";

    // Animation settings
    ss << "carSpeed=" << renderer.carSpeed << "\n";
    ss << "carSpacing=" << renderer.carSpacing << "\n";

    // Debug settings
    ss << "showDebugLights=" << (renderer.showDebugLights ? 1 : 0) << "\n";
    ss << "showLightOverlap=" << (renderer.showLightOverlap ? 1 : 0) << "\n";
    ss << "overlapMaxCount=" << renderer.overlapMaxCount << "\n";
    ss << "activeLightCount=" << renderer.activeLightCount << "\n";
    ss << "showShadowMapDebug=" << (renderer.showShadowMapDebug ? 1 : 0) << "\n";
    ss << "debugShadowMapIndex=" << renderer.debugShadowMapIndex << "\n";

    // Simulation time (first car's track progress as reference)
    ss << "simulationTime=" << renderer.carTrackProgress[0] << "\n";

    return ss.str();
}

// Deserialize settings from a string
static bool DeserializeState(D3D12Renderer& renderer, const std::string& data)
{
    std::istringstream ss(data);
    std::string line;

    float simulationTime = -1.0f;
    float oldSimTime = renderer.carTrackProgress[0];

    while (std::getline(ss, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
            continue;

        std::string key = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);

        // Camera
        if (key == "camera.position.x") renderer.camera.position.x = std::stof(value);
        else if (key == "camera.position.y") renderer.camera.position.y = std::stof(value);
        else if (key == "camera.position.z") renderer.camera.position.z = std::stof(value);
        else if (key == "camera.yaw") renderer.camera.yaw = std::stof(value);
        else if (key == "camera.pitch") renderer.camera.pitch = std::stof(value);

        // Lighting
        else if (key == "ambientIntensity") renderer.ambientIntensity = std::stof(value);
        else if (key == "coneLightIntensity") renderer.coneLightIntensity = std::stof(value);
        else if (key == "headlightRange") renderer.headlightRange = std::stof(value);
        else if (key == "headlightFalloff") renderer.headlightFalloff = std::stof(value);
        else if (key == "shadowBias") renderer.shadowBias = std::stof(value);
        else if (key == "disableShadows") renderer.disableShadows = (std::stoi(value) != 0);
        else if (key == "useHorizonMapping") renderer.useHorizonMapping = (std::stoi(value) != 0);
        else if (key == "showGrid") renderer.showGrid = (std::stoi(value) != 0);

        // Animation
        else if (key == "carSpeed") renderer.carSpeed = std::stof(value);
        else if (key == "carSpacing") renderer.carSpacing = std::stof(value);

        // Debug
        else if (key == "showDebugLights") renderer.showDebugLights = (std::stoi(value) != 0);
        else if (key == "showLightOverlap") renderer.showLightOverlap = (std::stoi(value) != 0);
        else if (key == "overlapMaxCount") renderer.overlapMaxCount = std::stof(value);
        else if (key == "activeLightCount") renderer.activeLightCount = std::stoi(value);
        else if (key == "showShadowMapDebug") renderer.showShadowMapDebug = (std::stoi(value) != 0);
        else if (key == "debugShadowMapIndex") renderer.debugShadowMapIndex = std::stoi(value);

        // Simulation time
        else if (key == "simulationTime") simulationTime = std::stof(value);
    }

    // Apply simulation time delta to all cars
    if (simulationTime >= 0.0f)
    {
        float delta = simulationTime - oldSimTime;
        for (uint32_t i = 0; i < renderer.numCars; i++)
        {
            renderer.carTrackProgress[i] += delta;
            // Wrap to [0, 1)
            while (renderer.carTrackProgress[i] >= 1.0f)
                renderer.carTrackProgress[i] -= 1.0f;
            while (renderer.carTrackProgress[i] < 0.0f)
                renderer.carTrackProgress[i] += 1.0f;
        }
    }

    return true;
}

// Save state to a file
static bool SaveStateToFile(const D3D12Renderer& renderer, const char* filename)
{
    std::ofstream file(filename);
    if (!file.is_open())
        return false;
    file << SerializeState(renderer);
    return true;
}

// Load state from a file
static bool LoadStateFromFile(D3D12Renderer& renderer, const char* filename)
{
    std::ifstream file(filename);
    if (!file.is_open())
        return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return DeserializeState(renderer, buffer.str());
}

// Helper function to get position and direction on the oval track
// Must match the implementation in d3d12_renderer.cpp exactly!
static void GetTrackPositionAndDirection(float progress, float straightLength, float radius,
                                          Vec3& outPos, Vec3& outDir)
{
    const float PI = 3.14159265f;

    // Track layout (counterclockwise):
    // - Bottom straight: progress 0 to ~0.25 (going +X)
    // - Right semicircle: progress ~0.25 to ~0.5 (turning around)
    // - Top straight: progress ~0.5 to ~0.75 (going -X)
    // - Left semicircle: progress ~0.75 to 1.0 (turning around)

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

// Export scene to PBRT format for reference raytracer
static bool ExportToPBRT(const D3D12Renderer& renderer, const char* outputPath)
{
    std::ofstream file(outputPath);
    if (!file.is_open())
        return false;

    file << std::setprecision(6);
    file << "# PBRT scene exported from cl3d\n";
    file << "# Render with: pbrt scene.pbrt\n\n";

    // Film settings (match our window size)
    file << "Film \"rgb\"\n";
    file << "    \"integer xresolution\" [ 1280 ]\n";
    file << "    \"integer yresolution\" [ 720 ]\n";
    file << "    \"string filename\" \"render.exr\"\n\n";

    // Sampler for quality - higher samples = less noise
    file << "Sampler \"halton\" \"integer pixelsamples\" [ 256 ]\n\n";

    // Integrator - path tracing for realistic shadows
    file << "Integrator \"volpath\" \"integer maxdepth\" [ 5 ]\n\n";

    // Camera - negate X to convert from D3D12 left-handed to PBRT right-handed
    const Camera& cam = renderer.camera;
    Vec3 forward = cam.getForward();
    Vec3 lookAt = cam.position + forward;

    file << "LookAt " << -cam.position.x << " " << cam.position.y << " " << cam.position.z << "  # eye\n";
    file << "       " << -lookAt.x << " " << lookAt.y << " " << lookAt.z << "  # look at\n";
    file << "       0 1 0  # up\n\n";

    file << "Camera \"perspective\"\n";
    file << "    \"float fov\" [ 60 ]\n\n";

    // Begin world
    file << "WorldBegin\n\n";

    // Sky dome with cl3d fog color (0.5, 0.6, 0.7)
    // Use fog color directly - this is what appears at the horizon in cl3d
    file << "# Sky/ambient lighting\n";
    file << "LightSource \"infinite\"\n";
    file << "    \"rgb L\" [ 0.5 0.6 0.7 ]\n\n";

    // Ground plane material - darker to compensate for brighter sky light
    file << "# Ground plane\n";
    file << "AttributeBegin\n";
    float groundReflectance = 0.05f;  // Dark ground to match cl3d appearance
    file << "    Material \"diffuse\" \"rgb reflectance\" [ " << groundReflectance << " " << groundReflectance << " " << groundReflectance << " ]\n";
    file << "    Shape \"trianglemesh\"\n";
    file << "        \"point3 P\" [ -500 0 -500  500 0 -500  500 0 500  -500 0 500 ]\n";
    file << "        \"integer indices\" [ 0 1 2  0 2 3 ]\n";
    file << "AttributeEnd\n\n";

    // Car boxes - must match D3D12_Update calculation exactly
    file << "# Cars (boxes on oval track)\n";
    const float PI = 3.14159265f;
    const float carLength = 4.0f;
    const float carWidth = 2.0f;
    const float carHeight = 1.5f;
    const float straightLength = renderer.trackStraightLength;
    const float radius = renderer.trackRadius;
    const float trackLength = straightLength * 2.0f + 2.0f * PI * radius;

    // Calculate spacing (must match D3D12_Update)
    const int carsPerLane = renderer.numCars / 2;
    const float minGap = 0.5f;
    float maxSpacingMeters = trackLength / (float)carsPerLane;
    float minSpacingMeters = carLength + minGap;
    float currentSpacingMeters = minSpacingMeters + (maxSpacingMeters - minSpacingMeters) * renderer.carSpacing;
    float spacingFraction = currentSpacingMeters / trackLength;

    // Headlight parameters (must match D3D12_Update)
    const float headlightHeight = 0.6f;
    const float headlightSpacing = 0.4f;
    const float headlightInnerAngle = 15.0f * PI / 180.0f;
    const float headlightOuterAngle = 20.0f * PI / 180.0f;

    // Store car data for light calculation
    struct CarData { Vec3 pos; Vec3 dir; Vec3 right; };
    std::vector<CarData> carData(renderer.numCars);

    for (uint32_t i = 0; i < renderer.numCars; i++)
    {
        // Calculate actual progress with lane-based spacing (matches D3D12_Update)
        int lane = i % 2;
        int posInLane = i / 2;
        float baseProgress = renderer.carTrackProgress[lane];  // Lane leader's progress
        float progress = baseProgress + posInLane * spacingFraction;
        if (progress >= 1.0f) progress -= 1.0f;

        Vec3 trackPos, trackDir;
        GetTrackPositionAndDirection(progress, straightLength, radius, trackPos, trackDir);

        Vec3 trackRight(trackDir.z, 0, -trackDir.x);
        Vec3 carPos = trackPos + trackRight * renderer.carLane[i];
        carPos.y = carHeight * 0.5f;

        // Store for light calculation
        carData[i].pos = carPos;
        carData[i].dir = trackDir;
        carData[i].right = trackRight;

        file << "AttributeBegin\n";
        file << "    Material \"diffuse\" \"rgb reflectance\" [ 0.8 0.8 0.8 ]\n";

        // Transform: translate then rotate to align with track direction
        // Negate X for coordinate system conversion
        float angle = atan2f(-trackDir.x, trackDir.z) * 180.0f / PI;
        file << "    Translate " << -carPos.x << " " << carPos.y << " " << carPos.z << "\n";
        file << "    Rotate " << angle << " 0 1 0\n";
        file << "    Scale " << carWidth * 0.5f << " " << carHeight * 0.5f << " " << carLength * 0.5f << "\n";

        // Unit cube centered at origin
        file << "    Shape \"trianglemesh\"\n";
        file << "        \"point3 P\" [\n";
        file << "            -1 -1 -1  1 -1 -1  1 1 -1  -1 1 -1\n";
        file << "            -1 -1  1  1 -1  1  1 1  1  -1 1  1\n";
        file << "        ]\n";
        file << "        \"integer indices\" [\n";
        file << "            0 2 1  0 3 2  4 5 6  4 6 7\n";
        file << "            0 1 5  0 5 4  2 3 7  2 7 6\n";
        file << "            0 4 7  0 7 3  1 2 6  1 6 5\n";
        file << "        ]\n";
        file << "AttributeEnd\n\n";
    }

    // Headlights - calculate positions based on car positions (matches D3D12_Update)
    file << "# Headlights (spotlights)\n";
    int numLights = (renderer.activeLightCount > 0) ? renderer.activeLightCount : (int)renderer.numConeLights;
    int lightsExported = 0;

    for (uint32_t carIdx = 0; carIdx < renderer.numCars && lightsExported < numLights; carIdx++)
    {
        const CarData& car = carData[carIdx];

        // Front of car
        float frontOffset = carLength * 0.5f;
        Vec3 frontPos = car.pos + car.dir * frontOffset;
        frontPos.y = headlightHeight;

        // Left headlight (negate X for coordinate system conversion)
        if (lightsExported < numLights)
        {
            Vec3 leftOffset = car.right * (-headlightSpacing);
            Vec3 lightPos = frontPos + leftOffset;
            Vec3 lightTarget = lightPos + car.dir * 10.0f;
            float coneAngle = headlightOuterAngle * 180.0f / PI;
            float power = renderer.coneLightIntensity * 500.0f;

            file << "AttributeBegin\n";
            file << "    LightSource \"spot\"\n";
            file << "        \"point3 from\" [ " << -lightPos.x << " " << lightPos.y << " " << lightPos.z << " ]\n";
            file << "        \"point3 to\" [ " << -lightTarget.x << " " << lightTarget.y << " " << lightTarget.z << " ]\n";
            file << "        \"float coneangle\" [ " << coneAngle << " ]\n";
            file << "        \"float conedeltaangle\" [ 5 ]\n";
            file << "        \"rgb I\" [ " << 1.5f * power << " " << 1.4f * power << " " << 1.2f * power << " ]\n";
            file << "AttributeEnd\n\n";
            lightsExported++;
        }

        // Right headlight (negate X for coordinate system conversion)
        if (lightsExported < numLights)
        {
            Vec3 rightOffset = car.right * headlightSpacing;
            Vec3 lightPos = frontPos + rightOffset;
            Vec3 lightTarget = lightPos + car.dir * 10.0f;
            float coneAngle = headlightOuterAngle * 180.0f / PI;
            float power = renderer.coneLightIntensity * 500.0f;

            file << "AttributeBegin\n";
            file << "    LightSource \"spot\"\n";
            file << "        \"point3 from\" [ " << -lightPos.x << " " << lightPos.y << " " << lightPos.z << " ]\n";
            file << "        \"point3 to\" [ " << -lightTarget.x << " " << lightTarget.y << " " << lightTarget.z << " ]\n";
            file << "        \"float coneangle\" [ " << coneAngle << " ]\n";
            file << "        \"float conedeltaangle\" [ 5 ]\n";
            file << "        \"rgb I\" [ " << 1.5f * power << " " << 1.4f * power << " " << 1.2f * power << " ]\n";
            file << "AttributeEnd\n\n";
            lightsExported++;
        }
    }

    // pbrt-v4 no WorldEnd
    file.close();

    return true;
}

// Copy state to clipboard
static bool CopyStateToClipboard(HWND hwnd, const D3D12Renderer& renderer)
{
    std::string state = SerializeState(renderer);

    if (!OpenClipboard(hwnd))
        return false;

    EmptyClipboard();

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, state.size() + 1);
    if (!hMem)
    {
        CloseClipboard();
        return false;
    }

    char* pMem = (char*)GlobalLock(hMem);
    memcpy(pMem, state.c_str(), state.size() + 1);
    GlobalUnlock(hMem);

    SetClipboardData(CF_TEXT, hMem);
    CloseClipboard();
    return true;
}

// Paste state from clipboard
static bool PasteStateFromClipboard(HWND hwnd, D3D12Renderer& renderer)
{
    if (!OpenClipboard(hwnd))
        return false;

    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData)
    {
        CloseClipboard();
        return false;
    }

    char* pData = (char*)GlobalLock(hData);
    if (!pData)
    {
        CloseClipboard();
        return false;
    }

    std::string state(pData);
    GlobalUnlock(hData);
    CloseClipboard();

    return DeserializeState(renderer, state);
}

// Write TGA file (uncompressed, 32-bit BGRA)
static bool WriteTGA(const char* filename, uint32_t width, uint32_t height, const uint8_t* pixels)
{
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
        return false;

    // TGA header
    uint8_t header[18] = {};
    header[2] = 2;  // Uncompressed true-color
    header[12] = width & 0xFF;
    header[13] = (width >> 8) & 0xFF;
    header[14] = height & 0xFF;
    header[15] = (height >> 8) & 0xFF;
    header[16] = 32;  // 32 bits per pixel
    header[17] = 0x20;  // Top-left origin

    file.write((char*)header, sizeof(header));
    file.write((char*)pixels, width * height * 4);

    return true;
}

// Generate output filename from config filename
// "config.cfg" -> "config_test_out.tga"
static std::string GenerateTestOutputFilename(const std::string& configFile)
{
    std::string result = configFile;

    // Remove .cfg extension if present
    size_t dotPos = result.rfind(".cfg");
    if (dotPos != std::string::npos && dotPos == result.length() - 4)
    {
        result = result.substr(0, dotPos);
    }

    // Also remove path, keep just the filename
    size_t slashPos = result.rfind('/');
    size_t backslashPos = result.rfind('\\');
    size_t pathEnd = 0;
    if (slashPos != std::string::npos) pathEnd = slashPos + 1;
    if (backslashPos != std::string::npos && backslashPos >= pathEnd) pathEnd = backslashPos + 1;
    if (pathEnd > 0) result = result.substr(pathEnd);

    return result + "_test_out.tga";
}

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
    if (g_Keys['Q']) moveDir += Vec3(0, -1, 0);

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
    ImGui::Checkbox("Show Grid", &g_Renderer.showGrid);

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

        // Ctrl+C: Copy state to clipboard
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            CopyStateToClipboard(hwnd, g_Renderer);
        }

        // Ctrl+V: Paste state from clipboard
        if (wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            PasteStateFromClipboard(hwnd, g_Renderer);
        }

        // Ctrl+1..9: Save bookmark to 1.cfg..9.cfg
        if (wParam >= '1' && wParam <= '9' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            char filename[16];
            snprintf(filename, sizeof(filename), "%c.cfg", (char)wParam);
            SaveStateToFile(g_Renderer, filename);
        }

        // 1..9: Load bookmark from 1.cfg..9.cfg (without modifiers)
        if (wParam >= '1' && wParam <= '9' &&
            !(GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_MENU) & 0x8000) &&
            !(GetKeyState(VK_SHIFT) & 0x8000))
        {
            char filename[16];
            snprintf(filename, sizeof(filename), "%c.cfg", (char)wParam);
            LoadStateFromFile(g_Renderer, filename);
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

    case WM_MOUSEWHEEL:
        {
            // Adjust camera speed with mouse wheel
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            float multiplier = (delta > 0) ? 1.2f : (1.0f / 1.2f);
            g_Renderer.camera.moveSpeed *= multiplier;
            // Clamp to reasonable range
            if (g_Renderer.camera.moveSpeed < 1.0f) g_Renderer.camera.moveSpeed = 1.0f;
            if (g_Renderer.camera.moveSpeed > 10000.0f) g_Renderer.camera.moveSpeed = 10000.0f;
        }
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

    // Parse command line for .cfg file to load or -test mode
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        for (int i = 1; i < argc; i++)
        {
            // Convert wide string to narrow string
            int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (len > 0)
            {
                char* arg = new char[len];
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg, len, nullptr, nullptr);

                // Check for -test flag
                if (strcmp(arg, "-test") == 0 && i + 1 < argc)
                {
                    g_TestMode = true;
                    // Get the next argument as config file
                    int cfgLen = WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0, nullptr, nullptr);
                    if (cfgLen > 0)
                    {
                        char* cfgFile = new char[cfgLen];
                        WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, cfgFile, cfgLen, nullptr, nullptr);
                        g_TestConfigFile = cfgFile;
                        g_TestOutputFile = GenerateTestOutputFilename(g_TestConfigFile);
                        LoadStateFromFile(g_Renderer, cfgFile);
                        delete[] cfgFile;
                    }
                    i++;  // Skip next argument
                }
				// Check for -generate-ref flag
				else if (strcmp(arg, "-generate-ref") == 0 && i + 1 < argc)
                {
                    g_GenerateRefMode = true;
                    // Get the next argument as config file
                    int cfgLen = WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0, nullptr, nullptr);
                    if (cfgLen > 0)
                    {
                        char* cfgFile = new char[cfgLen];
                        WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, cfgFile, cfgLen, nullptr, nullptr);
                        g_GenerateRefConfigFile = cfgFile;
                        LoadStateFromFile(g_Renderer, cfgFile);
                        delete[] cfgFile;
                    }
                    i++;  // Skip next argument
                }
                // Check if it's a .cfg file (for non-test loading)
                else
                {
                    size_t argLen = strlen(arg);
                    if (argLen > 4 && strcmp(arg + argLen - 4, ".cfg") == 0)
                    {
                        LoadStateFromFile(g_Renderer, arg);
                    }
                }
                delete[] arg;
            }
        }
        LocalFree(argv);
    }

    // Handle -generate-ref mode: export and exit immediately
    if (g_GenerateRefMode)
    {
        // Generate output filename from config file
        std::string outputFile = g_GenerateRefConfigFile;
        size_t dotPos = outputFile.rfind('.');
        if (dotPos != std::string::npos)
            outputFile = outputFile.substr(0, dotPos);
        outputFile += ".pbrt";

        if (ExportToPBRT(g_Renderer, outputFile.c_str()))
        {
            printf("Exported PBRT scene to: %s\n", outputFile.c_str());
            printf("Render with: pbrt %s\n", outputFile.c_str());
        }
        else
        {
            printf("ERROR: Failed to export PBRT scene\n");
        }

        D3D12_Shutdown(&g_Renderer);
        DestroyWindow(g_Hwnd);
        return 0;
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

            // Start ImGui frame (skip in test mode)
            if (!g_TestMode)
            {
                ImGui_ImplDX12_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();

                // Draw ImGui UI
                DrawImGui(deltaTime);

                // Render ImGui
                ImGui::Render();
            }

            // Render scene + ImGui
            D3D12_Render(&g_Renderer);

            // Test mode: capture backbuffer after N frames and exit
            if (g_TestMode)
            {
                g_TestFrameCount++;
                if (g_TestFrameCount >= TEST_FRAME_WAIT)
                {
                    uint8_t* pixels = nullptr;
                    uint32_t width = 0, height = 0;

                    if (D3D12_CaptureBackbuffer(&g_Renderer, &pixels, &width, &height))
                    {
                        WriteTGA(g_TestOutputFile.c_str(), width, height, pixels);
                        delete[] pixels;
                    }

                    g_Running = false;
                }
            }
        }
    }

    // Cleanup
    D3D12_Shutdown(&g_Renderer);

    return 0;
}
