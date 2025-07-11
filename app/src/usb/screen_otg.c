#include "screen_otg.h"

#include <assert.h>
#include <stddef.h>

#include "icon.h"
#include "options.h"
#include "util/acksync.h"
#include "util/log.h"
#include "util/window.h"

static void
sc_screen_otg_render(struct sc_screen_otg *screen) {
    SDL_RenderClear(screen->renderer);
    if (screen->texture) {
        SDL_RenderTexture(screen->renderer, screen->texture, NULL, NULL);
    }
    SDL_RenderPresent(screen->renderer);
}

bool
sc_screen_otg_init(struct sc_screen_otg *screen,
                   const struct sc_screen_otg_params *params) {
    screen->keyboard = params->keyboard;
    screen->mouse = params->mouse;
    screen->gamepad = params->gamepad;

    const char *title = params->window_title;
    assert(title);

    int x = params->window_x != SC_WINDOW_POSITION_UNDEFINED
          ? params->window_x : (int) SDL_WINDOWPOS_UNDEFINED;
    int y = params->window_y != SC_WINDOW_POSITION_UNDEFINED
          ? params->window_y : (int) SDL_WINDOWPOS_UNDEFINED;
    int width = params->window_width ? params->window_width : 256;
    int height = params->window_height ? params->window_height : 256;

    uint32_t window_flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (params->always_on_top) {
        window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (params->window_borderless) {
        window_flags |= SDL_WINDOW_BORDERLESS;
    }

    screen->window =
        sc_create_sdl_window(title, x, y, width, height, window_flags);
    if (!screen->window) {
        LOGE("Could not create window: %s", SDL_GetError());
        return false;
    }

    screen->renderer = SDL_CreateRenderer(screen->window, NULL);
    if (!screen->renderer) {
        LOGE("Could not create renderer: %s", SDL_GetError());
        goto error_destroy_window;
    }

    SDL_Surface *icon = scrcpy_icon_load();

    if (icon) {
        SDL_SetWindowIcon(screen->window, icon);

        bool ok =
            SDL_SetRenderLogicalPresentation(screen->renderer, icon->w, icon->h,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
        if (!ok) {
            LOGW("Could not set renderer logical size: %s", SDL_GetError());
            // don't fail
        }

        screen->texture = SDL_CreateTextureFromSurface(screen->renderer, icon);
        scrcpy_icon_destroy(icon);
        if (!screen->texture) {
            goto error_destroy_renderer;
        }
    } else {
        screen->texture = NULL;
        LOGW("Could not load icon");
    }

    sc_mouse_capture_init(&screen->mc, screen->window, params->shortcut_mods);

    if (screen->mouse) {
        // Capture mouse on start
        sc_mouse_capture_set_active(&screen->mc, true);
    }

    return true;

error_destroy_window:
    SDL_DestroyWindow(screen->window);
error_destroy_renderer:
    SDL_DestroyRenderer(screen->renderer);

    return false;
}

void
sc_screen_otg_destroy(struct sc_screen_otg *screen) {
    if (screen->texture) {
        SDL_DestroyTexture(screen->texture);
    }
    SDL_DestroyRenderer(screen->renderer);
    SDL_DestroyWindow(screen->window);
}

static void
sc_screen_otg_process_key(struct sc_screen_otg *screen,
                             const SDL_KeyboardEvent *event) {
    assert(screen->keyboard);
    struct sc_key_processor *kp = &screen->keyboard->key_processor;

    struct sc_key_event evt = {
        .action = sc_action_from_sdl_keyboard_type(event->type),
        .keycode = sc_keycode_from_sdl(event->key),
        .scancode = sc_scancode_from_sdl(event->scancode),
        .repeat = event->repeat,
        .mods_state = sc_mods_state_from_sdl(event->mod),
    };

    assert(kp->ops->process_key);
    kp->ops->process_key(kp, &evt, SC_SEQUENCE_INVALID);
}

static void
sc_screen_otg_process_mouse_motion(struct sc_screen_otg *screen,
                                   const SDL_MouseMotionEvent *event) {
    assert(screen->mouse);
    struct sc_mouse_processor *mp = &screen->mouse->mouse_processor;

    struct sc_mouse_motion_event evt = {
        // .position not used for HID events
        .xrel = event->xrel,
        .yrel = event->yrel,
        .buttons_state = sc_mouse_buttons_state_from_sdl(event->state),
    };

    assert(mp->ops->process_mouse_motion);
    mp->ops->process_mouse_motion(mp, &evt);
}

static void
sc_screen_otg_process_mouse_button(struct sc_screen_otg *screen,
                                   const SDL_MouseButtonEvent *event) {
    assert(screen->mouse);
    struct sc_mouse_processor *mp = &screen->mouse->mouse_processor;

    uint32_t sdl_buttons_state = SDL_GetMouseState(NULL, NULL);

    struct sc_mouse_click_event evt = {
        // .position not used for HID events
        .action = sc_action_from_sdl_mousebutton_type(event->type),
        .button = sc_mouse_button_from_sdl(event->button),
        .buttons_state = sc_mouse_buttons_state_from_sdl(sdl_buttons_state),
    };

    assert(mp->ops->process_mouse_click);
    mp->ops->process_mouse_click(mp, &evt);
}

static void
sc_screen_otg_process_mouse_wheel(struct sc_screen_otg *screen,
                                  const SDL_MouseWheelEvent *event) {
    assert(screen->mouse);
    struct sc_mouse_processor *mp = &screen->mouse->mouse_processor;

    uint32_t sdl_buttons_state = SDL_GetMouseState(NULL, NULL);

    struct sc_mouse_scroll_event evt = {
        // .position not used for HID events
#if SDL_VERSION_ATLEAST(2, 0, 18)
        .hscroll = event->x,
        .vscroll = event->y,
#else
        .hscroll = event->x,
        .vscroll = event->y,
#endif
        .buttons_state = sc_mouse_buttons_state_from_sdl(sdl_buttons_state),
    };

    assert(mp->ops->process_mouse_scroll);
    mp->ops->process_mouse_scroll(mp, &evt);
}

static void
sc_screen_otg_process_gamepad_device(struct sc_screen_otg *screen,
                                     const SDL_GamepadDeviceEvent *event) {
    assert(screen->gamepad);
    struct sc_gamepad_processor *gp = &screen->gamepad->gamepad_processor;

    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        SDL_Gamepad *gc = SDL_OpenGamepad(event->which);
        if (!gc) {
            LOGW("Could not open game controller");
            return;
        }

        SDL_Joystick *joystick = SDL_GetGamepadJoystick(gc);
        if (!joystick) {
            LOGW("Could not get controller joystick");
            SDL_CloseGamepad(gc);
            return;
        }

        struct sc_gamepad_device_event evt = {
            .gamepad_id = SDL_GetJoystickID(joystick),
        };
        gp->ops->process_gamepad_added(gp, &evt);
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        SDL_JoystickID id = event->which;

        SDL_Gamepad *gc = SDL_GetGamepadFromID(id);
        if (gc) {
            SDL_CloseGamepad(gc);
        } else {
            LOGW("Unknown gamepad device removed");
        }

        struct sc_gamepad_device_event evt = {
            .gamepad_id = id,
        };
        gp->ops->process_gamepad_removed(gp, &evt);
    }
}

static void
sc_screen_otg_process_gamepad_axis(struct sc_screen_otg *screen,
                                   const SDL_GamepadAxisEvent *event) {
    assert(screen->gamepad);
    struct sc_gamepad_processor *gp = &screen->gamepad->gamepad_processor;

    enum sc_gamepad_axis axis = sc_gamepad_axis_from_sdl(event->axis);
    if (axis == SC_GAMEPAD_AXIS_UNKNOWN) {
        return;
    }

    struct sc_gamepad_axis_event evt = {
        .gamepad_id = event->which,
        .axis = axis,
        .value = event->value,
    };
    gp->ops->process_gamepad_axis(gp, &evt);
}

static void
sc_screen_otg_process_gamepad_button(struct sc_screen_otg *screen,
                                     const SDL_GamepadButtonEvent *event) {
    assert(screen->gamepad);
    struct sc_gamepad_processor *gp = &screen->gamepad->gamepad_processor;

    enum sc_gamepad_button button = sc_gamepad_button_from_sdl(event->button);
    if (button == SC_GAMEPAD_BUTTON_UNKNOWN) {
        return;
    }

    struct sc_gamepad_button_event evt = {
        .gamepad_id = event->which,
        .action = sc_action_from_sdl_controllerbutton_type(event->type),
        .button = button,
    };
    gp->ops->process_gamepad_button(gp, &evt);
}

void
sc_screen_otg_handle_event(struct sc_screen_otg *screen, SDL_Event *event) {
    if (sc_mouse_capture_handle_event(&screen->mc, event)) {
        // The mouse capture handler consumed the event
        return;
    }

    switch (event->type) {
        case SDL_EVENT_WINDOW_EXPOSED:
            sc_screen_otg_render(screen);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (screen->keyboard) {
                sc_screen_otg_process_key(screen, &event->key);
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (screen->keyboard) {
                sc_screen_otg_process_key(screen, &event->key);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (screen->mouse) {
                sc_screen_otg_process_mouse_motion(screen, &event->motion);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (screen->mouse) {
                sc_screen_otg_process_mouse_button(screen, &event->button);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (screen->mouse) {
                sc_screen_otg_process_mouse_button(screen, &event->button);
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (screen->mouse) {
                sc_screen_otg_process_mouse_wheel(screen, &event->wheel);
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            // Handle device added or removed even if paused
            if (screen->gamepad) {
                sc_screen_otg_process_gamepad_device(screen, &event->gdevice);
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (screen->gamepad) {
                sc_screen_otg_process_gamepad_axis(screen, &event->gaxis);
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (screen->gamepad) {
                sc_screen_otg_process_gamepad_button(screen, &event->gbutton);
            }
            break;
    }
}
