#pragma once

#include "core/ISystem.hpp"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

namespace Meridian {

class NetworkSystem final : public ISystem {
public:
    NetworkSystem() = default;
    ~NetworkSystem();

    NetworkSystem(const NetworkSystem&) = delete;
    NetworkSystem& operator=(const NetworkSystem&) = delete;
    NetworkSystem(NetworkSystem&&) = delete;
    NetworkSystem& operator=(NetworkSystem&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();
    void update(float deltaTimeSeconds) override;

    [[nodiscard]] ISteamNetworkingSockets* getSockets() const noexcept { return m_sockets; }
    [[nodiscard]] ISteamNetworkingUtils* getUtils() const noexcept { return m_utils; }
    [[nodiscard]] bool isInitialised() const noexcept { return m_initialized; }

private:
    ISteamNetworkingSockets* m_sockets{nullptr};
    ISteamNetworkingUtils* m_utils{nullptr};
    bool m_initialized{false};
};

} // namespace Meridian
