#pragma once

#include "world/TerrainHeightmapGenerator.hpp"
#include "world/WorldChunkStorage.hpp"

#include <filesystem>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

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
    struct PendingChunkWrite {
        std::uint64_t sequence{0};
        WorldChunkStorage chunkStorage;
        std::uint64_t terrainSettingsSignature{0};
    };

    void databaseWorkerMain();
    [[nodiscard]] bool storeChunkNow(
        const WorldChunkStorage& chunkStorage,
        std::uint64_t terrainSettingsSignature);
    [[nodiscard]] bool isInitialised() const noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_workerCondition;
    std::thread m_workerThread;
    std::deque<std::shared_ptr<PendingChunkWrite>> m_pendingWrites;
    std::unordered_map<ChunkKey, std::shared_ptr<PendingChunkWrite>> m_pendingWriteLookup;
    std::uint64_t m_nextPendingWriteSequence{0};
    bool m_stopWorker{false};
    MDB_env* m_env{nullptr};
    MDB_dbi m_chunkDatabaseHandle{0};
    bool m_initialised{false};
};

} // namespace Meridian