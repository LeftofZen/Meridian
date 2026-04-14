#include "world/World.hpp"

#include "core/Logger.hpp"
#include "world/WorldChunkManager.hpp"

#include "tasks/TaskSystem.hpp"

#include <tracy/Tracy.hpp>

namespace Meridian {

World::~World()
{
    shutdown();
}

bool World::init()
{
    if (m_tasks == nullptr) {
        MRD_ERROR("World init failed: TaskSystem dependency was not configured");
        return false;
    }

    if (m_vulkanContext == nullptr) {
        MRD_ERROR("World init failed: VulkanContext dependency was not configured");
        return false;
    }

    if (!m_tasks->isInitialised()) {
        MRD_ERROR("World init failed: TaskSystem is not initialised");
        return false;
    }

    m_heightmapGenerator = std::make_unique<TerrainHeightmapGenerator>();
    if (!m_heightmapGenerator->init(*m_vulkanContext)) {
        MRD_ERROR("World init failed: TerrainHeightmapGenerator init failed");
        m_heightmapGenerator.reset();
        return false;
    }

    m_chunkManager = std::make_unique<WorldChunkManager>(*m_tasks, m_heightmapGenerator.get());
    if (!m_chunkManager->init()) {
        MRD_ERROR("World init failed: WorldChunkManager init failed");
        m_chunkManager.reset();
        m_heightmapGenerator.reset();
        return false;
    }

    m_initialised = true;
    MRD_INFO("World system initialized");
    return true;
}

void World::shutdown()
{
    if (m_initialised) {
        if (m_chunkManager) {
            m_chunkManager->shutdown();
            m_chunkManager.reset();
        }
        if (m_heightmapGenerator) {
            m_heightmapGenerator->shutdown();
            m_heightmapGenerator.reset();
        }
        m_initialised = false;
        MRD_INFO("World system shutting down");
    }
}

void World::update(float deltaTimeSeconds)
{
    if (!m_initialised || !m_chunkManager) {
        return;
    }

    ZoneScopedN("World::update");
    applyPendingTerrainSettings();
    m_chunkManager->update(deltaTimeSeconds);
}

std::size_t World::getResidentChunkCount() const noexcept
{
    return m_chunkManager ? m_chunkManager->getResidentChunkCount() : 0;
}

std::size_t World::getInFlightChunkCount() const noexcept
{
    return m_chunkManager ? m_chunkManager->getInFlightChunkCount() : 0;
}

std::size_t World::getPendingChunkCount() const noexcept
{
    return m_chunkManager ? m_chunkManager->getPendingChunkCount() : 0;
}

std::uint64_t World::getRenderRevision() const noexcept
{
    return m_chunkManager ? m_chunkManager->renderRevision() : 0;
}

std::vector<WorldChunkRenderData> World::buildRenderData() const
{
    return m_chunkManager ? m_chunkManager->buildRenderData() : std::vector<WorldChunkRenderData>{};
}

void World::setStreamingCamera(const CameraRenderState& cameraState) noexcept
{
    if (m_chunkManager) {
        m_chunkManager->setStreamingCamera(cameraState);
    }
}

void World::setRenderDistanceChunks(float renderDistanceChunks) noexcept
{
    if (m_chunkManager) {
        m_chunkManager->setRenderDistanceChunks(renderDistanceChunks);
    }
}

void World::setChunkGenerationDistanceChunks(float generationDistanceChunks) noexcept
{
    if (m_chunkManager) {
        m_chunkManager->setChunkGenerationDistanceChunks(generationDistanceChunks);
    }
}

TerrainHeightmapSettings World::terrainSettings() const
{
    if (!m_heightmapGenerator) {
        return TerrainHeightmapSettings{};
    }

    return m_heightmapGenerator->settings();
}

void World::requestTerrainSettings(TerrainHeightmapSettings settings)
{
    settings.clamp();
    std::scoped_lock lock(m_terrainSettingsMutex);
    m_pendingTerrainSettings = settings;
}

void World::applyPendingTerrainSettings()
{
    if (!m_heightmapGenerator || !m_chunkManager) {
        return;
    }

    ZoneScopedN("World::applyPendingTerrainSettings");
    std::optional<TerrainHeightmapSettings> pendingSettings;
    {
        std::scoped_lock lock(m_terrainSettingsMutex);
        if (!m_pendingTerrainSettings.has_value()) {
            return;
        }

        pendingSettings = m_pendingTerrainSettings;
        m_pendingTerrainSettings.reset();
    }

    m_heightmapGenerator->setSettings(*pendingSettings);
    m_chunkManager->rebuildActiveTerrain();
}

} // namespace Meridian
