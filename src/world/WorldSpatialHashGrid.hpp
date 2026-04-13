#pragma once

#include "world/WorldChunkStorage.hpp"

#include <cstddef>
#include <unordered_map>

namespace Meridian {

class WorldSpatialHashGrid final {
public:
    void upsert(WorldChunkStorage chunkStorage);
    [[nodiscard]] bool contains(ChunkKey key) const noexcept;
    [[nodiscard]] WorldChunkStorage* find(ChunkKey key) noexcept;
    [[nodiscard]] const WorldChunkStorage* find(ChunkKey key) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return m_chunks.size(); }
    void clear() noexcept { m_chunks.clear(); }

private:
    std::unordered_map<ChunkKey, WorldChunkStorage> m_chunks;
};

} // namespace Meridian