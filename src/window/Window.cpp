#include "window/Window.hpp"

#include "core/Logger.hpp"

namespace Meridian {

Window::Window(const WindowConfig& config) : m_config(config) {}

Window::~Window()
{
    shutdown();
}

bool Window::init()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        MRD_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    m_window = SDL_CreateWindow(
        m_config.title.c_str(),
        m_config.width,
        m_config.height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    if (!m_window) {
        MRD_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }

    MRD_INFO("Window '{}' created ({}x{})", m_config.title, m_config.width, m_config.height);
    return true;
}

void Window::shutdown()
{
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        SDL_Quit();
    }
}

void Window::processEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_QUIT:
                m_shouldClose = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) {
                    m_shouldClose = true;
                }
                break;
            default:
                break;
        }
    }
}

} // namespace Meridian
