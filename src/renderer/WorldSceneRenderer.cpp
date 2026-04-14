#include "renderer/WorldSceneRenderer.hpp"

#include "renderer/VulkanContext.hpp"
#include "world/SparseVoxelOctree.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace Meridian {

namespace {

constexpr float kMinProjectionAspectRatio = 0.01F;
constexpr float kMinProjectionVerticalFovDegrees = 1.0F;
constexpr float kMinProjectionNearPlane = 0.01F;
constexpr float kWireframeTerrainProbeOffset = 0.15F;
constexpr std::uint32_t kPackedNodeWordCount = 9U;
constexpr float kPi = 3.14159265F;

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

[[nodiscard]] Vec3 lerp(Vec3 start, Vec3 end, float t) noexcept
{
    return add(start, multiply(subtract(end, start), t));
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

[[nodiscard]] int worldToChunkCoord(float worldPosition, int chunkSize) noexcept
{
    return static_cast<int>(std::floor(worldPosition / static_cast<float>(chunkSize)));
}

[[nodiscard]] std::uint32_t nodeWord(
    const std::vector<std::uint32_t>& packedNodes,
    std::uint32_t nodeIndex,
    std::uint32_t wordIndex) noexcept
{
    return packedNodes[nodeIndex * kPackedNodeWordCount + wordIndex];
}

[[nodiscard]] std::uint32_t nodeMetadata(
    const std::vector<std::uint32_t>& packedNodes,
    std::uint32_t nodeIndex) noexcept
{
    return nodeWord(packedNodes, nodeIndex, 8U);
}

[[nodiscard]] bool nodeIsLeaf(
    const std::vector<std::uint32_t>& packedNodes,
    std::uint32_t nodeIndex) noexcept
{
    return ((nodeMetadata(packedNodes, nodeIndex) >> 8U) & 1U) != 0U;
}

[[nodiscard]] std::uint32_t nodeChildMask(
    const std::vector<std::uint32_t>& packedNodes,
    std::uint32_t nodeIndex) noexcept
{
    return nodeMetadata(packedNodes, nodeIndex) & 0xffU;
}

[[nodiscard]] std::uint32_t nodeMaterialId(
    const std::vector<std::uint32_t>& packedNodes,
    std::uint32_t nodeIndex) noexcept
{
    return (nodeMetadata(packedNodes, nodeIndex) >> 16U) & 0xffU;
}

[[nodiscard]] std::uint32_t nodeChildIndex(
    const std::vector<std::uint32_t>& packedNodes,
    std::uint32_t nodeIndex,
    std::uint32_t childIndex) noexcept
{
    return nodeWord(packedNodes, nodeIndex, childIndex);
}

[[nodiscard]] bool isPointSolidInChunk(
    const WorldChunkRenderData& chunk,
    Vec3 worldPoint) noexcept
{
    if (chunk.packedOctreeNodes == nullptr || chunk.packedOctreeNodes->empty()) {
        return false;
    }

    const float chunkExtent = static_cast<float>(chunk.voxelResolution);
    const Vec3 chunkMin{
        static_cast<float>(chunk.coord.x * static_cast<int>(chunk.voxelResolution)),
        static_cast<float>(chunk.coord.y * static_cast<int>(chunk.voxelResolution)),
        static_cast<float>(chunk.coord.z * static_cast<int>(chunk.voxelResolution)),
    };
    const Vec3 localPoint = subtract(worldPoint, chunkMin);
    if (localPoint.x < 0.0F || localPoint.y < 0.0F || localPoint.z < 0.0F ||
        localPoint.x >= chunkExtent || localPoint.y >= chunkExtent || localPoint.z >= chunkExtent) {
        return false;
    }

    const std::vector<std::uint32_t>& packedNodes = *chunk.packedOctreeNodes;
    const std::uint32_t nodeCount = static_cast<std::uint32_t>(packedNodes.size() / kPackedNodeWordCount);
    if (nodeCount == 0U) {
        return false;
    }

    float nodeMinX = 0.0F;
    float nodeMinY = 0.0F;
    float nodeMinZ = 0.0F;
    float nodeExtent = chunkExtent;
    std::uint32_t nodeIndex = nodeCount - 1U;

    for (;;) {
        if (nodeIsLeaf(packedNodes, nodeIndex)) {
            return nodeMaterialId(packedNodes, nodeIndex) !=
                static_cast<std::uint32_t>(VoxelMaterialId::Air);
        }

        nodeExtent *= 0.5F;
        const bool highX = localPoint.x >= nodeMinX + nodeExtent;
        const bool highY = localPoint.y >= nodeMinY + nodeExtent;
        const bool highZ = localPoint.z >= nodeMinZ + nodeExtent;
        const std::uint32_t childIndex =
            (highX ? 1U : 0U) |
            (highY ? 2U : 0U) |
            (highZ ? 4U : 0U);

        if ((nodeChildMask(packedNodes, nodeIndex) & (1U << childIndex)) == 0U) {
            return false;
        }

        nodeIndex = nodeChildIndex(packedNodes, nodeIndex, childIndex);
        if (nodeIndex == SparseVoxelOctree::kInvalidChildIndex) {
            return false;
        }

        if (highX) {
            nodeMinX += nodeExtent;
        }
        if (highY) {
            nodeMinY += nodeExtent;
        }
        if (highZ) {
            nodeMinZ += nodeExtent;
        }
    }
}

[[nodiscard]] bool isPointSolid(
    const std::unordered_map<ChunkKey, const WorldChunkRenderData*>& chunkLookup,
    Vec3 worldPoint) noexcept
{
    const ChunkCoord chunkCoord{
        .x = worldToChunkCoord(worldPoint.x, kWorldChunkSize),
        .y = worldToChunkCoord(worldPoint.y, kWorldChunkSize),
        .z = worldToChunkCoord(worldPoint.z, kWorldChunkSize),
    };
    const auto chunkIt = chunkLookup.find(makeChunkKey(chunkCoord));
    if (chunkIt == chunkLookup.end() || chunkIt->second == nullptr) {
        return false;
    }

    return isPointSolidInChunk(*chunkIt->second, worldPoint);
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
    const float aspectRatio = std::max(
        m_renderStateSnapshot.camera.projection.aspectRatio,
        kMinProjectionAspectRatio);
    const float verticalFovDegrees = std::max(
        m_renderStateSnapshot.camera.projection.verticalFovDegrees,
        kMinProjectionVerticalFovDegrees);
    const float nearPlane = std::max(
        m_renderStateSnapshot.camera.projection.nearClipDistance,
        kMinProjectionNearPlane);
    const float tanHalfFov = std::tan(verticalFovDegrees * 0.5F * kPi / 180.0F);
    const CameraBasis cameraBasis = buildCameraBasis(m_renderStateSnapshot.camera);
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    std::unordered_map<ChunkKey, const WorldChunkRenderData*> chunkLookup;
    chunkLookup.reserve(m_renderStateSnapshot.world.chunks.size());
    for (const WorldChunkRenderData& chunk : m_renderStateSnapshot.world.chunks) {
        chunkLookup.emplace(makeChunkKey(chunk.coord), &chunk);
    }

    constexpr std::array<std::array<int, 2>, 12> edges{{
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7},
    }};

    for (const WorldChunkRenderData& chunk : m_renderStateSnapshot.world.chunks) {
        const ChunkCoord cameraChunkCoord{
            .x = worldToChunkCoord(
                m_renderStateSnapshot.camera.position[0],
                static_cast<int>(chunk.voxelResolution)),
            .y = worldToChunkCoord(
                m_renderStateSnapshot.camera.position[1],
                static_cast<int>(chunk.voxelResolution)),
            .z = worldToChunkCoord(
                m_renderStateSnapshot.camera.position[2],
                static_cast<int>(chunk.voxelResolution)),
        };
        if (chunk.coord != cameraChunkCoord) {
            continue;
        }

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

        const int segmentCount = std::max(16, static_cast<int>(chunk.voxelResolution) * 2);

        for (const auto& edge : edges) {
            const Vec3 edgeDelta = subtract(corners[edge[1]], corners[edge[0]]);
            const ImU32 edgeColor = std::abs(edgeDelta.x) > 0.0F
                ? IM_COL32(255, 96, 96, 255)
                : (std::abs(edgeDelta.y) > 0.0F
                    ? IM_COL32(96, 255, 96, 255)
                    : IM_COL32(96, 160, 255, 255));

            for (int segmentIndex = 0; segmentIndex < segmentCount; ++segmentIndex) {
                const float startT = static_cast<float>(segmentIndex) / static_cast<float>(segmentCount);
                const float endT = static_cast<float>(segmentIndex + 1) / static_cast<float>(segmentCount);
                const Vec3 worldStart = lerp(corners[edge[0]], corners[edge[1]], startT);
                const Vec3 worldEnd = lerp(corners[edge[0]], corners[edge[1]], endT);
                const Vec3 midpoint = lerp(worldStart, worldEnd, 0.5F);
                const Vec3 probeDirection = normalize(subtract(cameraBasis.position, midpoint));
                const Vec3 probePoint = add(
                    midpoint,
                    multiply(probeDirection, kWireframeTerrainProbeOffset));
                if (isPointSolid(chunkLookup, probePoint)) {
                    continue;
                }

                CameraSpacePoint start = worldToCameraSpace(cameraBasis, worldStart);
                CameraSpacePoint end = worldToCameraSpace(cameraBasis, worldEnd);
                if (!clipLineToNearPlane(start, end, nearPlane)) {
                    continue;
                }

                const ImVec2 startScreen = projectToScreen(start, tanHalfFov, aspectRatio, displaySize);
                const ImVec2 endScreen = projectToScreen(end, tanHalfFov, aspectRatio, displaySize);
                drawList->AddLine(startScreen, endScreen, edgeColor, 1.0F);
            }
        }

        break;
    }
}

float WorldSceneRenderer::residentChunkBlend() const noexcept
{
    constexpr float expectedBootstrapChunks = 27.0F;
    const float residentChunks = static_cast<float>(m_renderStateSnapshot.world.residentChunkCount);
    return std::clamp(residentChunks / expectedBootstrapChunks, 0.0F, 1.0F);
}

} // namespace Meridian
