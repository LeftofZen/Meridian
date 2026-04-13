#pragma once

#include "core/ISystem.hpp"
#include "world/WorldChunkManager.hpp"

#include <cstddef>
#include <memory>

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

    [[nodiscard]] bool init();
    void shutdown();
    void update(float deltaTimeSeconds) override;

    [[nodiscard]] std::size_t getResidentChunkCount() const noexcept;
    [[nodiscard]] std::size_t getInFlightChunkCount() const noexcept;
    [[nodiscard]] std::size_t getPendingChunkCount() const noexcept;

private:
    TaskSystem* m_tasks{nullptr};
    bool m_initialised{false};
    std::unique_ptr<WorldChunkManager> m_chunkManager;
};

} // namespace Meridian
