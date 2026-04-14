#include "renderer/FreeCameraController.hpp"

#include "input/InputManager.hpp"

#include <algorithm>
#include <cmath>

namespace Meridian {

namespace {

struct Vec3 {
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

[[nodiscard]] Vec3 add(Vec3 left, Vec3 right) noexcept
{
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] Vec3 subtract(Vec3 left, Vec3 right) noexcept
{
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] Vec3 multiply(Vec3 vector, float scalar) noexcept
{
    return Vec3{vector.x * scalar, vector.y * scalar, vector.z * scalar};
}

[[nodiscard]] float length(Vec3 vector) noexcept
{
    return std::sqrt(
        vector.x * vector.x +
        vector.y * vector.y +
        vector.z * vector.z);
}

[[nodiscard]] Vec3 normalize(Vec3 vector) noexcept
{
    const float vectorLength = length(vector);
    if (vectorLength <= 0.0F) {
        return Vec3{};
    }

    return multiply(vector, 1.0F / vectorLength);
}

[[nodiscard]] Vec3 cross(Vec3 left, Vec3 right) noexcept
{
    return Vec3{
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] Vec3 toVec3(const std::array<float, 3>& values) noexcept
{
    return Vec3{values[0], values[1], values[2]};
}

[[nodiscard]] std::array<float, 3> toArray(Vec3 vector) noexcept
{
    return {vector.x, vector.y, vector.z};
}

} // namespace

FreeCameraController::FreeCameraController(InputManager& inputManager) noexcept
    : m_inputManager(inputManager)
{
}

bool FreeCameraController::init() noexcept
{
    const Vec3 initialForward = normalize(toVec3(m_cameraState.forward));
    m_yawRadians = std::atan2(initialForward.z, initialForward.x);
    m_pitchRadians = std::asin(std::clamp(initialForward.y, -1.0F, 1.0F));
    updateForwardVector();
    return true;
}

void FreeCameraController::shutdown() noexcept
{
}

void FreeCameraController::update(float deltaTimeSeconds)
{
    const float turnDelta = m_lookSpeed * deltaTimeSeconds;
    const std::array<float, 2> mouseLookDelta = m_inputManager.mouseLookDelta();

    m_yawRadians += mouseLookDelta[0] * m_mouseLookSensitivity;
    m_pitchRadians -= mouseLookDelta[1] * m_mouseLookSensitivity;

    if (m_inputManager.isActionDown(InputAction::LookLeft)) {
        m_yawRadians -= turnDelta;
    }
    if (m_inputManager.isActionDown(InputAction::LookRight)) {
        m_yawRadians += turnDelta;
    }
    if (m_inputManager.isActionDown(InputAction::LookUp)) {
        m_pitchRadians += turnDelta;
    }
    if (m_inputManager.isActionDown(InputAction::LookDown)) {
        m_pitchRadians -= turnDelta;
    }

    constexpr float kPitchLimit = 1.55334303F;
    m_pitchRadians = std::clamp(m_pitchRadians, -kPitchLimit, kPitchLimit);
    updateForwardVector();

    const Vec3 worldUp{0.0F, 1.0F, 0.0F};
    const Vec3 forward = toVec3(m_cameraState.forward);
    const Vec3 right = normalize(cross(forward, worldUp));

    Vec3 moveDirection{};
    if (m_inputManager.isActionDown(InputAction::MoveForward)) {
        moveDirection = add(moveDirection, forward);
    }
    if (m_inputManager.isActionDown(InputAction::MoveBackward)) {
        moveDirection = subtract(moveDirection, forward);
    }
    if (m_inputManager.isActionDown(InputAction::MoveRight)) {
        moveDirection = add(moveDirection, right);
    }
    if (m_inputManager.isActionDown(InputAction::MoveLeft)) {
        moveDirection = subtract(moveDirection, right);
    }
    if (m_inputManager.isActionDown(InputAction::MoveUp)) {
        moveDirection = add(moveDirection, worldUp);
    }
    if (m_inputManager.isActionDown(InputAction::MoveDown)) {
        moveDirection = subtract(moveDirection, worldUp);
    }

    if (length(moveDirection) > 0.0F) {
        const float speed =
            m_moveSpeed *
            (m_inputManager.isActionDown(InputAction::Boost) ? m_boostMultiplier : 1.0F) *
            deltaTimeSeconds;
        const Vec3 updatedPosition = add(
            toVec3(m_cameraState.position),
            multiply(normalize(moveDirection), speed));
        m_cameraState.position = toArray(updatedPosition);
    }
}

void FreeCameraController::updateForwardVector() noexcept
{
    const Vec3 forward{
        std::cos(m_pitchRadians) * std::cos(m_yawRadians),
        std::sin(m_pitchRadians),
        std::cos(m_pitchRadians) * std::sin(m_yawRadians),
    };
    m_cameraState.forward = toArray(normalize(forward));
}

} // namespace Meridian