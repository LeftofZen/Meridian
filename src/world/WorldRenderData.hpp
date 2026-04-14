#pragma once

#include "world/WorldData.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace Meridian {

struct WorldChunkRenderData {
    ChunkCoord coord;
    std::uint32_t voxelResolution{0};
    std::shared_ptr<const std::vector<std::uint32_t>> packedOctreeNodes;

    [[nodiscard]] std::size_t octreeNodeCount() const noexcept
    {
        return packedOctreeNodes != nullptr ? packedOctreeNodes->size() / 9U : 0U;
    }
};

} // namespace Meridian