#pragma once

#include <entt/entt.hpp>

namespace Meridian {

class ECSSystem {
public:
    ECSSystem() = default;
    ~ECSSystem() = default;

    ECSSystem(const ECSSystem&) = delete;
    ECSSystem& operator=(const ECSSystem&) = delete;
    ECSSystem(ECSSystem&&) = delete;
    ECSSystem& operator=(ECSSystem&&) = delete;

    [[nodiscard]] bool init() { return true; }

    void shutdown() { m_registry.clear(); }

    [[nodiscard]] entt::registry& getRegistry() noexcept { return m_registry; }
    [[nodiscard]] const entt::registry& getRegistry() const noexcept { return m_registry; }

    [[nodiscard]] entt::entity createEntity() { return m_registry.create(); }

    void destroyEntity(entt::entity entity) { m_registry.destroy(entity); }

private:
    entt::registry m_registry;
};

} // namespace Meridian
