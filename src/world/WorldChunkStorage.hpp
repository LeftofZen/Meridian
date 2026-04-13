#pragma once

#include "world/SparseVoxelOctree.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Meridian {

class WorldChunkStorage final {
public:
    WorldChunkStorage(
        ChunkCoord coord,
        ChunkKey key,
        std::uint32_t voxelResolution,
        std::vector<VoxelSample> voxels,
        SparseVoxelOctree octree) noexcept;

    [[nodiscard]] const ChunkCoord& coord() const noexcept { return m_coord; }
    [[nodiscard]] ChunkKey key() const noexcept { return m_key; }
    [[nodiscard]] std::uint32_t voxelResolution() const noexcept { return m_voxelResolution; }
    [[nodiscard]] const std::vector<VoxelSample>& voxels() const noexcept { return m_voxels; }
    [[nodiscard]] const SparseVoxelOctree& octree() const noexcept { return m_octree; }
    [[nodiscard]] std::size_t solidVoxelCount() const noexcept
    {
        return m_octree.stats().solidVoxelCount;
    }

private:
    ChunkCoord m_coord;
    ChunkKey m_key{0};
    std::uint32_t m_voxelResolution{0};
    std::vector<VoxelSample> m_voxels;
    SparseVoxelOctree m_octree;
};

} // namespace Meridian