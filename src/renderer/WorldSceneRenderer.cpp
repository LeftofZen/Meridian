#include "renderer/WorldSceneRenderer.hpp"

#include "renderer/VulkanContext.hpp"
#include "world/SparseVoxelOctree.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <unordered_map>

namespace Meridian {

namespace {

constexpr float kMinProjectionAspectRatio = 0.01F;
constexpr float kMinProjectionVerticalFovDegrees = 1.0F;
constexpr float kMinProjectionNearPlane = 0.01F;
constexpr float kWireframeTerrainProbeOffset = 0.15F;
constexpr std::uint32_t kPackedNodeWordCount = 9U;
constexpr float kPi = 3.14159265F;
constexpr float kLightGizmoAxisLengthMin = 3.5F;
constexpr float kLightGizmoAxisLengthMax = 12.0F;
constexpr float kLightGizmoAxisLengthDistanceScale = 0.10F;
constexpr float kLightGizmoHandleRadiusPixels = 7.0F;
constexpr float kLightGizmoHitRadiusPixels = 12.0F;
constexpr float kLightGizmoLabelOffsetPixels = 10.0F;

struct Vec3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

struct Ray3 {
    Vec3 origin;
    Vec3 direction;
};

struct CameraSpacePoint {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

struct ProjectedPoint {
    bool visible{false};
    ImVec2 screen{};
    float depth{0.0F};
};

struct LightGizmoHitCandidate {
    bool valid{false};
    bool pointLight{true};
    std::size_t lightIndex{0};
    int axisIndex{0};
    float screenDistance{std::numeric_limits<float>::max()};
    float depth{std::numeric_limits<float>::max()};
};

[[nodiscard]] Vec3 toVec3(const std::array<float, 3>& value) noexcept
{
    return Vec3{value[0], value[1], value[2]};
}

void storeVec3(std::array<float, 3>& target, Vec3 value) noexcept
{
    target = {value.x, value.y, value.z};
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

[[nodiscard]] Vec3 axisDirection(int axisIndex) noexcept
{
    switch (axisIndex) {
    case 0:
        return Vec3{1.0F, 0.0F, 0.0F};
    case 1:
        return Vec3{0.0F, 1.0F, 0.0F};
    default:
        return Vec3{0.0F, 0.0F, 1.0F};
    }
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

[[nodiscard]] float distance(Vec3 left, Vec3 right) noexcept
{
    return length(subtract(left, right));
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

[[nodiscard]] ProjectedPoint projectWorldPoint(
    const CameraBasis& basis,
    Vec3 point,
    float nearPlane,
    float tanHalfFov,
    float aspectRatio,
    ImVec2 displaySize) noexcept
{
    const CameraSpacePoint cameraPoint = worldToCameraSpace(basis, point);
    if (cameraPoint.z <= nearPlane) {
        return ProjectedPoint{};
    }

    return ProjectedPoint{
        .visible = true,
        .screen = projectToScreen(cameraPoint, tanHalfFov, aspectRatio, displaySize),
        .depth = cameraPoint.z,
    };
}

[[nodiscard]] float axisLengthForDistance(float cameraDistance) noexcept
{
    return std::clamp(
        cameraDistance * kLightGizmoAxisLengthDistanceScale,
        kLightGizmoAxisLengthMin,
        kLightGizmoAxisLengthMax);
}

[[nodiscard]] float screenDistance(ImVec2 left, ImVec2 right) noexcept
{
    const float deltaX = left.x - right.x;
    const float deltaY = left.y - right.y;
    return std::sqrt(deltaX * deltaX + deltaY * deltaY);
}

[[nodiscard]] ImU32 axisColor(int axisIndex, bool highlighted) noexcept
{
    switch (axisIndex) {
    case 0:
        return highlighted ? IM_COL32(255, 176, 176, 255) : IM_COL32(255, 96, 96, 255);
    case 1:
        return highlighted ? IM_COL32(176, 255, 176, 255) : IM_COL32(96, 255, 96, 255);
    default:
        return highlighted ? IM_COL32(176, 208, 255, 255) : IM_COL32(96, 160, 255, 255);
    }
}

[[nodiscard]] ImU32 centerColor(bool pointLight) noexcept
{
    return pointLight ? IM_COL32(255, 214, 96, 255) : IM_COL32(255, 144, 64, 255);
}

[[nodiscard]] Ray3 screenRay(
    ImVec2 mousePosition,
    ImVec2 displaySize,
    float aspectRatio,
    float tanHalfFov,
    const CameraBasis& basis) noexcept
{
    const float ndcX = (mousePosition.x / displaySize.x) * 2.0F - 1.0F;
    const float ndcY = -((mousePosition.y / displaySize.y) * 2.0F - 1.0F);
    return Ray3{
        .origin = basis.position,
        .direction = normalize(add(
            basis.forward,
            add(
                multiply(basis.right, ndcX * tanHalfFov * aspectRatio),
                multiply(basis.up, ndcY * tanHalfFov)))),
    };
}

[[nodiscard]] bool pointOnAxisDragPlane(
    Vec3 axisOrigin,
    Vec3 axisDir,
    const Ray3& ray,
    Vec3 cameraForward,
    float& outAxisParameter) noexcept
{
    Vec3 planeBinormal = cross(cameraForward, axisDir);
    if (length(planeBinormal) <= 0.0001F) {
        planeBinormal = cross(Vec3{0.0F, 1.0F, 0.0F}, axisDir);
    }
    if (length(planeBinormal) <= 0.0001F) {
        planeBinormal = cross(Vec3{1.0F, 0.0F, 0.0F}, axisDir);
    }

    const Vec3 planeNormalUnnormalized = cross(axisDir, planeBinormal);
    if (length(planeNormalUnnormalized) <= 0.0001F) {
        return false;
    }

    const Vec3 planeNormal = normalize(planeNormalUnnormalized);
    const float denominator = dot(planeNormal, ray.direction);
    if (std::abs(denominator) <= 0.0001F) {
        return false;
    }

    const float t = dot(planeNormal, subtract(axisOrigin, ray.origin)) / denominator;
    const Vec3 intersectionPoint = add(ray.origin, multiply(ray.direction, t));
    outAxisParameter = dot(subtract(intersectionPoint, axisOrigin), axisDir);
    return true;
}

[[nodiscard]] bool drawWorldLine(
    ImDrawList* drawList,
    const CameraBasis& basis,
    Vec3 start,
    Vec3 end,
    float nearPlane,
    float tanHalfFov,
    float aspectRatio,
    ImVec2 displaySize,
    ImU32 color,
    float thickness) noexcept
{
    CameraSpacePoint startCamera = worldToCameraSpace(basis, start);
    CameraSpacePoint endCamera = worldToCameraSpace(basis, end);
    if (!clipLineToNearPlane(startCamera, endCamera, nearPlane)) {
        return false;
    }

    const ImVec2 screenStart = projectToScreen(startCamera, tanHalfFov, aspectRatio, displaySize);
    const ImVec2 screenEnd = projectToScreen(endCamera, tanHalfFov, aspectRatio, displaySize);
    drawList->AddLine(screenStart, screenEnd, color, thickness);
    return true;
}

[[nodiscard]] std::array<float, 3>* editableLightPosition(
    LightingRenderSnapshot& lighting,
    bool pointLight,
    std::size_t lightIndex) noexcept
{
    if (pointLight) {
        if (lightIndex >= lighting.pointLights.size()) {
            return nullptr;
        }

        return &lighting.pointLights[lightIndex].positionMeters;
    }

    if (lightIndex >= lighting.areaLights.size()) {
        return nullptr;
    }

    return &lighting.areaLights[lightIndex].centerMeters;
}

[[nodiscard]] Vec3 lightPosition(
    const LightingRenderSnapshot& lighting,
    bool pointLight,
    std::size_t lightIndex) noexcept
{
    return pointLight
        ? toVec3(lighting.pointLights[lightIndex].positionMeters)
        : toVec3(lighting.areaLights[lightIndex].centerMeters);
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
    clearActiveLightDrag();
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
    ImGui::Text(
        "Lights: %zu point, %zu area",
        m_renderStateSnapshot.lighting.pointLights.size(),
        m_renderStateSnapshot.lighting.areaLights.size());
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
    drawLightGizmoOverlay();
}

void WorldSceneRenderer::clearActiveLightDrag() noexcept
{
    m_activeLightDrag = LightGizmoDragState{};
}

void WorldSceneRenderer::drawLightGizmoOverlay()
{
    if (m_context == nullptr || m_renderStateStore == nullptr) {
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
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mousePosition = io.MousePos;
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    LightingRenderSnapshot lighting = m_renderStateSnapshot.lighting;
    if ((m_activeLightDrag.active && m_activeLightDrag.pointLight &&
            m_activeLightDrag.lightIndex >= lighting.pointLights.size()) ||
        (m_activeLightDrag.active && !m_activeLightDrag.pointLight &&
            m_activeLightDrag.lightIndex >= lighting.areaLights.size())) {
        clearActiveLightDrag();
    }

    LightGizmoHitCandidate hoveredCandidate{};
    const bool canPickHandle = !m_activeLightDrag.active && !io.WantCaptureMouse;

    const auto considerLightHandles = [&](bool pointLight, std::size_t lightIndex, Vec3 position) {
        const float axisLength = axisLengthForDistance(distance(cameraBasis.position, position));
        for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
            const Vec3 handlePosition = add(position, multiply(axisDirection(axisIndex), axisLength));
            const ProjectedPoint projectedHandle = projectWorldPoint(
                cameraBasis,
                handlePosition,
                nearPlane,
                tanHalfFov,
                aspectRatio,
                displaySize);
            if (!projectedHandle.visible) {
                continue;
            }

            const float hitDistance = screenDistance(projectedHandle.screen, mousePosition);
            if (hitDistance > kLightGizmoHitRadiusPixels) {
                continue;
            }

            if (!hoveredCandidate.valid ||
                hitDistance < hoveredCandidate.screenDistance ||
                (std::abs(hitDistance - hoveredCandidate.screenDistance) < 0.001F &&
                    projectedHandle.depth < hoveredCandidate.depth)) {
                hoveredCandidate = LightGizmoHitCandidate{
                    .valid = true,
                    .pointLight = pointLight,
                    .lightIndex = lightIndex,
                    .axisIndex = axisIndex,
                    .screenDistance = hitDistance,
                    .depth = projectedHandle.depth,
                };
            }
        }
    };

    if (canPickHandle) {
        for (std::size_t lightIndex = 0; lightIndex < lighting.pointLights.size(); ++lightIndex) {
            considerLightHandles(true, lightIndex, lightPosition(lighting, true, lightIndex));
        }
        for (std::size_t lightIndex = 0; lightIndex < lighting.areaLights.size(); ++lightIndex) {
            considerLightHandles(false, lightIndex, lightPosition(lighting, false, lightIndex));
        }
    }

    if (!m_activeLightDrag.active && io.MouseClicked[0] && hoveredCandidate.valid) {
        const Vec3 origin = lightPosition(lighting, hoveredCandidate.pointLight, hoveredCandidate.lightIndex);
        const Vec3 dragAxis = axisDirection(hoveredCandidate.axisIndex);
        const Ray3 ray = screenRay(mousePosition, displaySize, aspectRatio, tanHalfFov, cameraBasis);
        float axisParameter = 0.0F;
        if (pointOnAxisDragPlane(origin, dragAxis, ray, cameraBasis.forward, axisParameter)) {
            m_activeLightDrag.active = true;
            m_activeLightDrag.pointLight = hoveredCandidate.pointLight;
            m_activeLightDrag.lightIndex = hoveredCandidate.lightIndex;
            m_activeLightDrag.axisIndex = hoveredCandidate.axisIndex;
            m_activeLightDrag.startPosition = {origin.x, origin.y, origin.z};
            m_activeLightDrag.startAxisParameter = axisParameter;
        }
    }

    bool lightingChanged = false;
    if (m_activeLightDrag.active) {
        if (!io.MouseDown[0]) {
            clearActiveLightDrag();
        } else {
            std::array<float, 3>* dragPosition = editableLightPosition(
                lighting,
                m_activeLightDrag.pointLight,
                m_activeLightDrag.lightIndex);
            if (dragPosition == nullptr) {
                clearActiveLightDrag();
            } else {
                const Vec3 dragOrigin = toVec3(m_activeLightDrag.startPosition);
                const Vec3 dragAxis = axisDirection(m_activeLightDrag.axisIndex);
                const Ray3 ray = screenRay(mousePosition, displaySize, aspectRatio, tanHalfFov, cameraBasis);
                float axisParameter = 0.0F;
                if (pointOnAxisDragPlane(dragOrigin, dragAxis, ray, cameraBasis.forward, axisParameter)) {
                    const float delta = axisParameter - m_activeLightDrag.startAxisParameter;
                    storeVec3(*dragPosition, add(dragOrigin, multiply(dragAxis, delta)));
                    lightingChanged = true;
                }
            }
        }
    }

    const auto drawPointMarker = [&](Vec3 position, bool pointLight, const char* label) {
        const ProjectedPoint projectedCenter = projectWorldPoint(
            cameraBasis,
            position,
            nearPlane,
            tanHalfFov,
            aspectRatio,
            displaySize);
        if (!projectedCenter.visible) {
            return;
        }

        drawList->AddCircleFilled(projectedCenter.screen, 5.0F, centerColor(pointLight));
        drawList->AddCircle(projectedCenter.screen, 6.5F, IM_COL32(16, 18, 20, 255), 0, 1.5F);
        drawList->AddText(
            ImVec2(
                projectedCenter.screen.x + kLightGizmoLabelOffsetPixels,
                projectedCenter.screen.y - kLightGizmoLabelOffsetPixels),
            IM_COL32(255, 255, 255, 255),
            label);
    };

    const auto drawAxes = [&](bool pointLight, std::size_t lightIndex, Vec3 position) {
        const bool isActiveLight =
            m_activeLightDrag.active &&
            m_activeLightDrag.pointLight == pointLight &&
            m_activeLightDrag.lightIndex == lightIndex;
        const bool isHoveredLight =
            hoveredCandidate.valid &&
            hoveredCandidate.pointLight == pointLight &&
            hoveredCandidate.lightIndex == lightIndex;
        const float axisLength = axisLengthForDistance(distance(cameraBasis.position, position));

        for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
            const bool highlighted =
                (isActiveLight && m_activeLightDrag.axisIndex == axisIndex) ||
                (isHoveredLight && hoveredCandidate.axisIndex == axisIndex);
            const Vec3 axisEnd = add(position, multiply(axisDirection(axisIndex), axisLength));
            const ImU32 color = axisColor(axisIndex, highlighted);
            const float lineThickness = highlighted ? 3.0F : 2.0F;
            if (!drawWorldLine(
                    drawList,
                    cameraBasis,
                    position,
                    axisEnd,
                    nearPlane,
                    tanHalfFov,
                    aspectRatio,
                    displaySize,
                    color,
                    lineThickness)) {
                continue;
            }

            const ProjectedPoint projectedHandle = projectWorldPoint(
                cameraBasis,
                axisEnd,
                nearPlane,
                tanHalfFov,
                aspectRatio,
                displaySize);
            if (!projectedHandle.visible) {
                continue;
            }

            drawList->AddCircleFilled(
                projectedHandle.screen,
                highlighted ? kLightGizmoHandleRadiusPixels + 1.5F : kLightGizmoHandleRadiusPixels,
                color);
            drawList->AddCircle(
                projectedHandle.screen,
                highlighted ? kLightGizmoHandleRadiusPixels + 2.5F : kLightGizmoHandleRadiusPixels + 1.5F,
                IM_COL32(20, 22, 24, 255),
                0,
                1.5F);
        }
    };

    for (std::size_t lightIndex = 0; lightIndex < lighting.pointLights.size(); ++lightIndex) {
        const Vec3 position = lightPosition(lighting, true, lightIndex);
        char label[16]{};
        std::snprintf(label, sizeof(label), "P%zu", lightIndex);
        drawPointMarker(position, true, label);
        drawAxes(true, lightIndex, position);
    }

    constexpr std::array<std::array<int, 2>, 4> areaEdges{{
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
    }};
    for (std::size_t lightIndex = 0; lightIndex < lighting.areaLights.size(); ++lightIndex) {
        const AreaLightRenderState& light = lighting.areaLights[lightIndex];
        const Vec3 center = toVec3(light.centerMeters);
        const Vec3 rightExtent = toVec3(light.rightExtentMeters);
        const Vec3 upExtent = toVec3(light.upExtentMeters);
        const std::array<Vec3, 4> corners{{
            add(add(center, rightExtent), upExtent),
            add(subtract(center, rightExtent), upExtent),
            subtract(subtract(center, rightExtent), upExtent),
            add(subtract(center, upExtent), rightExtent),
        }};
        for (const auto& edge : areaEdges) {
            (void)drawWorldLine(
                drawList,
                cameraBasis,
                corners[edge[0]],
                corners[edge[1]],
                nearPlane,
                tanHalfFov,
                aspectRatio,
                displaySize,
                IM_COL32(255, 176, 64, 220),
                2.0F);
        }

        char label[16]{};
        std::snprintf(label, sizeof(label), "A%zu", lightIndex);
        drawPointMarker(center, false, label);
        drawAxes(false, lightIndex, center);
    }

    if (lightingChanged) {
        m_renderStateStore->updateLightingState(lighting);
        m_renderStateSnapshot.lighting = std::move(lighting);
    }
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
