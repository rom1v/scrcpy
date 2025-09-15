#include "display.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <libavutil/pixfmt.h>

#include "util/log.h"
#include "util/sdl.h"

static bool
sc_display_init_novideo_icon(struct sc_display *display,
                             SDL_Surface *icon_novideo) {
    assert(icon_novideo);

    bool ok = SDL_SetRenderLogicalPresentation(display->renderer,
                                               icon_novideo->w,
                                               icon_novideo->h,
                                        SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (!ok) {
        LOGW("Could not set renderer logical size: %s", SDL_GetError());
        // don't fail
    }

    display->texture = SDL_CreateTextureFromSurface(display->renderer,
                                                    icon_novideo);
    if (!display->texture) {
        LOGE("Could not create texture: %s", SDL_GetError());
        return false;
    }

    return true;
}

bool
sc_display_init(struct sc_display *display, SDL_Window *window,
                SDL_Surface *icon_novideo, bool mipmaps) {
    display->renderer = SDL_CreateRenderer(window, NULL);
    if (!display->renderer) {
        LOGE("Could not create renderer: %s", SDL_GetError());
        return false;
    }

    const char *renderer_name = SDL_GetRendererName(display->renderer);
    LOGI("Renderer: %s", renderer_name ? renderer_name : "(unknown)");

    display->mipmaps = false;

#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    display->gl_context = NULL;
#endif

    // starts with "opengl"
    bool use_opengl = renderer_name && !strncmp(renderer_name, "opengl", 6);
    if (use_opengl) {

#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
        // Persuade macOS to give us something better than OpenGL 2.1.
        // If we create a Core Profile context, we get the best OpenGL version.
        bool ok = SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                      SDL_GL_CONTEXT_PROFILE_CORE);
        if (!ok) {
            LOGW("Could not set a GL Core Profile Context");
        }

        LOGD("Creating OpenGL Core Profile context");
        display->gl_context = SDL_GL_CreateContext(window);
        if (!display->gl_context) {
            LOGE("Could not create OpenGL context: %s", SDL_GetError());
            SDL_DestroyRenderer(display->renderer);
            return false;
        }
#endif

        struct sc_opengl *gl = &display->gl;
        sc_opengl_init(gl);

        LOGI("OpenGL version: %s", gl->version);

        if (mipmaps) {
            bool supports_mipmaps =
                sc_opengl_version_at_least(gl, 3, 0, /* OpenGL 3.0+ */
                                               2, 0  /* OpenGL ES 2.0+ */);
            if (supports_mipmaps) {
                LOGI("Trilinear filtering enabled");
                display->mipmaps = true;
            } else {
                LOGW("Trilinear filtering disabled "
                     "(OpenGL 3.0+ or ES 2.0+ required)");
            }
        } else {
            LOGI("Trilinear filtering disabled");
        }
    } else if (mipmaps) {
        LOGD("Trilinear filtering disabled (not an OpenGL renderer)");
    }

    display->texture = NULL;
    display->pending.flags = 0;
    display->pending.frame = NULL;

    if (icon_novideo) {
        // Without video, set a static scrcpy icon as window content
        bool ok = sc_display_init_novideo_icon(display, icon_novideo);
        if (!ok) {
#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
            SDL_GL_DestroyContext(display->gl_context);
#endif
            SDL_DestroyRenderer(display->renderer);
            return false;
        }
    }

    return true;
}

void
sc_display_destroy(struct sc_display *display) {
    if (display->pending.frame) {
        av_frame_free(&display->pending.frame);
    }
#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    SDL_GL_DestroyContext(display->gl_context);
#endif
    if (display->texture) {
        SDL_DestroyTexture(display->texture);
    }
    SDL_DestroyRenderer(display->renderer);
}

static enum SDL_Colorspace
sc_display_to_sdl_color_space(enum AVColorSpace color_space,
                              enum AVColorRange color_range) {
    bool full_range = color_range == AVCOL_RANGE_JPEG;

    switch (color_space) {
        case AVCOL_SPC_BT709:
        case AVCOL_SPC_RGB:
            return full_range ? SDL_COLORSPACE_BT709_FULL
                              : SDL_COLORSPACE_BT709_LIMITED;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return full_range ? SDL_COLORSPACE_BT601_FULL
                              : SDL_COLORSPACE_BT601_LIMITED;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return full_range ? SDL_COLORSPACE_BT2020_FULL
                              : SDL_COLORSPACE_BT2020_LIMITED;
        default:
            return SDL_COLORSPACE_JPEG;
    }
}

