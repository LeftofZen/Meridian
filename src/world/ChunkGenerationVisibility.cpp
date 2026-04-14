#include "world/ChunkGenerationVisibility.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Meridian {

namespace {

constexpr float kMinNearClipDistance = 0.01F;
constexpr float kMinVerticalFovDegrees = 1.0F;
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

[[nodiscard]] Vec3 add(Vec3 left, Vec3 right) noexcept
{
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept
{
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
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

[[nodiscard]] int worldToChunkCoord(float worldPosition) noexcept
{
    return static_cast<int>(std::floor(worldPosition / static_cast<float>(kWorldChunkSize)));
}

} // namespace

bool ChunkGenerationVisibility::canGenerateChunk(
    ChunkCoord coord,
    const CameraRenderState& cameraState,
    const std::unordered_set<ChunkKey>& solidOccluderKeys) const noexcept
{
    return isChunkVisible(coord, cameraState) &&
        !isChunkOccluded(coord, cameraState, solidOccluderKeys);
}

bool ChunkGenerationVisibility::isChunkVisible(
    ChunkCoord coord,
    const CameraRenderState& cameraState) const noexcept
{
    const Vec3 cameraPosition = toVec3(cameraState.position);
    const Vec3 forward = normalize(toVec3(cameraState.forward));
    const Vec3 worldUp = std::abs(forward.y) > 0.999F
        ? Vec3{0.0F, 0.0F, 1.0F}
        : Vec3{0.0F, 1.0F, 0.0F};
    const Vec3 right = normalize(cross(forward, worldUp));
    const Vec3 up = normalize(cross(right, forward));
    const float nearClipDistance = std::max(cameraState.projection.nearClipDistance, kMinNearClipDistance);
    const float verticalFovDegrees = std::max(cameraState.projection.verticalFovDegrees, kMinVerticalFovDegrees);
    const float aspectRatio = std::max(cameraState.projection.aspectRatio, 1.0F);
    const float tanHalfVerticalFov = std::tan(verticalFovDegrees * 0.5F * kPi / 180.0F);
    const float tanHalfHorizontalFov = tanHalfVerticalFov * aspectRatio;

    const float chunkExtent = static_cast<float>(kWorldChunkSize);
    const Vec3 chunkMin{
        static_cast<float>(coord.x * kWorldChunkSize),
        static_cast<float>(coord.y * kWorldChunkSize),
        static_cast<float>(coord.z * kWorldChunkSize),
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

    for (const Vec3& corner : corners) {
        const Vec3 relative = subtract(corner, cameraPosition);
        const float depth = dot(relative, forward);
        if (depth <= nearClipDistance) {
            continue;
        }

        const float horizontal = dot(relative, right);
        const float vertical = dot(relative, up);
        if (std::abs(horizontal) <= depth * tanHalfHorizontalFov &&
            std::abs(vertical) <= depth * tanHalfVerticalFov) {
            return true;
        }
    }

    const Vec3 chunkCenter = add(chunkMin, Vec3{chunkExtent * 0.5F, chunkExtent * 0.5F, chunkExtent * 0.5F});
    const Vec3 centerRelative = subtract(chunkCenter, cameraPosition);
    const float centerDepth = dot(centerRelative, forward);
    if (centerDepth <= nearClipDistance) {
        return false;
    }

    const float horizontal = dot(centerRelative, right);
    const float vertical = dot(centerRelative, up);
    return std::abs(horizontal) <= centerDepth * tanHalfHorizontalFov &&
        std::abs(vertical) <= centerDepth * tanHalfVerticalFov;
}

bool ChunkGenerationVisibility::isChunkOccluded(
    ChunkCoord coord,
    const CameraRenderState& cameraState,
    const std::unordered_set<ChunkKey>& solidOccluderKeys) const noexcept
{
    const Vec3 cameraPosition = toVec3(cameraState.position);
    const float chunkExtent = static_cast<float>(kWorldChunkSize);
    const Vec3 chunkCenter{
        static_cast<float>(coord.x * kWorldChunkSize) + chunkExtent * 0.5F,
        static_cast<float>(coord.y * kWorldChunkSize) + chunkExtent * 0.5F,
        static_cast<float>(coord.z * kWorldChunkSize) + chunkExtent * 0.5F,
    };
    const Vec3 ray = subtract(chunkCenter, cameraPosition);
    const float rayLength = length(ray);
    if (rayLength <= 0.001F) {
        return false;
    }

    const Vec3 rayDirection = multiply(ray, 1.0F / rayLength);
    const float nearClipDistance = std::max(cameraState.projection.nearClipDistance, kMinNearClipDistance);
    const Vec3 startPosition = add(cameraPosition, multiply(rayDirection, nearClipDistance));
    ChunkCoord currentCoord{
        .x = worldToChunkCoord(startPosition.x),
        .y = worldToChunkCoord(startPosition.y),
        .z = worldToChunkCoord(startPosition.z),
    };

    const int stepX = rayDirection.x > 0.0F ? 1 : (rayDirection.x < 0.0F ? -1 : 0);
    const int stepY = rayDirection.y > 0.0F ? 1 : (rayDirection.y < 0.0F ? -1 : 0);
    const int stepZ = rayDirection.z > 0.0F ? 1 : (rayDirection.z < 0.0F ? -1 : 0);

    const auto nextBoundary = [](float position, int step) noexcept {
        const float chunkCoord = std::floor(position / static_cast<float>(kWorldChunkSize));
        return step > 0
            ? (chunkCoord + 1.0F) * static_cast<float>(kWorldChunkSize)
            : chunkCoord * static_cast<float>(kWorldChunkSize);
    };

    float tMaxX = stepX == 0
        ? std::numeric_limits<float>::max()
        : (nextBoundary(startPosition.x, stepX) - startPosition.x) / rayDirection.x;
    float tMaxY = stepY == 0
        ? std::numeric_limits<float>::max()
        : (nextBoundary(startPosition.y, stepY) - startPosition.y) / rayDirection.y;
    float tMaxZ = stepZ == 0
        ? std::numeric_limits<float>::max()
        : (nextBoundary(startPosition.z, stepZ) - startPosition.z) / rayDirection.z;

    const float tDeltaX = stepX == 0
        ? std::numeric_limits<float>::max()
        : chunkExtent / std::abs(rayDirection.x);
    const float tDeltaY = stepY == 0
        ? std::numeric_limits<float>::max()
        : chunkExtent / std::abs(rayDirection.y);
    const float tDeltaZ = stepZ == 0
        ? std::numeric_limits<float>::max()
        : chunkExtent / std::abs(rayDirection.z);

    const int maxChunkSteps = std::max({
        std::abs(coord.x - currentCoord.x),
        std::abs(coord.y - currentCoord.y),
        std::abs(coord.z - currentCoord.z)}) + 2;

    for (int stepIndex = 0; stepIndex < maxChunkSteps; ++stepIndex) {
        if (currentCoord == coord) {
            return false;
        }

        if (solidOccluderKeys.contains(makeChunkKey(currentCoord))) {
            return true;
        }

        if (tMaxX < tMaxY) {
            if (tMaxX < tMaxZ) {
                currentCoord.x += stepX;
                tMaxX += tDeltaX;
            } else {
                currentCoord.z += stepZ;
                tMaxZ += tDeltaZ;
            }
        } else {
            if (tMaxY < tMaxZ) {
                currentCoord.y += stepY;
                tMaxY += tDeltaY;
            } else {
                currentCoord.z += stepZ;
                tMaxZ += tDeltaZ;
            }
        }
    }

    return false;
}

} // namespace Meridian