#pragma once

#include "audio/AudioSystem.hpp"
#include "core/Logger.hpp"
#include "ecs/ECSSystem.hpp"
#include "networking/NetworkSystem.hpp"
#include "physics/PhysicsSystem.hpp"
#include "renderer/VulkanContext.hpp"
#include "scripting/ScriptingSystem.hpp"
#include "tasks/TaskSystem.hpp"
#include "window/Window.hpp"
#include "world/World.hpp"

#include <memory>

namespace Meridian {

class Engine {
public:
    Engine() = default;
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    /// Initialise all engine subsystems. Returns false on any critical failure.
    [[nodiscard]] bool init();

    /// Run the main event/update loop until the window is closed.
    void run();

    /// Tear down all subsystems in reverse init order.
    void shutdown();

    // ── subsystem accessors ───────────────────────────────────────────────
    [[nodiscard]] Window& getWindow() noexcept { return *m_window; }
    [[nodiscard]] VulkanContext& getRenderer() noexcept { return *m_vulkan; }
    [[nodiscard]] AudioSystem& getAudio() noexcept { return *m_audio; }
    [[nodiscard]] PhysicsSystem& getPhysics() noexcept { return *m_physics; }
    [[nodiscard]] ECSSystem& getECS() noexcept { return *m_ecs; }
    [[nodiscard]] NetworkSystem& getNetwork() noexcept { return *m_network; }
    [[nodiscard]] ScriptingSystem& getScripting() noexcept { return *m_scripting; }
    [[nodiscard]] TaskSystem& getTasks() noexcept { return *m_tasks; }
    [[nodiscard]] World& getWorld() noexcept { return *m_world; }

private:
    std::unique_ptr<Window> m_window;
    std::unique_ptr<VulkanContext> m_vulkan;
    std::unique_ptr<AudioSystem> m_audio;
    std::unique_ptr<PhysicsSystem> m_physics;
    std::unique_ptr<ECSSystem> m_ecs;
    std::unique_ptr<NetworkSystem> m_network;
    std::unique_ptr<ScriptingSystem> m_scripting;
    std::unique_ptr<TaskSystem> m_tasks;
    std::unique_ptr<World> m_world;
};

} // namespace Meridian
