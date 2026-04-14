#pragma once

#include "renderer/CameraState.hpp"
#include "world/WorldData.hpp"

#include <unordered_set>

namespace Meridian {

class ChunkGenerationVisibility final {
public:
    [[nodiscard]] bool canGenerateChunk(
        ChunkCoord coord,
        const CameraRenderState& cameraState,
        const std::unordered_set<ChunkKey>& solidOccluderKeys) const noexcept;

private:
    [[nodiscard]] bool isChunkVisible(ChunkCoord coord, const CameraRenderState& cameraState) const noexcept;
    [[nodiscard]] bool isChunkOccluded(
        ChunkCoord coord,
        const CameraRenderState& cameraState,
        const std::unordered_set<ChunkKey>& solidOccluderKeys) const noexcept;
};

} // namespace Meridian