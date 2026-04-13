#pragma once

#include "world/WorldData.hpp"
#include "world/WorldRenderData.hpp"
#include "world/WorldSpatialHashGrid.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <unordered_map>
#include <vector>

namespace Meridian {

class TaskSystem;

class WorldChunkManager final {
public:
    explicit WorldChunkManager(TaskSystem& taskSystem) noexcept;
    ~WorldChunkManager() = default;

    WorldChunkManager(const WorldChunkManager&) = delete;
    WorldChunkManager& operator=(const WorldChunkManager&) = delete;
    WorldChunkManager(WorldChunkManager&&) = delete;
    WorldChunkManager& operator=(WorldChunkManager&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();
    void update(float deltaTimeSeconds);

    [[nodiscard]] std::size_t getResidentChunkCount() const noexcept;
    [[nodiscard]] std::size_t getInFlightChunkCount() const noexcept;
    [[nodiscard]] std::size_t getPendingChunkCount() const noexcept;
    [[nodiscard]] std::vector<WorldChunkRenderData> buildRenderData() const;

private:
    enum class ChunkStatus {
        Requested,
        Generating,
        Resident,
    };

    struct ChunkRecord {
        ChunkCoord coord;
        ChunkKey key{0};
        ChunkStatus status{ChunkStatus::Requested};
    };

    struct GeneratedChunk {
        WorldChunkStorage chunkStorage;
    };

    struct ChunkJob {
        ChunkCoord coord;
        ChunkKey key{0};
        std::future<GeneratedChunk> future;
    };

    void bootstrapChunkRequests();
    void requestChunk(ChunkCoord coord);
    void dispatchChunkJobs();
    void collectCompletedJobs();

    [[nodiscard]] static GeneratedChunk generateChunk(
        ChunkCoord coord);
    [[nodiscard]] static std::vector<VoxelSample> generateDefaultChunkVoxels(ChunkCoord coord);

    static constexpr int kBootstrapRadius{1};
    static constexpr std::size_t kMaxConcurrentJobs{2};

    TaskSystem& m_tasks;
    bool m_initialised{false};
    bool m_bootstrapRequested{false};
    std::deque<ChunkCoord> m_pendingRequests;
    std::vector<ChunkJob> m_inFlightJobs;
    std::unordered_map<ChunkKey, ChunkRecord> m_chunkRecords;
    WorldSpatialHashGrid m_residentChunks;
};

} // namespace Meridian