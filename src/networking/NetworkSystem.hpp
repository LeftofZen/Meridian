#pragma once

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace Meridian {

class NetworkSystem {
public:
    NetworkSystem() = default;
    ~NetworkSystem();

    NetworkSystem(const NetworkSystem&) = delete;
    NetworkSystem& operator=(const NetworkSystem&) = delete;
    NetworkSystem(NetworkSystem&&) = delete;
    NetworkSystem& operator=(NetworkSystem&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

    [[nodiscard]] ISteamNetworkingSockets* getSockets() const noexcept { return m_sockets; }
    [[nodiscard]] ISteamNetworkingUtils* getUtils() const noexcept { return m_utils; }

private:
    ISteamNetworkingSockets* m_sockets{nullptr};
    ISteamNetworkingUtils* m_utils{nullptr};
    bool m_initialized{false};
};

} // namespace Meridian
