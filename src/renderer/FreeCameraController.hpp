#pragma once

#include "core/ISystem.hpp"
#include "renderer/CameraState.hpp"

namespace Meridian {

class InputManager;

class FreeCameraController final : public ISystem {
public:
    explicit FreeCameraController(InputManager& inputManager) noexcept;
    ~FreeCameraController() = default;

    FreeCameraController(const FreeCameraController&) = delete;
    FreeCameraController& operator=(const FreeCameraController&) = delete;
    FreeCameraController(FreeCameraController&&) = delete;
    FreeCameraController& operator=(FreeCameraController&&) = delete;

    [[nodiscard]] bool init() noexcept;
    void shutdown() noexcept;
    void update(float deltaTimeSeconds) override;

    [[nodiscard]] const CameraRenderState& cameraState() const noexcept { return m_cameraState; }

private:
    void updateForwardVector() noexcept;

    InputManager& m_inputManager;
    CameraRenderState m_cameraState;
    float m_yawRadians{0.0F};
    float m_pitchRadians{0.0F};
    float m_moveSpeed{18.0F};
    float m_boostMultiplier{2.5F};
    float m_lookSpeed{1.6F};
    float m_mouseLookSensitivity{0.0025F};
};

} // namespace Meridian