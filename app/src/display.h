#ifndef SC_DISPLAY_H
#define SC_DISPLAY_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <libavutil/frame.h>
#include <SDL3/SDL.h>

#include "coords.h"
#include "opengl.h"

struct sc_display {
    SDL_Renderer *renderer; // owned by the caller
    SDL_Texture *texture;

    struct sc_opengl gl;

    bool mipmaps;
    uint32_t texture_id; // only set if mipmaps is enabled
};

bool
sc_display_init(struct sc_display *display, SDL_Renderer *renderer,
                bool mipmaps);

void
sc_display_destroy(struct sc_display *display);

bool
sc_display_prepare_texture(struct sc_display *display, struct sc_size size,
                           enum AVColorSpace color_space,
                           enum AVColorRange color_range);

bool
sc_display_update_texture(struct sc_display *display, const AVFrame *frame);

bool
sc_display_set_texture_from_surface(struct sc_display *display,
                                    SDL_Surface *surface);

#endif
