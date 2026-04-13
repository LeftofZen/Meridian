#include "world/World.hpp"

#include "core/Logger.hpp"
#include "world/WorldChunkManager.hpp"

#include "tasks/TaskSystem.hpp"

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

    if (!m_tasks->isInitialised()) {
        MRD_ERROR("World init failed: TaskSystem is not initialised");
        return false;
    }

    m_chunkManager = std::make_unique<WorldChunkManager>(*m_tasks);
    if (!m_chunkManager->init()) {
        MRD_ERROR("World init failed: WorldChunkManager init failed");
        m_chunkManager.reset();
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
        m_initialised = false;
        MRD_INFO("World system shutting down");
    }
}

void World::update(float deltaTimeSeconds)
{
    if (!m_initialised || !m_chunkManager) {
        return;
    }

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

} // namespace Meridian
