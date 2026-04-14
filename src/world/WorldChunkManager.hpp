#pragma once

#include "renderer/CameraState.hpp"
#include "world/WorldData.hpp"
#include "world/TerrainHeightmapGenerator.hpp"
#include "world/WorldRenderData.hpp"
#include "world/WorldSpatialHashGrid.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Meridian {

class TaskSystem;
class WorldChunkDatabase;

class WorldChunkManager final {
public:
    WorldChunkManager(TaskSystem& taskSystem, TerrainHeightmapGenerator* heightmapGenerator) noexcept;
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
    [[nodiscard]] std::uint64_t renderRevision() const noexcept { return m_renderRevision; }
    [[nodiscard]] std::vector<WorldChunkRenderData> buildRenderData() const;

    void setStreamingCamera(const CameraRenderState& cameraState) noexcept;
    void setRenderDistanceChunks(float renderDistanceChunks) noexcept;
    void setChunkGenerationDistanceChunks(float generationDistanceChunks) noexcept;
    void rebuildActiveTerrain();

private:
    enum class ChunkStatus {
        Requested,
        Generating,
        Missing,
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

    void queueChunksAroundFocus();
    void requestChunk(ChunkCoord coord);
    void dispatchChunkJobs();
    void collectCompletedJobs();
    void pruneChunksOutsideRetention();
    void persistResidentChunks();
    void persistResidentChunk(const WorldChunkStorage& chunkStorage);
    void cacheResidentChunkRenderData(const WorldChunkStorage& chunkStorage);
    [[nodiscard]] bool shouldKeepChunk(ChunkCoord coord) const noexcept;
    [[nodiscard]] bool shouldRequestChunk(ChunkCoord coord) const noexcept;
    [[nodiscard]] bool shouldGenerateChunk(ChunkCoord coord) const noexcept;
    [[nodiscard]] int renderRadiusXZ() const noexcept;
    [[nodiscard]] int generationRadiusXZ() const noexcept;
    [[nodiscard]] int residentRadiusXZ() const noexcept;
    [[nodiscard]] int retentionRadiusXZ() const noexcept;
    [[nodiscard]] static WorldChunkRenderData createRenderData(const WorldChunkStorage& chunkStorage);

    [[nodiscard]] static GeneratedChunk generateChunk(
        ChunkCoord coord,
        TerrainHeightmapTile heightmapTile,
        TerrainHeightmapSettings heightmapSettings);
    [[nodiscard]] static std::vector<VoxelSample> generateHeightmapChunkVoxels(
        ChunkCoord coord,
        const TerrainHeightmapTile& heightmapTile,
        const TerrainHeightmapSettings& heightmapSettings);
    [[nodiscard]] static std::vector<VoxelSample> generateDefaultChunkVoxels(ChunkCoord coord);

    static constexpr std::size_t kMaxConcurrentJobs{2};
    static constexpr int kStreamChunksBelowFocus{3};
    static constexpr int kStreamChunksAboveFocus{1};
    static constexpr int kRetentionChunksBelowFocus{4};
    static constexpr int kRetentionChunksAboveFocus{2};
    static constexpr float kDefaultRenderDistanceChunks{8.0F};
    static constexpr float kDefaultGenerationDistanceChunks{8.0F};
    static constexpr float kRetentionPaddingChunks{1.0F};

    TaskSystem& m_tasks;
    TerrainHeightmapGenerator* m_heightmapGenerator{nullptr};
    std::shared_ptr<WorldChunkDatabase> m_chunkDatabase;
    bool m_initialised{false};
    bool m_hasStreamingCamera{false};
    ChunkCoord m_streamingFocusChunk{};
    float m_renderDistanceChunks{kDefaultRenderDistanceChunks};
    float m_generationDistanceChunks{kDefaultGenerationDistanceChunks};
    std::deque<ChunkCoord> m_pendingRequests;
    std::vector<ChunkJob> m_inFlightJobs;
    std::unordered_map<ChunkKey, ChunkRecord> m_chunkRecords;
    std::unordered_map<ChunkKey, TerrainHeightmapTile> m_heightmapTiles;
    std::unordered_map<ChunkKey, WorldChunkRenderData> m_renderChunkData;
    WorldSpatialHashGrid m_residentChunks;
    std::uint64_t m_renderRevision{0};
};

} // namespace Meridian