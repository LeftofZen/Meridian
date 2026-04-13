#pragma once

#include "world/WorldChunkStorage.hpp"

#include <cstddef>
#include <unordered_map>

namespace Meridian {

class WorldSpatialHashGrid final {
public:
    void upsert(WorldChunkStorage chunkStorage);
    void erase(ChunkKey key) noexcept;
    [[nodiscard]] bool contains(ChunkKey key) const noexcept;
    [[nodiscard]] WorldChunkStorage* find(ChunkKey key) noexcept;
    [[nodiscard]] const WorldChunkStorage* find(ChunkKey key) const noexcept;
    [[nodiscard]] const std::unordered_map<ChunkKey, WorldChunkStorage>& chunks() const noexcept
    {
        return m_chunks;
    }
    [[nodiscard]] std::size_t size() const noexcept { return m_chunks.size(); }
    void clear() noexcept { m_chunks.clear(); }

private:
    std::unordered_map<ChunkKey, WorldChunkStorage> m_chunks;
};

} // namespace Meridian