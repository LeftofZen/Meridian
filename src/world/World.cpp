#include "world/World.hpp"

#include "core/Logger.hpp"
#include "world/WorldChunkManager.hpp"

#include "tasks/TaskSystem.hpp"

#include <future>
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

    std::promise<void> readyPromise;
    std::future<void> readyFuture = readyPromise.get_future();
    try {
        m_workerThread = std::thread(&World::workerMain, this, std::move(readyPromise));
    } catch (const std::system_error& error) {
        MRD_ERROR("World init failed: could not start world worker thread: {}", error.what());
        m_chunkManager->shutdown();
        m_chunkManager.reset();
        m_heightmapGenerator->shutdown();
        m_heightmapGenerator.reset();
        return false;
    }

    readyFuture.get();

    m_initialised = true;
    MRD_INFO("World system initialized");
    return true;
}

void World::shutdown()
{
    if (m_initialised) {
        {
            std::scoped_lock lock(m_commandMutex);
            m_stopWorker = true;
            m_tickRequested = true;
        }
        m_commandCondition.notify_one();
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
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
    (void)deltaTimeSeconds;

    if (!m_initialised) {
        return;
    }

    {
        std::scoped_lock lock(m_commandMutex);
        m_tickRequested = true;
    }
    m_commandCondition.notify_one();
}

std::size_t World::getResidentChunkCount() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.residentChunkCount;
}

std::size_t World::getInFlightChunkCount() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.inFlightChunkCount;
}

std::size_t World::getPendingChunkCount() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.pendingChunkCount;
}

std::uint64_t World::getRenderRevision() const noexcept
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.renderRevision;
}

std::vector<WorldChunkRenderData> World::buildRenderData() const
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.renderData;
}

void World::setStreamingCamera(const CameraRenderState& cameraState) noexcept
{
    if (!m_initialised) {
        return;
    }

    {
        std::scoped_lock lock(m_commandMutex);
        m_pendingStreamingCamera = cameraState;
    }
    m_commandCondition.notify_one();
}

void World::setChunkGenerationDistanceChunks(float generationDistanceChunks) noexcept
{
    if (!m_initialised) {
        return;
    }

    {
        std::scoped_lock lock(m_commandMutex);
        m_pendingGenerationDistanceChunks = generationDistanceChunks;
    }
    m_commandCondition.notify_one();
}

TerrainHeightmapSettings World::terrainSettings() const
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.terrainSettings;
}

std::vector<std::shared_ptr<const TerrainHeightmapTile>> World::terrainHeightmapTiles() const
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot.terrainHeightmapTiles;
}

void World::requestTerrainSettings(TerrainHeightmapSettings settings)
{
    settings.clamp();
    if (!m_initialised) {
        return;
    }

    {
        std::scoped_lock lock(m_commandMutex);
        m_pendingTerrainSettings = settings;
    }
    m_commandCondition.notify_one();
}

void World::workerMain(std::promise<void> readySignal)
{
    tracy::SetThreadName("World API Worker");
    publishSnapshot();
    readySignal.set_value();

    for (;;) {
        bool stopWorker = false;
        bool tickRequested = false;
        std::optional<TerrainHeightmapSettings> pendingTerrainSettings;
        std::optional<CameraRenderState> pendingStreamingCamera;
        std::optional<float> pendingGenerationDistanceChunks;

        {
            std::unique_lock lock(m_commandMutex);
            m_commandCondition.wait_for(lock, std::chrono::milliseconds(16), [this]() {
                return m_stopWorker ||
                    m_tickRequested ||
                    m_pendingTerrainSettings.has_value() ||
                    m_pendingStreamingCamera.has_value() ||
                    m_pendingGenerationDistanceChunks.has_value();
            });

            stopWorker = m_stopWorker;
            tickRequested = m_tickRequested;
            m_tickRequested = false;
            pendingTerrainSettings = m_pendingTerrainSettings;
            m_pendingTerrainSettings.reset();
            pendingStreamingCamera = m_pendingStreamingCamera;
            m_pendingStreamingCamera.reset();
            pendingGenerationDistanceChunks = m_pendingGenerationDistanceChunks;
            m_pendingGenerationDistanceChunks.reset();
        }

        if (pendingGenerationDistanceChunks.has_value() && m_chunkManager) {
            m_chunkManager->setChunkGenerationDistanceChunks(*pendingGenerationDistanceChunks);
        }
        if (pendingStreamingCamera.has_value() && m_chunkManager) {
            m_chunkManager->setStreamingCamera(*pendingStreamingCamera);
        }
        if (pendingTerrainSettings.has_value()) {
            if (m_heightmapGenerator) {
                m_heightmapGenerator->setSettings(*pendingTerrainSettings);
            }
            if (m_chunkManager) {
                m_chunkManager->rebuildActiveTerrain();
            }
        }
        if ((tickRequested || pendingTerrainSettings.has_value()) && m_chunkManager) {
            m_chunkManager->update(0.0F);
        }

        publishSnapshot();

        if (stopWorker) {
            return;
        }
    }
}

void World::publishSnapshot()
{
    Snapshot snapshot{};
    if (m_chunkManager) {
        snapshot.residentChunkCount = m_chunkManager->getResidentChunkCount();
        snapshot.inFlightChunkCount = m_chunkManager->getInFlightChunkCount();
        snapshot.pendingChunkCount = m_chunkManager->getPendingChunkCount();
        snapshot.renderRevision = m_chunkManager->renderRevision();
        snapshot.renderData = m_chunkManager->buildRenderData();
    }
    if (m_heightmapGenerator) {
        snapshot.terrainSettings = m_heightmapGenerator->settings();
        snapshot.terrainHeightmapTiles = m_heightmapGenerator->cachedTiles();
    }

    std::scoped_lock lock(m_snapshotMutex);
    m_snapshot = std::move(snapshot);
}

} // namespace Meridian
