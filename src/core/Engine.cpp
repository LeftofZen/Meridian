#include "core/Engine.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>

namespace Meridian {

void Engine::startRenderLoop()
{
    if (m_renderLoopRunning.exchange(true) || !m_vulkan) {
        return;
    }

    m_renderThread = std::thread([this]() {
        const float performanceFrequency = static_cast<float>(SDL_GetPerformanceFrequency());
        Uint64 previousCounter = SDL_GetPerformanceCounter();

        while (m_renderLoopRunning.load(std::memory_order_acquire)) {
            const Uint64 currentCounter = SDL_GetPerformanceCounter();
            const float renderDeltaMilliseconds =
                static_cast<float>(currentCounter - previousCounter) * 1000.0F /
                performanceFrequency;
            previousCounter = currentCounter;

            const Uint64 renderCpuStartCounter = SDL_GetPerformanceCounter();
            m_vulkan->render();
            const Uint64 renderCpuEndCounter = SDL_GetPerformanceCounter();

            const float renderCpuMilliseconds =
                static_cast<float>(renderCpuEndCounter - renderCpuStartCounter) * 1000.0F /
                performanceFrequency;
            m_renderStateStore.updateRenderStats(renderDeltaMilliseconds, renderCpuMilliseconds);
        }
    });
}

void Engine::stopRenderLoop()
{
    m_renderLoopRunning.store(false, std::memory_order_release);
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }
}

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

    // ── Input and camera ─────────────────────────────────────────────────
    m_inputManager = std::make_unique<InputManager>();
    if (!m_inputManager->init()) {
        MRD_CRITICAL("InputManager init failed");
        return false;
    }
    m_inputManager->attachWindow(m_window->getHandle());
    MRD_INFO("[OK] InputManager");

    m_freeCameraController = std::make_unique<FreeCameraController>(*m_inputManager);
    if (!m_freeCameraController->init()) {
        MRD_CRITICAL("FreeCameraController init failed");
        return false;
    }
    MRD_INFO("[OK] FreeCameraController");

    // ── Vulkan renderer ───────────────────────────────────────────────────
    m_renderPipeline = std::make_unique<RenderFramePipeline>();
    m_pathTracerRenderer = std::make_unique<PathTracerRenderer>();
    m_debugOverlay = std::make_unique<DebugOverlayRenderer>();
    m_worldSceneRenderer = std::make_unique<WorldSceneRenderer>();
    m_vulkan = std::make_unique<VulkanContext>(
        VulkanContextConfig{.appName = "Meridian", .enableValidation = true});
    m_pathTracerRenderer->setRenderStateStore(m_renderStateStore);
    m_debugOverlay->setRenderStateStore(m_renderStateStore);
    m_debugOverlay->setPathTracerSettings(m_pathTracerRenderer->settings());
    m_worldSceneRenderer->setRenderStateStore(m_renderStateStore);
    m_renderPipeline->addFeature(*m_pathTracerRenderer);
    m_renderPipeline->addFeature(*m_worldSceneRenderer);
    m_renderPipeline->addFeature(*m_debugOverlay);
    m_vulkan->setRenderFrontend(m_renderPipeline.get());
    m_window->setEventHandler([this](const SDL_Event& event) {
        if (m_inputManager) {
            m_inputManager->handleEvent(event);
        }
        if (m_renderPipeline) {
            m_renderPipeline->handleEvent(event);
        }
    });
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
    m_world->setTaskSystem(*m_tasks);
    m_world->setVulkanContext(*m_vulkan);
    if (!m_world->init()) {
        MRD_CRITICAL("World init failed");
        return false;
    }
    if (m_freeCameraController) {
        m_world->setStreamingCamera(m_freeCameraController->cameraState());
    }
    m_debugOverlay->setTerrainSettingsCallbacks(
        [this]() {
            return m_world ? m_world->terrainSettings() : TerrainHeightmapSettings{};
        },
        [this](const TerrainHeightmapSettings& terrainSettings) {
            if (m_world) {
                m_world->requestTerrainSettings(terrainSettings);
            }
        });
    MRD_INFO("[OK] World");

    MRD_INFO("=== All systems operational ===");
    return true;
}

void Engine::run()
{
    MRD_INFO("Entering main loop (press ESC or close window to exit)");

    startRenderLoop();

    const float performanceFrequency = static_cast<float>(SDL_GetPerformanceFrequency());
    Uint64 previousCounter = SDL_GetPerformanceCounter();

    while (!m_window->shouldClose()) {
        const Uint64 currentCounter = SDL_GetPerformanceCounter();
        const float deltaTimeSeconds =
            static_cast<float>(currentCounter - previousCounter) / performanceFrequency;
        previousCounter = currentCounter;

        const std::array<ISystem*, 10> systems{
            m_window.get(),
            m_inputManager.get(),
            m_freeCameraController.get(),
            m_audio.get(),
            m_physics.get(),
            m_ecs.get(),
            m_network.get(),
            m_scripting.get(),
            m_tasks.get(),
            m_world.get()};

        const Uint64 cpuFrameStartCounter = SDL_GetPerformanceCounter();

        for (std::size_t index = 0; index < systems.size(); ++index) {
            ISystem* system = systems[index];
            if (system != nullptr) {
                const Uint64 updateStartCounter = SDL_GetPerformanceCounter();
                system->update(deltaTimeSeconds);
                if (system == m_freeCameraController.get() && m_world != nullptr) {
                    m_world->setStreamingCamera(m_freeCameraController->cameraState());
                }
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

        m_renderStateStore.updateUpdateStats(
            m_systemFrameStats,
            m_lastFrameDeltaMilliseconds,
            m_lastFrameCpuMilliseconds);
        if (m_world) {
            m_renderStateStore.updateWorldStats(
                m_world->getResidentChunkCount(),
                m_world->getInFlightChunkCount(),
                m_world->getPendingChunkCount(),
                m_world->getRenderRevision(),
                m_world->buildRenderData());
        }
        if (m_freeCameraController) {
            m_renderStateStore.updateCameraState(m_freeCameraController->cameraState());
        }
    }

    stopRenderLoop();
}

void Engine::shutdown()
{
    // Guard against double-shutdown (destructor also calls this)
    if (!m_world && !m_scripting && !m_network && !m_ecs && !m_physics &&
        !m_audio && !m_vulkan && !m_window && !m_tasks) {
        return;
    }

    MRD_INFO("=== Meridian engine shutting down ===");

    stopRenderLoop();

    // Reset in reverse init order; each destructor calls its own shutdown()
    m_world.reset();
    m_scripting.reset();
    m_network.reset();
    m_ecs.reset();
    m_physics.reset();
    m_audio.reset();
    m_vulkan.reset();
    m_pathTracerRenderer.reset();
    m_freeCameraController.reset();
    m_worldSceneRenderer.reset();
    m_debugOverlay.reset();
    m_renderPipeline.reset();
    m_inputManager.reset();
    m_window.reset();
    m_tasks.reset();

    MRD_INFO("=== Shutdown complete ===");
}

} // namespace Meridian
