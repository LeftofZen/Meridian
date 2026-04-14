#include "renderer/WorldSceneRenderer.hpp"

#include "renderer/VulkanContext.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace Meridian {

namespace {

constexpr float kWireframeVerticalFovDegrees = 42.0F;
constexpr float kWireframeNearPlane = 0.05F;

struct Vec3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

[[nodiscard]] Vec3 toVec3(const std::array<float, 3>& value) noexcept
{
    return Vec3{value[0], value[1], value[2]};
}

[[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept
{
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] Vec3 add(Vec3 left, Vec3 right) noexcept
{
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] Vec3 multiply(Vec3 value, float scalar) noexcept
{
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] float dot(Vec3 left, Vec3 right) noexcept
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] Vec3 cross(Vec3 left, Vec3 right) noexcept
{
    return Vec3{
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] float length(Vec3 value) noexcept
{
    return std::sqrt(dot(value, value));
}

[[nodiscard]] Vec3 normalize(Vec3 value) noexcept
{
    const float valueLength = length(value);
    if (valueLength <= 0.0F) {
        return Vec3{};
    }

    return multiply(value, 1.0F / valueLength);
}

struct CameraBasis {
    Vec3 position;
    Vec3 forward;
    Vec3 right;
    Vec3 up;
};

[[nodiscard]] CameraBasis buildCameraBasis(const CameraRenderState& camera) noexcept
{
    const Vec3 position = toVec3(camera.position);
    const Vec3 forward = normalize(toVec3(camera.forward));
    const Vec3 worldUp = std::abs(forward.y) > 0.999F
        ? Vec3{0.0F, 0.0F, 1.0F}
        : Vec3{0.0F, 1.0F, 0.0F};
    const Vec3 right = normalize(cross(forward, worldUp));
    const Vec3 up = normalize(cross(right, forward));
    return CameraBasis{
        .position = position,
        .forward = forward,
        .right = right,
        .up = up,
    };
}

struct CameraSpacePoint {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

[[nodiscard]] CameraSpacePoint worldToCameraSpace(const CameraBasis& basis, Vec3 worldPoint) noexcept
{
    const Vec3 relative = subtract(worldPoint, basis.position);
    return CameraSpacePoint{
        .x = dot(relative, basis.right),
        .y = dot(relative, basis.up),
        .z = dot(relative, basis.forward),
    };
}

[[nodiscard]] bool clipLineToNearPlane(
    CameraSpacePoint& start,
    CameraSpacePoint& end,
    float nearPlane) noexcept
{
    const bool startVisible = start.z > nearPlane;
    const bool endVisible = end.z > nearPlane;
    if (!startVisible && !endVisible) {
        return false;
    }

    if (startVisible && endVisible) {
        return true;
    }

    const float t = (nearPlane - start.z) / (end.z - start.z);
    CameraSpacePoint clippedPoint{
        .x = start.x + (end.x - start.x) * t,
        .y = start.y + (end.y - start.y) * t,
        .z = nearPlane,
    };

    if (!startVisible) {
        start = clippedPoint;
    } else {
        end = clippedPoint;
    }

    return true;
}

[[nodiscard]] ImVec2 projectToScreen(
    CameraSpacePoint point,
    float tanHalfFov,
    float aspectRatio,
    ImVec2 displaySize) noexcept
{
    const float ndcX = point.x / (point.z * tanHalfFov * aspectRatio);
    const float ndcY = point.y / (point.z * tanHalfFov);
    return ImVec2(
        (ndcX * 0.5F + 0.5F) * displaySize.x,
        (-ndcY * 0.5F + 0.5F) * displaySize.y);
}

} // namespace

bool WorldSceneRenderer::init(VulkanContext& context)
{
    m_context = &context;
    return true;
}

void WorldSceneRenderer::shutdown()
{
    m_context = nullptr;
}

void WorldSceneRenderer::handleEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN &&
        event.key.scancode == SDL_SCANCODE_F4 &&
        !event.key.repeat) {
        m_chunkWireframeEnabled = !m_chunkWireframeEnabled;
    }
}

void WorldSceneRenderer::configureFrame(RenderFrameConfig& config)
{
    if (m_renderStateStore != nullptr) {
        m_renderStateSnapshot = m_renderStateStore->snapshot();
    }

    const float residentBlend = residentChunkBlend();
    config.clearColor = {
        0.05F + 0.06F * residentBlend,
        0.08F + 0.10F * residentBlend,
        0.12F + 0.14F * residentBlend,
        1.0F,
    };
}

void WorldSceneRenderer::beginFrame()
{
    if (m_context == nullptr || m_renderStateStore == nullptr) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(360.0F, 0.0F), ImGuiCond_FirstUseEver);
    ImGui::Begin("World Scene");
    ImGui::TextUnformatted("Scene module");
    ImGui::Separator();
    ImGui::Text("Resident chunks: %llu",
        static_cast<unsigned long long>(m_renderStateSnapshot.world.residentChunkCount));
    ImGui::Text("Generating chunks: %llu",
        static_cast<unsigned long long>(m_renderStateSnapshot.world.inFlightChunkCount));
    ImGui::Text("Queued chunks: %llu",
        static_cast<unsigned long long>(m_renderStateSnapshot.world.pendingChunkCount));
    ImGui::Spacing();
    ImGui::TextWrapped(
        "This module now consumes a render-state snapshot, which is the place to grow world-facing data like camera state, lighting state, and GPU residency without coupling scene rendering to the simulation thread.");
    ImGui::Text("Present mode: %s", m_context->getPresentModeName());
    ImGui::Spacing();

