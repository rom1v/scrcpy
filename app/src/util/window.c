#include "window.h"

SDL_Window *
sc_create_sdl_window(const char *title, int64_t x, int64_t y, int64_t width,
                     int64_t height, int64_t flags) {
    SDL_Window *window = NULL;

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        return NULL;
    }

    bool ok =
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                              title);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
                                width);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                                height);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                                flags);

    if (!ok) {
        SDL_DestroyProperties(props);
        return NULL;
    }

    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return window;
}
