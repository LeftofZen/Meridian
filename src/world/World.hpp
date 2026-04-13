#pragma once

namespace Meridian {

class World {
public:
    World() = default;
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

private:
    bool m_initialised = false;
};

} // namespace Meridian
