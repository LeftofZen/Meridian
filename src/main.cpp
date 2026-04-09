#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

int main(int argc, char* argv[])
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Meridian", 1280, 720, 0);
    if (!window)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Event event;
    while (SDL_WaitEvent(&event))
    {
        if (event.type == SDL_EVENT_QUIT)
        {
            break;
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
