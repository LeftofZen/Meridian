#include "world/World.hpp"

#include "core/Logger.hpp"

namespace Meridian {

World::~World()
{
    shutdown();
}

bool World::init()
{
    m_initialised = true;
    MRD_INFO("World system initialized");
    return true;
}

void World::shutdown()
{
    if (m_initialised) {
        m_initialised = false;
        MRD_INFO("World system shutting down");
    }
}

void World::update(float /*deltaTimeSeconds*/)
{
    if (!m_initialised) {
        return;
    }
}

} // namespace Meridian
