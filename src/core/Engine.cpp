#include "core/Engine.hpp"

#include <SDL3/SDL.h>

namespace Meridian {

Engine::~Engine()
{
    shutdown();
}

bool Engine::init()
{
    Logger::init();
    MRD_INFO("=== Meridian engine starting ===");

    // ── Task system (used by physics and other subsystems) ────────────────
    m_tasks = std::make_unique<TaskSystem>();
    if (!m_tasks->init()) {
        MRD_CRITICAL("TaskSystem init failed");
        return false;
    }
    MRD_INFO("[OK] TaskSystem");

    // ── Window ────────────────────────────────────────────────────────────
    m_window = std::make_unique<Window>(WindowConfig{.title = "Meridian", .width = 1280, .height = 720});
    if (!m_window->init()) {
        MRD_CRITICAL("Window init failed");
        return false;
    }
    MRD_INFO("[OK] Window");

    // ── Vulkan renderer ───────────────────────────────────────────────────
    m_vulkan = std::make_unique<VulkanContext>(
        VulkanContextConfig{.appName = "Meridian", .enableValidation = true});
    if (!m_vulkan->init(m_window->getHandle())) {
        MRD_CRITICAL("VulkanContext init failed");
        return false;
    }
    MRD_INFO("[OK] VulkanContext (compute support: {})",
        m_vulkan->hasComputeSupport() ? "yes" : "no");

    // ── Audio ─────────────────────────────────────────────────────────────
    m_audio = std::make_unique<AudioSystem>();
    if (!m_audio->init()) {
        MRD_CRITICAL("AudioSystem init failed");
        return false;
    }
    MRD_INFO("[OK] AudioSystem");

    // ── Physics ───────────────────────────────────────────────────────────
    m_physics = std::make_unique<PhysicsSystem>();
    if (!m_physics->init()) {
        MRD_CRITICAL("PhysicsSystem init failed");
        return false;
    }
    MRD_INFO("[OK] PhysicsSystem");

    // ── ECS ───────────────────────────────────────────────────────────────
    m_ecs = std::make_unique<ECSSystem>();
    if (!m_ecs->init()) {
        MRD_CRITICAL("ECSSystem init failed");
        return false;
    }
    MRD_INFO("[OK] ECSSystem");

    // ── Networking ────────────────────────────────────────────────────────
    m_network = std::make_unique<NetworkSystem>();
    if (!m_network->init()) {
        MRD_CRITICAL("NetworkSystem init failed");
        return false;
    }
    MRD_INFO("[OK] NetworkSystem");

    // ── Scripting ─────────────────────────────────────────────────────────
    m_scripting = std::make_unique<ScriptingSystem>();
    if (!m_scripting->init()) {
        MRD_CRITICAL("ScriptingSystem init failed");
        return false;
    }
    MRD_INFO("[OK] ScriptingSystem");

    MRD_INFO("=== All systems operational ===");
    return true;
}

void Engine::run()
{
    MRD_INFO("Entering main loop (press ESC or close window to exit)");

    while (!m_window->shouldClose()) {
        m_window->processEvents();
        if (!m_window->shouldClose()) {
            SDL_Delay(1);
        }
    }
}

void Engine::shutdown()
{
    // Guard against double-shutdown (destructor also calls this)
    if (!m_scripting && !m_network && !m_ecs && !m_physics &&
        !m_audio && !m_vulkan && !m_window && !m_tasks) {
        return;
    }

    MRD_INFO("=== Meridian engine shutting down ===");

    // Reset in reverse init order; each destructor calls its own shutdown()
    m_scripting.reset();
    m_network.reset();
    m_ecs.reset();
    m_physics.reset();
    m_audio.reset();
    m_vulkan.reset();
    m_window.reset();
    m_tasks.reset();

    MRD_INFO("=== Shutdown complete ===");
}

} // namespace Meridian
