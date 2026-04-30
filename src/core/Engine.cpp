#include "core/Engine.hpp"

#include <SDL3/SDL.h>
#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace Meridian {

namespace {

constexpr std::array<std::string_view, 10> kSystemNames{
    "Window",
    "Input",
    "Camera",
    "Audio",
    "Physics",
    "ECS",
    "Networking",
    "Scripting",
    "Tasks",
    "World",
};

[[nodiscard]] CameraRenderState buildCameraRenderState(
    const FreeCameraController& cameraController,
    const VulkanContext* vulkanContext) noexcept
{
    CameraRenderState cameraState = cameraController.cameraState();
    if (vulkanContext == nullptr) {
        return cameraState;
    }

    const VkExtent2D swapchainExtent = vulkanContext->getSwapchainExtent();
    if (swapchainExtent.width > 0 && swapchainExtent.height > 0) {
        cameraState.projection.aspectRatio =
            static_cast<float>(swapchainExtent.width) /
            static_cast<float>(std::max(swapchainExtent.height, 1U));
    }

    return cameraState;
}

[[nodiscard]] std::array<float, 3> normalizeDirection(std::array<float, 3> value) noexcept
{
    const float lengthSquared =
        value[0] * value[0] + value[1] * value[1] + value[2] * value[2];
    if (lengthSquared <= 0.0F) {
        return {0.0F, 1.0F, 0.0F};
    }

    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    return {
        value[0] * inverseLength,
        value[1] * inverseLength,
        value[2] * inverseLength,
    };
}

[[nodiscard]] LightingRenderSnapshot buildDefaultLightingState()
{
    LightingRenderSnapshot lighting{};
    lighting.sun.direction = normalizeDirection({-0.35F, 0.82F, -0.28F});
    lighting.sun.color = {1.0F, 0.95F, 0.84F};
    lighting.sun.intensity = 4.5F;

    lighting.pointLights = {
        PointLightRenderState{
            .positionMeters = {36.0F, 34.0F, 84.0F},
            .color = {1.0F, 0.58F, 0.32F},
            .intensity = 2600.0F,
            .rangeMeters = 42.0F,
        },
        PointLightRenderState{
            .positionMeters = {74.0F, 28.0F, 58.0F},
            .color = {0.35F, 0.62F, 1.0F},
            .intensity = 2100.0F,
            .rangeMeters = 36.0F,
        },
    };

    lighting.areaLights = {
        AreaLightRenderState{
            .centerMeters = {54.0F, 42.0F, 72.0F},
            .rightExtentMeters = {5.0F, 0.0F, 0.0F},
            .upExtentMeters = {0.0F, 0.0F, 3.0F},
            .color = {1.0F, 0.94F, 0.82F},
            .intensity = 6.0F,
            .doubleSided = false,
        },
    };

    return lighting;
}

} // namespace

void Engine::setUpdateRateLimit(float updatesPerSecond) noexcept
{
    m_updateRateLimit = std::clamp(updatesPerSecond, 1.0F, 1000.0F);
}

