#include "networking/NetworkSystem.hpp"

#include "core/Logger.hpp"

namespace Meridian {

NetworkSystem::~NetworkSystem()
{
    shutdown();
}

bool NetworkSystem::init()
{
    SteamDatagramErrMsg errMsg;
    if (!GameNetworkingSockets_Init(nullptr, errMsg)) {
        MRD_ERROR("GameNetworkingSockets_Init failed: {}", errMsg);
        return false;
    }

    m_sockets = SteamNetworkingSockets();
    m_utils = SteamNetworkingUtils();

    if (!m_sockets) {
        MRD_ERROR("SteamNetworkingSockets() returned nullptr");
        GameNetworkingSockets_Kill();
        return false;
    }

    m_initialized = true;
    MRD_INFO("GameNetworkingSockets ready");
    return true;
}

void NetworkSystem::shutdown()
{
    if (m_initialized) {
        GameNetworkingSockets_Kill();
        m_sockets = nullptr;
        m_utils = nullptr;
        m_initialized = false;
    }
}

void NetworkSystem::update(float /*deltaTimeSeconds*/)
{
    if (!m_initialized) {
        return;
    }
}

} // namespace Meridian
