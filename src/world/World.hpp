#pragma once

#include "core/ISystem.hpp"
#include "renderer/CameraState.hpp"
#include "world/WorldRenderData.hpp"
#include "world/TerrainHeightmapGenerator.hpp"
#include "world/WorldChunkManager.hpp"

#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace Meridian {

class TaskSystem;
class VulkanContext;

class World final : public ISystem {
public:
    World() = default;
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    void setTaskSystem(TaskSystem& taskSystem) noexcept { m_tasks = &taskSystem; }
    void setVulkanContext(VulkanContext& vulkanContext) noexcept { m_vulkanContext = &vulkanContext; }

    [[nodiscard]] bool init();
    void shutdown();
    void update(float deltaTimeSeconds) override;

    [[nodiscard]] std::size_t getResidentChunkCount() const noexcept;
    [[nodiscard]] std::size_t getInFlightChunkCount() const noexcept;
    [[nodiscard]] std::size_t getPendingChunkCount() const noexcept;
    [[nodiscard]] std::uint64_t getRenderRevision() const noexcept;
    [[nodiscard]] std::vector<WorldChunkRenderData> buildRenderData() const;

    [[nodiscard]] TerrainHeightmapSettings terrainSettings() const;
    void requestTerrainSettings(TerrainHeightmapSettings settings);
    void setStreamingCamera(const CameraRenderState& cameraState) noexcept;
    void setChunkGenerationDistanceChunks(float generationDistanceChunks) noexcept;

private:
    struct Snapshot {
        std::size_t residentChunkCount{0};
        std::size_t inFlightChunkCount{0};
        std::size_t pendingChunkCount{0};
        std::uint64_t renderRevision{0};
        TerrainHeightmapSettings terrainSettings{};
        std::vector<WorldChunkRenderData> renderData;
    };

    void workerMain(std::promise<void> readySignal);
    void publishSnapshot();

    TaskSystem* m_tasks{nullptr};
    VulkanContext* m_vulkanContext{nullptr};
    bool m_initialised{false};
    std::unique_ptr<TerrainHeightmapGenerator> m_heightmapGenerator;
    std::unique_ptr<WorldChunkManager> m_chunkManager;
    mutable std::mutex m_snapshotMutex;
    Snapshot m_snapshot;
    std::mutex m_commandMutex;
    std::condition_variable m_commandCondition;
    std::thread m_workerThread;
    std::optional<TerrainHeightmapSettings> m_pendingTerrainSettings;
    std::optional<CameraRenderState> m_pendingStreamingCamera;
    std::optional<float> m_pendingGenerationDistanceChunks;
    bool m_tickRequested{false};
    bool m_stopWorker{false};
};

} // namespace Meridian
