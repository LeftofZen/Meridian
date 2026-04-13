#include "core/Engine.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>

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

    // ── World ─────────────────────────────────────────────────────────────
    m_world = std::make_unique<World>();
    if (!m_world->init()) {
        MRD_CRITICAL("World init failed");
        return false;
    }
    MRD_INFO("[OK] World");

    MRD_INFO("=== All systems operational ===");
    return true;
}

void Engine::run()
{
    MRD_INFO("Entering main loop (press ESC or close window to exit)");

    const float performanceFrequency = static_cast<float>(SDL_GetPerformanceFrequency());
    Uint64 previousCounter = SDL_GetPerformanceCounter();

    while (!m_window->shouldClose()) {
        const Uint64 currentCounter = SDL_GetPerformanceCounter();
        const float deltaTimeSeconds =
            static_cast<float>(currentCounter - previousCounter) / performanceFrequency;
        previousCounter = currentCounter;

        const std::array<ISystem*, 9> systems{
            m_window.get(),
            m_audio.get(),
            m_physics.get(),
            m_ecs.get(),
            m_network.get(),
            m_scripting.get(),
            m_tasks.get(),
            m_world.get(),
            m_vulkan.get()};

        m_vulkan->setFrameStats(
            m_systemFrameStats,
            m_lastFrameDeltaMilliseconds,
            m_lastFrameCpuMilliseconds);

        const Uint64 cpuFrameStartCounter = SDL_GetPerformanceCounter();

        for (std::size_t index = 0; index < systems.size(); ++index) {
            ISystem* system = systems[index];
            if (system != nullptr) {
                const Uint64 updateStartCounter = SDL_GetPerformanceCounter();
                system->update(deltaTimeSeconds);
                const Uint64 updateEndCounter = SDL_GetPerformanceCounter();
                m_systemFrameStats[index].updateTimeMilliseconds =
                    static_cast<float>(updateEndCounter - updateStartCounter) * 1000.0F /
                    performanceFrequency;
            }
        }

        const Uint64 cpuFrameEndCounter = SDL_GetPerformanceCounter();
        m_lastFrameDeltaMilliseconds = deltaTimeSeconds * 1000.0F;
        m_lastFrameCpuMilliseconds =
            static_cast<float>(cpuFrameEndCounter - cpuFrameStartCounter) * 1000.0F /
            performanceFrequency;

        if (!m_window->shouldClose()) {
            SDL_Delay(1);
        }
    }
}

void Engine::shutdown()
{
    // Guard against double-shutdown (destructor also calls this)
    if (!m_world && !m_scripting && !m_network && !m_ecs && !m_physics &&
        !m_audio && !m_vulkan && !m_window && !m_tasks) {
        return;
    }

    MRD_INFO("=== Meridian engine shutting down ===");

    // Reset in reverse init order; each destructor calls its own shutdown()
    m_world.reset();
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
