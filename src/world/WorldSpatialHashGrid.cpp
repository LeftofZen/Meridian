#include "world/WorldSpatialHashGrid.hpp"

namespace Meridian {

void WorldSpatialHashGrid::upsert(WorldChunkStorage chunkStorage)
{
    const ChunkKey key = chunkStorage.key();
    m_chunks.insert_or_assign(key, std::move(chunkStorage));
}

void WorldSpatialHashGrid::erase(ChunkKey key) noexcept
{
    m_chunks.erase(key);
}

bool WorldSpatialHashGrid::contains(ChunkKey key) const noexcept
{
    return m_chunks.contains(key);
}

WorldChunkStorage* WorldSpatialHashGrid::find(ChunkKey key) noexcept
{
    const auto it = m_chunks.find(key);
    return it != m_chunks.end() ? &it->second : nullptr;
}

const WorldChunkStorage* WorldSpatialHashGrid::find(ChunkKey key) const noexcept
{
    const auto it = m_chunks.find(key);
    return it != m_chunks.end() ? &it->second : nullptr;
}

} // namespace Meridian