static SDL_Texture *
sc_display_create_texture(struct sc_display *display,
                          struct sc_size size, enum AVColorSpace color_space,
                          enum AVColorRange color_range) {
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        return NULL;
    }

    enum SDL_Colorspace sdl_color_space =
        sc_display_to_sdl_color_space(color_space, color_range);

    bool ok =
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                              SDL_PIXELFORMAT_YV12);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                SDL_TEXTUREACCESS_STREAMING);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER,
                                size.width);
    ok &= SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER,
                                size.height);
    ok &= SDL_SetNumberProperty(props,
                                SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                                sdl_color_space);

    if (!ok) {
        LOGE("Could not set texture properties");
        SDL_DestroyProperties(props);
        return NULL;
    }

    SDL_Renderer *renderer = display->renderer;
    SDL_Texture *texture = SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);
    if (!texture) {
        LOGD("Could not create texture: %s", SDL_GetError());
        return NULL;
    }

    if (display->mipmaps) {
        struct sc_opengl *gl = &display->gl;

        SDL_PropertiesID props = SDL_GetTextureProperties(texture);
        if (!props) {
            LOGE("Could not get texture properties: %s", SDL_GetError());
            SDL_DestroyTexture(texture);
            return NULL;
        }

        const char *renderer_name = SDL_GetRendererName(display->renderer);
        const char *key = !renderer_name || !strcmp(renderer_name, "opengl")
                        ? SDL_PROP_TEXTURE_OPENGL_TEXTURE_NUMBER
                        : SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER;

        int64_t texture_id = SDL_GetNumberProperty(props, key, 0);
        SDL_DestroyProperties(props);
        if (!texture_id) {
            LOGE("Could not get texture id: %s", SDL_GetError());
            SDL_DestroyTexture(texture);
            return NULL;
        }

        assert(!(texture_id & ~0xFFFFFFFF)); // fits in uint32_t
        display->texture_id = texture_id;
        gl->BindTexture(GL_TEXTURE_2D, display->texture_id);

        // Enable trilinear filtering for downscaling
        gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                          GL_LINEAR_MIPMAP_LINEAR);
        gl->TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -1.f);

        gl->BindTexture(GL_TEXTURE_2D, 0);
    }

    return texture;
}

static inline void
sc_display_set_pending_texture(struct sc_display *display,
                               struct sc_size size,
                               enum AVColorRange color_range) {
    assert(!display->texture);
    display->pending.texture.size = size;
    display->pending.texture.color_range = color_range;
    display->pending.flags |= SC_DISPLAY_PENDING_FLAG_TEXTURE;
}

static bool
sc_display_set_pending_frame(struct sc_display *display, const AVFrame *frame) {
    if (!display->pending.frame) {
        display->pending.frame = av_frame_alloc();
        if (!display->pending.frame) {
            LOG_OOM();
            return false;
        }
    }

    av_frame_unref(display->pending.frame);
    int r = av_frame_ref(display->pending.frame, frame);
    if (r) {
        LOGE("Could not ref frame: %d", r);
        return false;
    }

    display->pending.flags |= SC_DISPLAY_PENDING_FLAG_FRAME;

    return true;
}

// Forward declaration
static bool
sc_display_update_texture_internal(struct sc_display *display,
                                   const AVFrame *frame);

static bool
sc_display_apply_pending(struct sc_display *display) {
    if (display->pending.flags & SC_DISPLAY_PENDING_FLAG_TEXTURE) {
        assert(!display->texture);
        display->texture =
            sc_display_create_texture(display,
                                      display->pending.texture.size,
                                      display->pending.texture.color_space,
                                      display->pending.texture.color_range);
        if (!display->texture) {
            return false;
        }

        display->pending.flags &= ~SC_DISPLAY_PENDING_FLAG_TEXTURE;
    }

    if (display->pending.flags & SC_DISPLAY_PENDING_FLAG_FRAME) {
        assert(display->pending.frame);
        bool ok = sc_display_update_texture_internal(display,
                                                     display->pending.frame);
        if (!ok) {
            return false;
        }

        av_frame_unref(display->pending.frame);
        display->pending.flags &= ~SC_DISPLAY_PENDING_FLAG_FRAME;
    }

    return true;
}

