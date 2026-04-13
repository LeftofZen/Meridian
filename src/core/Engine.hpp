#pragma once

#include "audio/AudioSystem.hpp"
#include "core/Logger.hpp"
#include "core/SystemFrameStats.hpp"
#include "ecs/ECSSystem.hpp"
#include "input/InputManager.hpp"
#include "networking/NetworkSystem.hpp"
#include "physics/PhysicsSystem.hpp"
#include "renderer/DebugOverlayRenderer.hpp"
#include "renderer/FreeCameraController.hpp"
#include "renderer/PathTracerRenderer.hpp"
#include "renderer/RenderFramePipeline.hpp"
#include "renderer/RenderStateStore.hpp"
#include "renderer/VulkanContext.hpp"
#include "renderer/WorldSceneRenderer.hpp"
#include "scripting/ScriptingSystem.hpp"
#include "tasks/TaskSystem.hpp"
#include "window/Window.hpp"
#include "world/World.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <thread>

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
    void startRenderLoop();
    void stopRenderLoop();

    static constexpr std::array<SystemFrameStat, 11> kInitialFrameStats{{
        {"Window", 0.0F},
        {"Input", 0.0F},
        {"Camera", 0.0F},
        {"Audio", 0.0F},
        {"Physics", 0.0F},
        {"ECS", 0.0F},
        {"Networking", 0.0F},
        {"Scripting", 0.0F},
        {"Tasks", 0.0F},
        {"World", 0.0F},
        {"Renderer", 0.0F},
    }};

    std::unique_ptr<Window> m_window;
    std::unique_ptr<InputManager> m_inputManager;
    std::unique_ptr<VulkanContext> m_vulkan;
    std::unique_ptr<RenderFramePipeline> m_renderPipeline;
    std::unique_ptr<FreeCameraController> m_freeCameraController;
    std::unique_ptr<PathTracerRenderer> m_pathTracerRenderer;
    std::unique_ptr<DebugOverlayRenderer> m_debugOverlay;
    std::unique_ptr<WorldSceneRenderer> m_worldSceneRenderer;
    std::unique_ptr<AudioSystem> m_audio;
    std::unique_ptr<PhysicsSystem> m_physics;
    std::unique_ptr<ECSSystem> m_ecs;
    std::unique_ptr<NetworkSystem> m_network;
    std::unique_ptr<ScriptingSystem> m_scripting;
    std::unique_ptr<TaskSystem> m_tasks;
    std::unique_ptr<World> m_world;
    RenderStateStore m_renderStateStore;
    std::array<SystemFrameStat, 11> m_systemFrameStats{kInitialFrameStats};
    float m_lastFrameDeltaMilliseconds{0.0F};
    float m_lastFrameCpuMilliseconds{0.0F};
    std::atomic<bool> m_renderLoopRunning{false};
    std::thread m_renderThread;
};

} // namespace Meridian
