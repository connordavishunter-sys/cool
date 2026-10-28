#include "game.hpp"

#include <cstdio>
#include <exception>

int main(int, char**) {
    try {
        Game game;
        game.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ordis", e.what(), nullptr);
        return 1;
    }
    return 0;
}
