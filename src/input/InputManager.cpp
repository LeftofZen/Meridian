#include "input/InputManager.hpp"

#include <algorithm>

namespace Meridian {

namespace {

constexpr std::size_t actionIndex(InputAction action) noexcept
{
    return static_cast<std::size_t>(action);
}

} // namespace

bool InputManager::init() noexcept
{
    resetBindings();
    clearState();
    return true;
}

void InputManager::shutdown() noexcept
{
    setMouseLookActive(false);
    m_window = nullptr;
    clearState();
}

void InputManager::update(float /*deltaTimeSeconds*/)
{
    m_frameMouseDeltaX = m_pendingMouseDeltaX;
    m_frameMouseDeltaY = m_pendingMouseDeltaY;
    m_pendingMouseDeltaX = 0.0F;
    m_pendingMouseDeltaY = 0.0F;
}

void InputManager::attachWindow(SDL_Window* window) noexcept
{
    if (m_window == window) {
        return;
    }

    setMouseLookActive(false);
    m_window = window;
}

void InputManager::handleEvent(const SDL_Event& event) noexcept
{
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (event.key.scancode >= 0 && event.key.scancode < SDL_SCANCODE_COUNT) {
                m_keyStates[static_cast<std::size_t>(event.key.scancode)] = true;
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (event.key.scancode >= 0 && event.key.scancode < SDL_SCANCODE_COUNT) {
                m_keyStates[static_cast<std::size_t>(event.key.scancode)] = false;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_RIGHT) {
                setMouseLookActive(true);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_RIGHT) {
                setMouseLookActive(false);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (m_mouseLookActive) {
                m_pendingMouseDeltaX += event.motion.xrel;
                m_pendingMouseDeltaY += event.motion.yrel;
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            setMouseLookActive(false);
            clearState();
            break;
        default:
            break;
    }
}

void InputManager::setBinding(InputAction action, SDL_Scancode scancode) noexcept
{
    m_bindings[actionIndex(action)] = scancode;
}

SDL_Scancode InputManager::getBinding(InputAction action) const noexcept
{
    return m_bindings[actionIndex(action)];
}

bool InputManager::isActionDown(InputAction action) const noexcept
{
    const SDL_Scancode scancode = getBinding(action);
    return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
        ? m_keyStates[static_cast<std::size_t>(scancode)]
        : false;
}

std::array<float, 2> InputManager::mouseLookDelta() const noexcept
{
    return {m_frameMouseDeltaX, m_frameMouseDeltaY};
}

const char* InputManager::actionName(InputAction action) noexcept
{
    switch (action) {
        case InputAction::MoveForward:
            return "Move Forward";
        case InputAction::MoveBackward:
            return "Move Backward";
        case InputAction::MoveLeft:
            return "Move Left";
        case InputAction::MoveRight:
            return "Move Right";
        case InputAction::MoveUp:
            return "Move Up";
        case InputAction::MoveDown:
            return "Move Down";
        case InputAction::LookLeft:
            return "Look Left";
        case InputAction::LookRight:
            return "Look Right";
        case InputAction::LookUp:
            return "Look Up";
        case InputAction::LookDown:
            return "Look Down";
        case InputAction::Boost:
            return "Boost";
        case InputAction::Count:
            return "Count";
        default:
            return "Unknown";
    }
}

void InputManager::resetBindings() noexcept
{
    m_bindings[actionIndex(InputAction::MoveForward)] = SDL_SCANCODE_W;
    m_bindings[actionIndex(InputAction::MoveBackward)] = SDL_SCANCODE_S;
    m_bindings[actionIndex(InputAction::MoveLeft)] = SDL_SCANCODE_A;
    m_bindings[actionIndex(InputAction::MoveRight)] = SDL_SCANCODE_D;
    m_bindings[actionIndex(InputAction::MoveUp)] = SDL_SCANCODE_SPACE;
    m_bindings[actionIndex(InputAction::MoveDown)] = SDL_SCANCODE_LCTRL;
    m_bindings[actionIndex(InputAction::LookLeft)] = SDL_SCANCODE_LEFT;
    m_bindings[actionIndex(InputAction::LookRight)] = SDL_SCANCODE_RIGHT;
    m_bindings[actionIndex(InputAction::LookUp)] = SDL_SCANCODE_UP;
    m_bindings[actionIndex(InputAction::LookDown)] = SDL_SCANCODE_DOWN;
    m_bindings[actionIndex(InputAction::Boost)] = SDL_SCANCODE_LSHIFT;
}

void InputManager::clearState() noexcept
{
    std::fill(m_keyStates.begin(), m_keyStates.end(), false);
    m_pendingMouseDeltaX = 0.0F;
    m_pendingMouseDeltaY = 0.0F;
    m_frameMouseDeltaX = 0.0F;
    m_frameMouseDeltaY = 0.0F;
}

void InputManager::setMouseLookActive(bool active) noexcept
{
    if (m_mouseLookActive == active) {
        return;
    }

    if (m_window != nullptr) {
        SDL_SetWindowRelativeMouseMode(m_window, active);
    }

    m_mouseLookActive = active;
    m_pendingMouseDeltaX = 0.0F;
    m_pendingMouseDeltaY = 0.0F;
    m_frameMouseDeltaX = 0.0F;
    m_frameMouseDeltaY = 0.0F;
}

} // namespace Meridian