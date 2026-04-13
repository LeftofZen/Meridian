#pragma once

#include "world/WorldData.hpp"

#include <cstdint>
#include <vector>

namespace Meridian {

struct WorldChunkRenderData {
    ChunkCoord coord;
    std::uint32_t voxelResolution{0};
    std::vector<std::uint32_t> materialIds;
};

} // namespace Meridian