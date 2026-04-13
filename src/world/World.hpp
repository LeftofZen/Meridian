#pragma once

#include "core/ISystem.hpp"

namespace Meridian {

class World final : public ISystem {
public:
    World() = default;
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();
    void update(float deltaTimeSeconds) override;

private:
    bool m_initialised = false;
};

} // namespace Meridian
