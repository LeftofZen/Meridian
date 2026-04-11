#include "core/Engine.hpp"

#include <SDL3/SDL_main.h>

int main(int /*argc*/, char* /*argv*/[])
{
    Meridian::Engine engine;

    if (!engine.init()) {
        return 1;
    }

    engine.run();
    engine.shutdown();
    return 0;
}
