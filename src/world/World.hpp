#pragma once

#include "core/ISystem.hpp"
#include "renderer/VulkanContext.hpp"
#include "world/WorldChunkManager.hpp"
#include "world/WorldRenderData.hpp"

#include "world/TerrainHeightmapGenerator.hpp"

#include <cstddef>
#include <mutex>
#include <memory>
#include <optional>

namespace Meridian {

class TaskSystem;

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

private:
    void applyPendingTerrainSettings();

    TaskSystem* m_tasks{nullptr};
    VulkanContext* m_vulkanContext{nullptr};
    bool m_initialised{false};
    std::unique_ptr<TerrainHeightmapGenerator> m_heightmapGenerator;
    std::unique_ptr<WorldChunkManager> m_chunkManager;
    mutable std::mutex m_terrainSettingsMutex;
    std::optional<TerrainHeightmapSettings> m_pendingTerrainSettings;
};

} // namespace Meridian
