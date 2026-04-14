#pragma once

#include "world/WorldData.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace Meridian {

struct WorldChunkRenderData {
    ChunkCoord coord;
    std::uint32_t voxelResolution{0};
    std::shared_ptr<const std::vector<std::uint32_t>> materialIds;

    [[nodiscard]] std::size_t voxelCount() const noexcept
    {
        return materialIds != nullptr ? materialIds->size() : 0U;
    }
};

} // namespace Meridian