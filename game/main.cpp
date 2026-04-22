#include "GameApplication.h"

#include <SDL3/SDL_main.h>

int main(int argc, char* argv[])
{
    GameApplication app;

    if (!app.Init(argc, argv))
    {
        return 1;
    }

    app.RunLoop();
    app.Shutdown();
    return 0;
}
