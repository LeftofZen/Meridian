#include "window/Window.hpp"

#include "core/Logger.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>

#include <filesystem>

namespace {

constexpr const char* kWindowIconFileName = "meridian-icon.png";

void SetWindowIcon(SDL_Window* window)
{
    const char* const basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        MRD_WARN("SDL_GetBasePath failed while locating the window icon: {}", SDL_GetError());
        return;
    }

    const std::filesystem::path iconPath = std::filesystem::path(basePath) / kWindowIconFileName;

    SDL_Surface* iconSurface = SDL_LoadPNG(iconPath.string().c_str());

    if (iconSurface == nullptr) {
        MRD_WARN("Failed to load window icon '{}': {}", iconPath.string(), SDL_GetError());
        return;
    }

    SDL_SetWindowIcon(window, iconSurface);
    SDL_DestroySurface(iconSurface);
}

} // namespace

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

    SetWindowIcon(m_window);

    MRD_INFO("Window '{}' created ({}x{})", m_config.title, m_config.width, m_config.height);
    return true;
}

void Window::shutdown()
{
    if (m_window) {
        SDL_DestroyWindow(m_window);
        SDL_Quit();
        m_window = nullptr;
    }
}

void Window::update(float /*deltaTimeSeconds*/)
{
    if (!m_window) {
        return;
    }

    processEvents();
}

void Window::setEventHandler(std::function<void(const SDL_Event&)> eventHandler)
{
    m_eventHandler = std::move(eventHandler);
}

void Window::processEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (m_eventHandler) {
            m_eventHandler(event);
        }

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
