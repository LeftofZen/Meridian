#pragma once

#include "world/TerrainHeightmapGenerator.hpp"
#include "world/WorldChunkStorage.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>

struct MDB_env;
using MDB_dbi = unsigned int;

namespace Meridian {

class WorldChunkDatabase final {
public:
    WorldChunkDatabase() = default;
    ~WorldChunkDatabase();

    WorldChunkDatabase(const WorldChunkDatabase&) = delete;
    WorldChunkDatabase& operator=(const WorldChunkDatabase&) = delete;
    WorldChunkDatabase(WorldChunkDatabase&&) = delete;
    WorldChunkDatabase& operator=(WorldChunkDatabase&&) = delete;

    [[nodiscard]] bool init(const std::filesystem::path& databasePath);
    void shutdown() noexcept;

    [[nodiscard]] std::optional<WorldChunkStorage> loadChunk(
        ChunkCoord coord,
        ChunkKey key,
        std::uint64_t terrainSettingsSignature) const;
    [[nodiscard]] bool storeChunk(
        const WorldChunkStorage& chunkStorage,
        std::uint64_t terrainSettingsSignature);

private:
    [[nodiscard]] bool isInitialised() const noexcept;

    mutable std::mutex m_mutex;
    MDB_env* m_env{nullptr};
    MDB_dbi m_chunkDatabaseHandle{0};
    bool m_initialised{false};
};

} // namespace Meridian