    float generationDistanceChunks =
        m_renderStateSnapshot.worldRenderSettings.chunkGenerationDistanceChunks;
    if (ImGui::SliderFloat(
            "Chunk Generation Distance (chunks)",
            &generationDistanceChunks,
            1.0F,
            32.0F,
            "%.1f")) {
        m_renderStateStore->setWorldChunkGenerationDistanceChunks(generationDistanceChunks);
        m_renderStateSnapshot.worldRenderSettings.chunkGenerationDistanceChunks =
            generationDistanceChunks;
    }

    ImGui::Text(
        "Approx generation radius: %.0f m",
        m_renderStateSnapshot.worldRenderSettings.chunkGenerationDistanceChunks * 32.0F);
    ImGui::Text(
        "Chunk Wireframe: %s (F4)",
        m_chunkWireframeEnabled ? "On" : "Off");
    ImGui::TextWrapped(
        "Chunk generation distance controls how far terrain stays resident and gets streamed from cache or generated around the camera.");
    ImGui::End();

    drawChunkWireframeOverlay();
}

void WorldSceneRenderer::drawChunkWireframeOverlay()
{
    if (!m_chunkWireframeEnabled || m_context == nullptr) {
        return;
    }

    const VkExtent2D swapchainExtent = m_context->getSwapchainExtent();
    if (swapchainExtent.width == 0 || swapchainExtent.height == 0) {
        return;
    }

    const ImVec2 displaySize{
        static_cast<float>(swapchainExtent.width),
        static_cast<float>(swapchainExtent.height),
    };
    const float aspectRatio = displaySize.x / std::max(displaySize.y, 1.0F);
    const float tanHalfFov = std::tan(kWireframeVerticalFovDegrees * 0.5F * 3.14159265F / 180.0F);
    const CameraBasis cameraBasis = buildCameraBasis(m_renderStateSnapshot.camera);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    constexpr std::array<std::array<int, 2>, 12> edges{{
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    }};

    for (const WorldChunkRenderData& chunk : m_renderStateSnapshot.world.chunks) {
        const float chunkExtent = static_cast<float>(chunk.voxelResolution);
        const Vec3 chunkMin{
            static_cast<float>(chunk.coord.x * static_cast<int>(chunk.voxelResolution)),
            static_cast<float>(chunk.coord.y * static_cast<int>(chunk.voxelResolution)),
            static_cast<float>(chunk.coord.z * static_cast<int>(chunk.voxelResolution)),
        };
        const Vec3 chunkMax = add(chunkMin, Vec3{chunkExtent, chunkExtent, chunkExtent});

        const std::array<Vec3, 8> corners{{
            {chunkMin.x, chunkMin.y, chunkMin.z},
            {chunkMax.x, chunkMin.y, chunkMin.z},
            {chunkMin.x, chunkMax.y, chunkMin.z},
            {chunkMax.x, chunkMax.y, chunkMin.z},
            {chunkMin.x, chunkMin.y, chunkMax.z},
            {chunkMax.x, chunkMin.y, chunkMax.z},
            {chunkMin.x, chunkMax.y, chunkMax.z},
            {chunkMax.x, chunkMax.y, chunkMax.z},
        }};

        for (const auto& edge : edges) {
            const Vec3 edgeDelta = subtract(corners[edge[1]], corners[edge[0]]);
            const ImU32 edgeColor = std::abs(edgeDelta.x) > 0.0F
                ? IM_COL32(255, 96, 96, 255)
                : (std::abs(edgeDelta.y) > 0.0F
                    ? IM_COL32(96, 255, 96, 255)
                    : IM_COL32(96, 160, 255, 255));

            CameraSpacePoint start = worldToCameraSpace(cameraBasis, corners[edge[0]]);
            CameraSpacePoint end = worldToCameraSpace(cameraBasis, corners[edge[1]]);
            if (!clipLineToNearPlane(start, end, kWireframeNearPlane)) {
                continue;
            }

            const ImVec2 startScreen = projectToScreen(start, tanHalfFov, aspectRatio, displaySize);
            const ImVec2 endScreen = projectToScreen(end, tanHalfFov, aspectRatio, displaySize);
            drawList->AddLine(startScreen, endScreen, edgeColor, 1.0F);
        }
    }
}

float WorldSceneRenderer::residentChunkBlend() const noexcept
{
    constexpr float expectedBootstrapChunks = 27.0F;
    const float residentChunks = static_cast<float>(m_renderStateSnapshot.world.residentChunkCount);
    return std::clamp(residentChunks / expectedBootstrapChunks, 0.0F, 1.0F);
}

} // namespace Meridian
