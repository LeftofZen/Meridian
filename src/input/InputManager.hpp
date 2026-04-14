#pragma once

#include "core/ISystem.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>

namespace Meridian {

enum class InputAction : std::size_t {
    MoveForward = 0,
    MoveBackward,
    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    LookLeft,
    LookRight,
    LookUp,
    LookDown,
    Boost,
    Count,
};

class InputManager final : public ISystem {
public:
    InputManager() = default;
    ~InputManager() = default;

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;
    InputManager(InputManager&&) = delete;
    InputManager& operator=(InputManager&&) = delete;

    [[nodiscard]] bool init() noexcept;
    void shutdown() noexcept;
    void update(float deltaTimeSeconds) override;

    void attachWindow(SDL_Window* window) noexcept;
    void handleEvent(const SDL_Event& event) noexcept;
    void setBinding(InputAction action, SDL_Scancode scancode) noexcept;
    [[nodiscard]] SDL_Scancode getBinding(InputAction action) const noexcept;
    [[nodiscard]] bool isActionDown(InputAction action) const noexcept;
    [[nodiscard]] std::array<float, 2> mouseLookDelta() const noexcept;
    [[nodiscard]] bool isMouseLookActive() const noexcept { return m_mouseLookActive; }
    [[nodiscard]] static const char* actionName(InputAction action) noexcept;

private:
    void resetBindings() noexcept;
    void clearState() noexcept;
    void setMouseLookActive(bool active) noexcept;

    std::array<SDL_Scancode, static_cast<std::size_t>(InputAction::Count)> m_bindings{};
    std::array<bool, SDL_SCANCODE_COUNT> m_keyStates{};
    SDL_Window* m_window{nullptr};
    float m_pendingMouseDeltaX{0.0F};
    float m_pendingMouseDeltaY{0.0F};
    float m_frameMouseDeltaX{0.0F};
    float m_frameMouseDeltaY{0.0F};
    bool m_mouseLookActive{false};
};

} // namespace Meridian