void Engine::startRenderLoop()
{
    if (m_renderLoopRunning.exchange(true) || !m_vulkan) {
        return;
    }

    m_renderThread = std::thread([this]() {
        tracy::SetThreadName("Render Thread");
        const float performanceFrequency = static_cast<float>(SDL_GetPerformanceFrequency());
        Uint64 previousCounter = SDL_GetPerformanceCounter();

        while (m_renderLoopRunning.load(std::memory_order_acquire)) {
            ZoneScopedN("Render Thread Tick");
            const Uint64 currentCounter = SDL_GetPerformanceCounter();
            const float renderDeltaMilliseconds =
                static_cast<float>(currentCounter - previousCounter) * 1000.0F /
                performanceFrequency;
            previousCounter = currentCounter;

            const Uint64 renderCpuStartCounter = SDL_GetPerformanceCounter();
            {
                ZoneScopedN("VulkanContext::render");
                m_vulkan->render();
            }
            const Uint64 renderCpuEndCounter = SDL_GetPerformanceCounter();

            const float renderCpuMilliseconds =
                static_cast<float>(renderCpuEndCounter - renderCpuStartCounter) * 1000.0F /
                performanceFrequency;
            m_renderStateStore.updateRenderStats(renderDeltaMilliseconds, renderCpuMilliseconds);
            FrameMarkNamed("Render");
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

void Engine::limitUpdateRate(std::uint64_t frameStartCounter, float performanceFrequency) const
{
    if (m_updateRateLimit <= 0.0F) {
        return;
    }

    ZoneScopedN("Engine::limitUpdateRate");
    const double targetFrameSeconds = 1.0 / static_cast<double>(m_updateRateLimit);

    for (;;) {
        const Uint64 currentCounter = SDL_GetPerformanceCounter();
        const double elapsedSeconds =
            static_cast<double>(currentCounter - frameStartCounter) /
            static_cast<double>(performanceFrequency);
        const double remainingSeconds = targetFrameSeconds - elapsedSeconds;
        if (remainingSeconds <= 0.0) {
            return;
        }

        if (remainingSeconds > 0.0015) {
            std::this_thread::sleep_for(
                std::chrono::duration<double>(remainingSeconds - 0.0005));
        } else {
            std::this_thread::yield();
        }
    }
}

Engine::~Engine()
{
    shutdown();
}

bool Engine::init()
{
    Logger::init();
    m_cachedWorldRenderRevision = std::numeric_limits<std::uint64_t>::max();
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
    m_renderStateStore.updateLightingState(buildDefaultLightingState());
    m_pathTracerRenderer->setRenderStateStore(m_renderStateStore);
    m_debugOverlay->setRenderStateStore(m_renderStateStore);
    m_debugOverlay->setPathTracerSettings(m_pathTracerRenderer->settings());
    m_debugOverlay->setUpdateRateLimitCallbacks(
        [this]() {
            return updateRateLimit();
        },
        [this](float updatesPerSecond) {
            setUpdateRateLimit(updatesPerSecond);
        });
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
        const CameraRenderState cameraState =
            buildCameraRenderState(*m_freeCameraController, m_vulkan.get());
        m_world->setChunkGenerationDistanceChunks(
            m_renderStateStore.worldChunkGenerationDistanceChunks());
        m_world->setStreamingCamera(cameraState);
        m_renderStateStore.updateCameraState(cameraState);
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
    m_debugOverlay->setTerrainHeightmapTilesCallback([this]() {
        return m_world ? m_world->terrainHeightmapTiles() :
                         std::vector<std::shared_ptr<const TerrainHeightmapTile>>{};
    });
    MRD_INFO("[OK] World");

    MRD_INFO("=== All systems operational ===");
    return true;
}

void Engine::run()
{
    MRD_INFO("Entering main loop (press ESC or close window to exit)");
    tracy::SetThreadName("Main Thread");

    startRenderLoop();

    const float performanceFrequency = static_cast<float>(SDL_GetPerformanceFrequency());
    Uint64 previousCounter = SDL_GetPerformanceCounter();

    while (!m_window->shouldClose()) {
        ZoneScopedN("Main Thread Tick");
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
                {
                    ZoneScopedN("ISystem::update");
                    const std::string_view systemName = kSystemNames[index];
                    ZoneName(systemName.data(), systemName.size());
                    system->update(deltaTimeSeconds);
                    if (system == m_freeCameraController.get() && m_world != nullptr) {
                        const CameraRenderState cameraState =
                            buildCameraRenderState(*m_freeCameraController, m_vulkan.get());
                        m_world->setChunkGenerationDistanceChunks(
                            m_renderStateStore.worldChunkGenerationDistanceChunks());
                        m_world->setStreamingCamera(cameraState);
                        m_renderStateStore.updateCameraState(cameraState);
                    }
                }
            }
        }

        const Uint64 cpuFrameEndCounter = SDL_GetPerformanceCounter();
        const float frameDeltaMilliseconds = deltaTimeSeconds * 1000.0F;
        const float frameCpuMilliseconds =
            static_cast<float>(cpuFrameEndCounter - cpuFrameStartCounter) * 1000.0F /
            performanceFrequency;

        m_renderStateStore.updateUpdateTiming(frameDeltaMilliseconds, frameCpuMilliseconds);
        if (m_world) {
            const std::uint64_t worldRenderRevision = m_world->getRenderRevision();
            if (worldRenderRevision != m_cachedWorldRenderRevision) {
                m_renderStateStore.updateWorldStats(
                    m_world->getResidentChunkCount(),
                    m_world->getInFlightChunkCount(),
                    m_world->getPendingChunkCount(),
                    worldRenderRevision,
                    m_world->buildRenderData());
                m_cachedWorldRenderRevision = worldRenderRevision;
            } else {
                m_renderStateStore.updateWorldStats(
                    m_world->getResidentChunkCount(),
                    m_world->getInFlightChunkCount(),
                    m_world->getPendingChunkCount(),
                    worldRenderRevision);
            }
        }
        FrameMarkNamed("Update");
        limitUpdateRate(currentCounter, performanceFrequency);
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
    m_cachedWorldRenderRevision = std::numeric_limits<std::uint64_t>::max();
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