static bool
sc_display_prepare_texture_internal(struct sc_display *display,
                                    struct sc_size size,
                                    enum AVColorSpace color_space,
                                    enum AVColorRange color_range) {
    assert(size.width && size.height);

    if (display->texture) {
        SDL_DestroyTexture(display->texture);
    }

    display->texture =
            sc_display_create_texture(display, size, color_space, color_range);
    if (!display->texture) {
        return false;
    }

    LOGI("Texture: %" PRIu16 "x%" PRIu16, size.width, size.height);
    return true;
}

enum sc_display_result
sc_display_prepare_texture(struct sc_display *display, struct sc_size size,
                           enum AVColorSpace color_space,
                           enum AVColorRange color_range) {
    bool ok = sc_display_prepare_texture_internal(display, size, color_space,
                                                  color_range);
    if (!ok) {
        sc_display_set_pending_texture(display, size, color_range);
        return SC_DISPLAY_RESULT_PENDING;

    }

    return SC_DISPLAY_RESULT_OK;
}

static bool
sc_display_update_texture_internal(struct sc_display *display,
                                   const AVFrame *frame) {
    bool ok = SDL_UpdateYUVTexture(display->texture, NULL,
                                   frame->data[0], frame->linesize[0],
                                   frame->data[1], frame->linesize[1],
                                   frame->data[2], frame->linesize[2]);
    if (!ok) {
        LOGD("Could not update texture: %s", SDL_GetError());
        return false;
    }

    if (display->mipmaps) {
        assert(display->texture_id);
        struct sc_opengl *gl = &display->gl;

        gl->BindTexture(GL_TEXTURE_2D, display->texture_id);
        gl->GenerateMipmap(GL_TEXTURE_2D);
        gl->BindTexture(GL_TEXTURE_2D, 0);
    }

    return true;
}

enum sc_display_result
sc_display_update_texture(struct sc_display *display, const AVFrame *frame) {
    bool ok = sc_display_update_texture_internal(display, frame);
    if (!ok) {
        ok = sc_display_set_pending_frame(display, frame);
        if (!ok) {
            LOGE("Could not set pending frame");
            return SC_DISPLAY_RESULT_ERROR;
        }

        return SC_DISPLAY_RESULT_PENDING;
    }

    return SC_DISPLAY_RESULT_OK;
}

enum sc_display_result
sc_display_render(struct sc_display *display, const SDL_Rect *geometry,
                  enum sc_orientation orientation) {
    sc_sdl_render_clear(display->renderer);

    if (display->pending.flags) {
        bool ok = sc_display_apply_pending(display);
        if (!ok) {
            return SC_DISPLAY_RESULT_PENDING;
        }
    }

    SDL_Renderer *renderer = display->renderer;
    SDL_Texture *texture = display->texture;

    if (orientation == SC_ORIENTATION_0) {
        SDL_FRect frect;
        SDL_RectToFRect(geometry, &frect);
        bool ok = SDL_RenderTexture(renderer, texture, NULL, &frect);
        if (!ok) {
            LOGE("Could not render texture: %s", SDL_GetError());
            return SC_DISPLAY_RESULT_ERROR;
        }
    } else {
        unsigned cw_rotation = sc_orientation_get_rotation(orientation);
        double angle = 90 * cw_rotation;

        SDL_FRect frect;
        if (sc_orientation_is_swap(orientation)) {
            frect.x = geometry->x + (geometry->w - geometry->h) / 2.f;
            frect.y = geometry->y + (geometry->h - geometry->w) / 2.f;
            frect.w = geometry->h;
            frect.h = geometry->w;
        } else {
            SDL_RectToFRect(geometry, &frect);
        }

        SDL_FlipMode flip = sc_orientation_is_mirror(orientation)
                              ? SDL_FLIP_HORIZONTAL : 0;

        bool ok = SDL_RenderTextureRotated(renderer, texture, NULL, &frect,
                                           angle, NULL, flip);
        if (!ok) {
            LOGE("Could not render texture: %s", SDL_GetError());
            return SC_DISPLAY_RESULT_ERROR;
        }
    }

    sc_sdl_render_present(display->renderer);
    return SC_DISPLAY_RESULT_OK;
}
