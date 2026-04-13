#pragma once

#include "core/ISystem.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace Meridian {

struct WindowConfig {
    std::string title{"Meridian"};
    int width{1280};
    int height{720};
};

class Window final : public ISystem {
public:
    explicit Window(const WindowConfig& config);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

    [[nodiscard]] SDL_Window* getHandle() const noexcept { return m_window; }
    [[nodiscard]] bool shouldClose() const noexcept { return m_shouldClose; }
    [[nodiscard]] int getWidth() const noexcept { return m_config.width; }
    [[nodiscard]] int getHeight() const noexcept { return m_config.height; }

    void update(float deltaTimeSeconds) override;
    void processEvents();

private:
    WindowConfig m_config;
    SDL_Window* m_window{nullptr};
    bool m_shouldClose{false};
};

} // namespace Meridian
