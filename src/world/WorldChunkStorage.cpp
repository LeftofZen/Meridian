#include "world/WorldChunkStorage.hpp"

namespace Meridian {

WorldChunkStorage::WorldChunkStorage(
    ChunkCoord coord,
    ChunkKey key,
    std::uint32_t voxelResolution,
    std::vector<VoxelSample> voxels,
    SparseVoxelOctree octree) noexcept
    : m_coord(coord),
      m_key(key),
      m_voxelResolution(voxelResolution),
      m_voxels(std::move(voxels)),
      m_octree(std::move(octree))
{
}

} // namespace Meridian