#include "amulet.h"

#ifdef AM_BACKEND_SDL

#define GLEW_STATIC 1
#include "GL/glew.h"
#include "SDL.h"

#define MIN_UPDATE_TIME (1.0/400.0)

enum axis_button {
    AXIS_BUTTON_LEFT,
    AXIS_BUTTON_RIGHT,
    AXIS_BUTTON_UP,
    AXIS_BUTTON_DOWN,
    NUM_AXIS_BUTTONS
};

static std::vector<SDL_Window*> windows;
static SDL_GLContext gl_context;
static bool gl_context_initialized = false;
static bool sdl_initialized = false;

static char *filename = NULL;

static bool init_gl();

static bool controller_present = false;
static bool axis_button_down[NUM_AXIS_BUTTONS];
static float axis_hwm[SDL_CONTROLLER_AXIS_MAX];
//static LTKey controller_button_to_key(SDL_GameControllerButton button);
static void load_controller_mappings();
static void init_gamepad();

static void init_sdl();
static void init_audio();
static bool handle_events();
//static void key_handler(SDL_Window *win, int key, int scancode, int state, int mods);
//static void gen_controller_key_events();
//static void mouse_button_handler(SDL_Window *win, int button, int action, int mods);
//static void mouse_pos_handler(SDL_Window *win, double x, double y);
//static void resize_handler(SDL_Window *win, int w, int h);
//static am_key convert_key(int key);
//static bool controller_to_keys = true;
static bool process_args(int argc, char **argv);
static int report_status(lua_State *L, int status);

am_native_window *am_create_native_window(
    am_window_mode mode,
    int top, int left,
    int width, int height,
    const char *title,
    bool resizable,
    bool borderless,
    bool depth_buffer,
    bool stencil_buffer,
    int msaa_samples)
{
    if (!sdl_initialized) {
        init_sdl();
    }
    if (windows.size() > 0) {
        SDL_GL_MakeCurrent(windows[0], gl_context);
        SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    }
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    if (depth_buffer) {
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    } else {
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    }
    if (stencil_buffer) {
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    } else {
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    }
    if (msaa_samples > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa_samples);
    } else {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
    }
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
    switch (mode) {
        case AM_WINDOW_MODE_WINDOWED: break;
        case AM_WINDOW_MODE_FULLSCREEN: flags |= SDL_WINDOW_FULLSCREEN; break;
        case AM_WINDOW_MODE_FULLSCREEN_DESKTOP: flags |= SDL_WINDOW_FULLSCREEN_DESKTOP; break;
    }
    if (borderless) flags |= SDL_WINDOW_BORDERLESS;
    if (resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (left == -1) left = SDL_WINDOWPOS_CENTERED;
    else if (left < -1) left = SDL_WINDOWPOS_UNDEFINED;
    if (top == -1) top = SDL_WINDOWPOS_CENTERED;
    else if (top < -1) top = SDL_WINDOWPOS_UNDEFINED;
    if (width <= 0) width = 1;
    if (height <= 0) height = 1;
    SDL_Window *win = SDL_CreateWindow(
        title,
        left, top,
        width, height,
        flags
    );
    if (win == NULL) {
        return NULL;
    }
    if (!gl_context_initialized) {
        gl_context = SDL_GL_CreateContext(win);
        gl_context_initialized = true;
    }
    SDL_GL_MakeCurrent(win, gl_context);
    if (!am_gl_initialized) {
        if (!init_gl()) {
            SDL_DestroyWindow(win);
            return NULL;
        }
        am_gl_initialized = true;
    }
    windows.push_back(win);
    return (am_native_window*)win;
}

void am_destroy_native_window(am_native_window* window) {
    for (unsigned int i = 0; i < windows.size(); i++) {
        if (windows[i] == (SDL_Window*)window) {
            SDL_DestroyWindow((SDL_Window*)window);
            windows.erase(windows.begin() + i);
            break;
        }
    }
}

void am_native_window_pre_render(am_native_window* window) {
    SDL_GL_MakeCurrent((SDL_Window*)window, gl_context);
    am_clear_framebuffer(true, true, true);
}

void am_native_window_post_render(am_native_window* window) {
    SDL_GL_SwapWindow((SDL_Window*)window);
}

double am_get_time() {
    return ((double)SDL_GetTicks())/1000.0;
}

static bool init_gl() {
    GLenum err = glewInit();
    if (GLEW_OK != err)
    {
        am_abort("Error initializing OpenGL: %s", glewGetErrorString(err));
        return false;
    }

    if (!GLEW_VERSION_2_1)
    {
        am_abort("Sorry, OpenGL 2.1 is required.");
        return false;
    }
    return true;
}

static double fps = 1.0;

int main( int argc, char *argv[] )
{
    int vsync;
    double t0;
    double t;
    double t_debt;
    double fps_t0;
    double fps_max;
    double dt;
    long long frame_count;
    int status;
    lua_State *L = NULL;

    int exit_status = EXIT_SUCCESS;

#ifdef AM_OSX
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
#endif

    if (!process_args(argc, argv)) {
        exit_status = EXIT_FAILURE;
        goto quit;
    }

    L = am_init_engine(false);
    if (L == NULL) {
        exit_status = EXIT_FAILURE;
        goto quit;
    }

    status = luaL_dofile(L, filename);
    if (report_status(L, status)) goto quit;
    if (windows.size() == 0) goto quit;

    t0 = am_get_time();
    t = t0;
    t_debt = 0.0;
    fps_t0 = 0.0;
    fps_max = 0.0;
    frame_count = 0;
    vsync = -2;

    while (windows.size() > 0) {
        if (vsync != (am_conf_vsync ? 1 : 0)) {
            vsync = (am_conf_vsync ? 1 : 0);
            SDL_GL_SetSwapInterval(vsync);
        }

        if (!am_update_windows(L)) {
            exit_status = EXIT_FAILURE;
            goto quit;
        }

        SDL_LockAudio();
        am_sync_audio_graph(L);
        SDL_UnlockAudio();

        if (!handle_events()) goto quit;

        t = am_get_time();
        dt = fmin(1.0/15.0, t - t0); // fmin in case process was suspended, or last frame took very long
        t_debt += dt;

        if (am_conf_fixed_update_time > 0.0) {
            while (t_debt > 0.0) {
                if (!am_execute_actions(L, am_conf_fixed_update_time)) {
                    exit_status = EXIT_FAILURE;
                    goto quit;
                }
                t_debt -= am_conf_fixed_update_time;
            }
        } else {
            if (t_debt > MIN_UPDATE_TIME) {
                if (!am_execute_actions(L, t_debt)) {
                    exit_status = EXIT_FAILURE;
                    goto quit;
                }
                t_debt = 0.0;
            }
        }

        fps_max = fmax(fps_max, dt);
        t0 = t;

        frame_count++;

        if (t - fps_t0 >= 2.0) {
            fps = (double)frame_count / (t - fps_t0);
            //am_debug("fps = %0.2f", fps);
            fps_t0 = t0;
            fps_max = 0.0;
            frame_count = 0;
        }

    }


quit:
    if (L != NULL) am_destroy_engine(L);
    for (unsigned int i = 0; i < windows.size(); i++) {
        SDL_DestroyWindow(windows[i]);
    }
    if (sdl_initialized) SDL_Quit();
#ifdef LTOSX
    [pool release];
#endif
    return exit_status;
}

static int report_status(lua_State *L, int status) {
    if (status && !lua_isnil(L, -1)) {
        const char *msg = lua_tostring(L, -1);
        if (msg == NULL) msg = "(error object is not a string)";
        fprintf(stderr, "%s\n", msg);
        lua_pop(L, 1);
    }
    return status;
}

static void init_sdl() {
    SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
    sdl_initialized = true;
    init_gamepad();
    init_audio();
}

static float *audio_buffer = NULL;

static void audio_callback(void *ud, Uint8 *stream, int len) {
    assert(audio_buffer != NULL);
    assert(len == (int)sizeof(float) * am_conf_audio_buffer_size * am_conf_audio_channels);
    int num_channels = am_conf_audio_channels;
    int num_samples = am_conf_audio_buffer_size;
    float *buffer = audio_buffer;
    memset(buffer, 0, len);
    am_audio_bus bus;
    bus.num_channels = num_channels;
    bus.num_samples = num_samples;
    bus.buffer = buffer;
    for (int i = 0; i < num_channels; i++) {
        bus.channel_data[i] = bus.buffer + i * num_samples;
    }
    am_fill_audio_bus(&bus);
    // SDL expects channels to be interleaved
    for (int c = 0; c < num_channels; c++) {
        for (int i = 0; i < num_samples; i++) {
            ((float*)stream)[i * num_channels + c] = bus.channel_data[c][i];
        }
    }
}

static void init_audio() {
    audio_buffer = (float*)malloc(sizeof(float) * am_conf_audio_channels * am_conf_audio_buffer_size);
    SDL_AudioSpec desired;
    desired.freq = am_conf_audio_sample_rate;
    desired.format = AUDIO_F32SYS;
    desired.channels = am_conf_audio_channels;
    desired.samples = am_conf_audio_channels * am_conf_audio_buffer_size;
    desired.callback = audio_callback;
    desired.userdata = NULL;
    SDL_AudioSpec obtained;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    am_init_audio(obtained.freq);
    if (dev == 0) {
        am_report_error("Failed to open audio: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(dev, 0);
}

static bool handle_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT: return false;
            case SDL_WINDOWEVENT: {
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_CLOSE:
                        for (unsigned int i = 0; i < windows.size(); i++) {
                            if (SDL_GetWindowID(windows[i]) == event.window.windowID) {
                                am_handle_window_close((am_native_window*)windows[i]);
                            }
                        }
                        break;
                }
                break;
            }
            case SDL_KEYDOWN: {
                /*
                if (!event.key.repeat) {
                    const char *key = SDL_GetKeyName(event.key.keysym.sym);
                    lua_pushstring(L, key);
                    am_call_amulet(L, "_key_down", 1, 0);
                }
                */
                break;
            }
            case SDL_KEYUP: {
                /*
                if (!event.key.repeat) {
                    const char *key = SDL_GetKeyName(event.key.keysym.sym);
                    lua_pushstring(L, key);
                    am_call_amulet(L, "_key_up", 1, 0);
                }
                */
                break;
            }
        }
    }
    return true;
}

/*
static void handle_events(lua_State *L) {
    am_call_amulet(L, "_clear_keys", 0, 0);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT: {
                // close all windows and stop processing any more events
                for (unsigned int i = 0; i < windows.size(); i++) {
                    windows[i]->needs_closing = true;
                }
                close_windows(L);
                break;
            }
            case SDL_WINDOWEVENT: {
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_CLOSE: {
                        for (unsigned int i = 0; i < windows.size(); i++) {
                            if (SDL_GetWindowID(windows[i]->sdl_win) == event.window.windowID) {
                                if (windows[i]->open) {
                                    windows[i]->needs_closing = true;
                                    close_windows(L);
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
                break;
            }
            case SDL_KEYDOWN: {
                if (!event.key.repeat) {
                    const char *key = SDL_GetKeyName(event.key.keysym.sym);
                    lua_pushstring(L, key);
                    am_call_amulet(L, "_key_down", 1, 0);
                }
                break;
            }
            case SDL_KEYUP: {
                if (!event.key.repeat) {
                    const char *key = SDL_GetKeyName(event.key.keysym.sym);
                    lua_pushstring(L, key);
                    am_call_amulet(L, "_key_up", 1, 0);
                }
                break;
            }
        }
    }
}
*/

/*
static void handle_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_KEYDOWN: {
                if (!event.key.repeat) {
                    LTKey ltkey = convert_key(event.key.keysym.sym);
                    ltLuaKeyDown(ltkey);
                }
                break;
            }
            case SDL_KEYUP: {
                if (!event.key.repeat) {
                    LTKey ltkey = convert_key(event.key.keysym.sym);
                    ltLuaKeyUp(ltkey);
                }
                break;
            }
            case SDL_WINDOWEVENT: {
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_CLOSE: {
                        lt_quit = true;
                        break;
                    }
                    case SDL_WINDOWEVENT_RESIZED: {
                        SDL_GL_GetDrawableSize(window, &framebuffer_width, &framebuffer_height);
                        SDL_GetWindowSize(window, &screen_window_width, &screen_window_height);
                        int w = event.window.data1;
                        int h = event.window.data2;
                        ltLuaResizeWindow(framebuffer_width, framebuffer_height);
                        break;
                    }
                }
                break;
            }
            case SDL_CONTROLLERDEVICEADDED: {
                if (!controller_present) {
                    int joystickid = event.cdevice.which;
                    SDL_GameController *controller = SDL_GameControllerOpen(joystickid);
                    if (controller != NULL) {
                        controller_present = true;
                    }
                }
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                controller_present = false;
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN: {
                if (lt_controller_enabled) {
                    ltLuaKeyDown(controller_button_to_key((SDL_GameControllerButton)event.cbutton.button));
                }
                break;
            }
            case SDL_CONTROLLERBUTTONUP: {
                if (lt_controller_enabled) {
                    ltLuaKeyUp(controller_button_to_key((SDL_GameControllerButton)event.cbutton.button));
                }
                break;
            }
            case SDL_CONTROLLERAXISMOTION: {
                Uint8 axis = event.caxis.axis;
                float val = (float)event.caxis.value / 32768.0f;
                float movement = val - axis_hwm[axis];
                float movement_mag = fabs(movement);
                switch (axis) {
                    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                        if (val > 0.5f && !axis_button_down[AXIS_BUTTON_LEFT]) {
                            axis_button_down[AXIS_BUTTON_LEFT] = true;
                            ltLuaKeyDown(LT_KEY_LEFT);
                        } else if (val < 0.4f && axis_button_down[AXIS_BUTTON_LEFT]) {
                            axis_button_down[AXIS_BUTTON_LEFT] = false;
                            ltLuaKeyUp(LT_KEY_LEFT);
                        }
                        break;
                    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                        if (val > 0.5f && !axis_button_down[AXIS_BUTTON_RIGHT]) {
                            axis_button_down[AXIS_BUTTON_RIGHT] = true;
                            ltLuaKeyDown(LT_KEY_RIGHT);
                        } else if (val < 0.4f && axis_button_down[AXIS_BUTTON_RIGHT]) {
                            axis_button_down[AXIS_BUTTON_RIGHT] = false;
                            ltLuaKeyUp(LT_KEY_RIGHT);
                        }
                        break;
                    case SDL_CONTROLLER_AXIS_LEFTX:
                        if (val < -0.5f && !axis_button_down[AXIS_BUTTON_LEFT]) {
                            axis_button_down[AXIS_BUTTON_LEFT] = true;
                            if (axis_button_down[AXIS_BUTTON_RIGHT]) {
                                axis_button_down[AXIS_BUTTON_RIGHT] = false;
                                ltLuaKeyUp(LT_KEY_RIGHT);
                            }
                            ltLuaKeyDown(LT_KEY_LEFT);
                            axis_hwm[axis] = val;
                        } else if (val > 0.5f && !axis_button_down[AXIS_BUTTON_RIGHT]) {
                            axis_button_down[AXIS_BUTTON_RIGHT] = true;
                            if (axis_button_down[AXIS_BUTTON_LEFT]) {
                                axis_button_down[AXIS_BUTTON_LEFT] = false;
                                ltLuaKeyUp(LT_KEY_LEFT);
                            }
                            ltLuaKeyDown(LT_KEY_RIGHT);
                            axis_hwm[axis] = val;
                        } else if (val > -0.4f && val < 0.4f && movement_mag > 0.2f) {
                            if (axis_button_down[AXIS_BUTTON_LEFT]) {
                                axis_button_down[AXIS_BUTTON_LEFT] = false;
                                ltLuaKeyUp(LT_KEY_LEFT);
                            }
                            if (axis_button_down[AXIS_BUTTON_RIGHT]) {
                                axis_button_down[AXIS_BUTTON_RIGHT] = false;
                                ltLuaKeyUp(LT_KEY_RIGHT);
                            }
                            axis_hwm[axis] = val;
                        }
                        break;
                    case SDL_CONTROLLER_AXIS_LEFTY:
                        if (val < -0.5f && !axis_button_down[AXIS_BUTTON_UP]) {
                            axis_button_down[AXIS_BUTTON_UP] = true;
                            if (axis_button_down[AXIS_BUTTON_DOWN]) {
                                axis_button_down[AXIS_BUTTON_DOWN] = false;
                                ltLuaKeyUp(LT_KEY_DOWN);
                            }
                            ltLuaKeyDown(LT_KEY_UP);
                            axis_hwm[axis] = val;
                        } else if (val > 0.5f && !axis_button_down[AXIS_BUTTON_DOWN]) {
                            axis_button_down[AXIS_BUTTON_DOWN] = true;
                            if (axis_button_down[AXIS_BUTTON_UP]) {
                                axis_button_down[AXIS_BUTTON_UP] = false;
                                ltLuaKeyUp(LT_KEY_UP);
                            }
                            ltLuaKeyDown(LT_KEY_DOWN);
                            axis_hwm[axis] = val;
                        } else if (val > -0.4f && val < 0.4f && movement_mag > 0.2f) {
                            if (axis_button_down[AXIS_BUTTON_UP]) {
                                axis_button_down[AXIS_BUTTON_UP] = false;
                                ltLuaKeyUp(LT_KEY_UP);
                            }
                            if (axis_button_down[AXIS_BUTTON_DOWN]) {
                                axis_button_down[AXIS_BUTTON_DOWN] = false;
                                ltLuaKeyUp(LT_KEY_DOWN);
                            }
                            axis_hwm[axis] = val;
                        }
                        break;
                    case SDL_CONTROLLER_AXIS_RIGHTX:
                        if (val < -0.5f && !axis_button_down[AXIS_BUTTON_LEFT]) {
                            axis_button_down[AXIS_BUTTON_LEFT] = true;
                            if (axis_button_down[AXIS_BUTTON_RIGHT]) {
                                axis_button_down[AXIS_BUTTON_RIGHT] = false;
                                ltLuaKeyUp(LT_KEY_RIGHT);
                            }
                            ltLuaKeyDown(LT_KEY_LEFT);
                            axis_hwm[axis] = val;
                        } else if (val > 0.5f && !axis_button_down[AXIS_BUTTON_RIGHT]) {
                            axis_button_down[AXIS_BUTTON_RIGHT] = true;
                            if (axis_button_down[AXIS_BUTTON_LEFT]) {
                                axis_button_down[AXIS_BUTTON_LEFT] = false;
                                ltLuaKeyUp(LT_KEY_LEFT);
                            }
                            ltLuaKeyDown(LT_KEY_RIGHT);
                            axis_hwm[axis] = val;
                        } else if (val > -0.4f && val < 0.4f && movement_mag > 0.2f) {
                            if (axis_button_down[AXIS_BUTTON_LEFT]) {
                                axis_button_down[AXIS_BUTTON_LEFT] = false;
                                ltLuaKeyUp(LT_KEY_LEFT);
                            }
                            if (axis_button_down[AXIS_BUTTON_RIGHT]) {
                                axis_button_down[AXIS_BUTTON_RIGHT] = false;
                                ltLuaKeyUp(LT_KEY_RIGHT);
                            }
                            axis_hwm[axis] = val;
                        }
                        break;
                    case SDL_CONTROLLER_AXIS_RIGHTY:
                        if (val < -0.5f && !axis_button_down[AXIS_BUTTON_UP]) {
                            axis_button_down[AXIS_BUTTON_UP] = true;
                            if (axis_button_down[AXIS_BUTTON_DOWN]) {
                                axis_button_down[AXIS_BUTTON_DOWN] = false;
                                ltLuaKeyUp(LT_KEY_DOWN);
                            }
                            ltLuaKeyDown(LT_KEY_UP);
                            axis_hwm[axis] = val;
                        } else if (val > 0.5f && !axis_button_down[AXIS_BUTTON_DOWN]) {
                            axis_button_down[AXIS_BUTTON_DOWN] = true;
                            if (axis_button_down[AXIS_BUTTON_UP]) {
                                axis_button_down[AXIS_BUTTON_UP] = false;
                                ltLuaKeyUp(LT_KEY_UP);
                            }
                            ltLuaKeyDown(LT_KEY_DOWN);
                            axis_hwm[axis] = val;
                        } else if (val > -0.4f && val < 0.4f && movement_mag > 0.2f) {
                            if (axis_button_down[AXIS_BUTTON_UP]) {
                                axis_button_down[AXIS_BUTTON_UP] = false;
                                ltLuaKeyUp(LT_KEY_UP);
                            }
                            if (axis_button_down[AXIS_BUTTON_DOWN]) {
                                axis_button_down[AXIS_BUTTON_DOWN] = false;
                                ltLuaKeyUp(LT_KEY_DOWN);
                            }
                            axis_hwm[axis] = val;
                        }
                        break;
                }
                break;
            }
            case SDL_QUIT: {
                lt_quit = true;
                break;
            }
        }
    }
}

static int to_framebuf_x(double x) {
    return (x/(double)screen_window_width)*(double)framebuffer_width;
}

static int to_framebuf_y(double y) {
    return (y/(double)screen_window_height)*(double)framebuffer_height;
}

static LTKey convert_key(SDL_Keycode key) {
    switch(key) {
        case SDLK_TAB: return LT_KEY_TAB;
        case SDLK_RETURN: return LT_KEY_ENTER;
        case SDLK_ESCAPE: return LT_KEY_ESC;
        case SDLK_SPACE: return LT_KEY_SPACE;
        case SDLK_QUOTE: return LT_KEY_APOS;
        case SDLK_EQUALS: return LT_KEY_PLUS;
        case SDLK_COMMA: return LT_KEY_COMMA;
        case SDLK_MINUS: return LT_KEY_MINUS;
        case SDLK_PERIOD: return LT_KEY_PERIOD;
        case SDLK_SLASH: return LT_KEY_SLASH;
        case SDLK_0: return LT_KEY_0;
        case SDLK_1: return LT_KEY_1;
        case SDLK_2: return LT_KEY_2;
        case SDLK_3: return LT_KEY_3;
        case SDLK_4: return LT_KEY_4;
        case SDLK_5: return LT_KEY_5;
        case SDLK_6: return LT_KEY_6;
        case SDLK_7: return LT_KEY_7;
        case SDLK_8: return LT_KEY_8;
        case SDLK_9: return LT_KEY_9;
        case SDLK_SEMICOLON: return LT_KEY_SEMI_COLON;
        case SDLK_LEFTBRACKET: return LT_KEY_LEFT_BRACKET;
        case SDLK_BACKSLASH: return LT_KEY_BACKSLASH;
        case SDLK_RIGHTBRACKET: return LT_KEY_RIGHT_BRACKET;
        case SDLK_BACKQUOTE: return LT_KEY_TICK;
        case SDLK_a: return LT_KEY_A;
        case SDLK_b: return LT_KEY_B;
        case SDLK_c: return LT_KEY_C;
        case SDLK_d: return LT_KEY_D;
        case SDLK_e: return LT_KEY_E;
        case SDLK_f: return LT_KEY_F;
        case SDLK_g: return LT_KEY_G;
        case SDLK_h: return LT_KEY_H;
        case SDLK_i: return LT_KEY_I;
        case SDLK_j: return LT_KEY_J;
        case SDLK_k: return LT_KEY_K;
        case SDLK_l: return LT_KEY_L;
        case SDLK_m: return LT_KEY_M;
        case SDLK_n: return LT_KEY_N;
        case SDLK_o: return LT_KEY_O;
        case SDLK_p: return LT_KEY_P;
        case SDLK_q: return LT_KEY_Q;
        case SDLK_r: return LT_KEY_R;
        case SDLK_s: return LT_KEY_S;
        case SDLK_t: return LT_KEY_T;
        case SDLK_u: return LT_KEY_U;
        case SDLK_v: return LT_KEY_V;
        case SDLK_w: return LT_KEY_W;
        case SDLK_x: return LT_KEY_X;
        case SDLK_y: return LT_KEY_Y;
        case SDLK_z: return LT_KEY_Z;
        case SDLK_DELETE: return LT_KEY_DEL;
        case SDLK_BACKSPACE: return LT_KEY_DEL;
        case SDLK_UP: return LT_KEY_UP;
        case SDLK_DOWN: return LT_KEY_DOWN;
        case SDLK_RIGHT: return LT_KEY_RIGHT;
        case SDLK_LEFT: return LT_KEY_LEFT;
    }
    return LT_KEY_UNKNOWN;
}
*/

static bool process_args(int argc, char **argv) {
    bool in_osx_bundle = false;
#ifdef LTOSX
    NSString *bundleIdentifier = [[NSBundle mainBundle] bundleIdentifier];
    in_osx_bundle = (bundleIdentifier != nil);
#endif
    if (!in_osx_bundle) { // When invoked from an OSX bundle extra arguments may be set by the OS.
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-') {
                am_report_error("Unrecognised option: %s", argv[i]);
                return false;
            } else if (filename != NULL) {
                am_report_error("Unrecognised option: %s", argv[i]);
                return false;
            } else {
                filename = argv[i];
            }
        }
    }
    if (filename == NULL) {
        am_report_error("Expecting a filename");
        return false;
    }
    return true;
}

/*
static LTKey controller_button_to_key(SDL_GameControllerButton button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return LT_KEY_ENTER;
        case SDL_CONTROLLER_BUTTON_B: return LT_KEY_ESC;
        case SDL_CONTROLLER_BUTTON_X: return LT_KEY_ENTER;
        case SDL_CONTROLLER_BUTTON_BACK: return LT_KEY_ESC;
        case SDL_CONTROLLER_BUTTON_START: return LT_KEY_ENTER;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return LT_KEY_LEFT;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return LT_KEY_RIGHT;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return LT_KEY_UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return LT_KEY_DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return LT_KEY_LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return LT_KEY_RIGHT;
        default:;
    }
    return LT_KEY_UNKNOWN;
}
*/

static const char *controller_mappings[] =
{
#if SDL_JOYSTICK_XINPUT
    "xinput,XInput Controller,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b10,leftshoulder:b4,leftstick:b8,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b9,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
#endif
#if SDL_JOYSTICK_DINPUT
    "341a3608000000000000504944564944,Afterglow PS3 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b0,y:b3,",
    "ffff0000000000000000504944564944,GameStop Gamepad,a:b0,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b2,y:b3,",
    "6d0416c2000000000000504944564944,Generic DirectInput Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b0,y:b3,",
	"6d0419c2000000000000504944564944,Logitech F710 Gamepad,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b0,y:b3,", /* Guide button doesn't seem to be sent in DInput mode. */
    "4d6963726f736f66742050432d6a6f79,OUYA Controller,a:b0,b:b3,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b14,leftshoulder:b4,leftstick:b6,lefttrigger:b12,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b7,righttrigger:b13,rightx:a5,righty:a4,x:b1,y:b2,",
    "88880803000000000000504944564944,PS3 Controller,a:b2,b:b1,back:b8,dpdown:h0.8,dpleft:h0.4,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b9,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:b7,rightx:a3,righty:a4,start:b11,x:b0,y:b3,",
    "4c056802000000000000504944564944,PS3 Controller,a:b14,b:b13,back:b0,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,guide:b16,leftshoulder:b10,leftstick:b1,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b11,rightstick:b2,righttrigger:b9,rightx:a2,righty:a3,start:b3,x:b15,y:b12,",
    "25090500000000000000504944564944,PS3 DualShock,a:b2,b:b1,back:b9,dpdown:h0.8,dpleft:h0.4,dpright:h0.2,dpup:h0.1,guide:,leftshoulder:b6,leftstick:b10,lefttrigger:b4,leftx:a0,lefty:a1,rightshoulder:b7,rightstick:b11,righttrigger:b5,rightx:a2,righty:a3,start:b8,x:b0,y:b3,",
    "4c05c405000000000000504944564944,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,",
#endif
#if defined(__MACOSX__)
    "0500000047532047616d657061640000,GameStop Gamepad,a:b0,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b2,y:b3,",
    "6d0400000000000016c2000000000000,Logitech F310 Gamepad (DInput),a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b0,y:b3,", /* Guide button doesn't seem to be sent in DInput mode. */
    "6d0400000000000018c2000000000000,Logitech F510 Gamepad (DInput),a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b0,y:b3,",
    "6d040000000000001fc2000000000000,Logitech F710 Gamepad (XInput),a:b0,b:b1,back:b9,dpdown:b12,dpleft:b13,dpright:b14,dpup:b11,guide:b10,leftshoulder:b4,leftstick:b6,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b7,righttrigger:a5,rightx:a3,righty:a4,start:b8,x:b2,y:b3,",
    "6d0400000000000019c2000000000000,Logitech Wireless Gamepad (DInput),a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b0,y:b3,", /* This includes F710 in DInput mode and the "Logitech Cordless RumblePad 2", at the very least. */
    "4c050000000000006802000000000000,PS3 Controller,a:b14,b:b13,back:b0,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,guide:b16,leftshoulder:b10,leftstick:b1,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b11,rightstick:b2,righttrigger:b9,rightx:a2,righty:a3,start:b3,x:b15,y:b12,",
    "4c05000000000000c405000000000000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,",
    "5e040000000000008e02000000000000,X360 Controller,a:b0,b:b1,back:b9,dpdown:b12,dpleft:b13,dpright:b14,dpup:b11,guide:b10,leftshoulder:b4,leftstick:b6,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b7,righttrigger:a5,rightx:a3,righty:a4,start:b8,x:b2,y:b3,",
#endif
#if defined(__LINUX__)
    "0500000047532047616d657061640000,GameStop Gamepad,a:b0,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b2,y:b3,",
    "03000000ba2200002010000001010000,Jess Technology USB Game Controller,a:b2,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:,leftshoulder:b4,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,righttrigger:b7,rightx:a3,righty:a2,start:b9,x:b3,y:b0,",
    "030000006d04000019c2000010010000,Logitech Cordless RumblePad 2,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b0,y:b3,",
    "030000006d0400001dc2000014400000,Logitech F310 Gamepad (XInput),a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
    "030000006d0400001ec2000020200000,Logitech F510 Gamepad (XInput),a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
    "030000006d04000019c2000011010000,Logitech F710 Gamepad (DInput),a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2,righty:a3,start:b9,x:b0,y:b3,", /* Guide button doesn't seem to be sent in DInput mode. */
    "030000006d0400001fc2000005030000,Logitech F710 Gamepad (XInput),a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
    "050000003620000100000002010000,OUYA Game Controller,a:b0,b:b3,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b14,leftshoulder:b4,leftstick:b6,lefttrigger:a2,leftx:a0,lefty:a1,platform:Linux,rightshoulder:b5,rightstick:b7,righttrigger:a5,rightx:a3,righty:a4,x:b1,y:b2,",
    "030000004c0500006802000011010000,PS3 Controller,a:b14,b:b13,back:b0,dpdown:b6,dpleft:b7,dpright:b5,dpup:b4,guide:b16,leftshoulder:b10,leftstick:b1,lefttrigger:b8,leftx:a0,lefty:a1,rightshoulder:b11,rightstick:b2,righttrigger:b9,rightx:a2,righty:a3,start:b3,x:b15,y:b12,",
    "030000004c050000c405000011010000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,",
    "050000004c050000c405000000010000,PS4 Controller,a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b12,leftshoulder:b4,leftstick:b10,lefttrigger:a3,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b11,righttrigger:a4,rightx:a2,righty:a5,start:b9,x:b0,y:b3,",
    "03000000de280000ff11000001000000,Valve Streaming Gamepad,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
    "030000005e0400008e02000014010000,X360 Controller,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
    "030000005e0400008e02000010010000,X360 Controller,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
    "030000005e0400001907000000010000,X360 Wireless Controller,a:b0,b:b1,back:b6,dpdown:b14,dpleft:b11,dpright:b12,dpup:b13,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
    "030000005e0400009102000007010000,X360 Wireless Controller,a:b0,b:b1,back:b6,dpdown:b14,dpleft:b11,dpright:b12,dpup:b13,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
#endif
#if defined(__ANDROID__)
    "4e564944494120436f72706f72617469,NVIDIA Controller,a:b0,b:b1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b9,leftstick:b7,lefttrigger:a4,leftx:a0,lefty:a1,rightshoulder:b10,rightstick:b8,righttrigger:a5,rightx:a2,righty:a3,start:b6,x:b2,y:b3,",
#endif
    NULL
};

static void init_gamepad() {
    for (int i = 0; i < NUM_AXIS_BUTTONS; i++) {
        axis_button_down[i] = false;
    }
    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++) {
        axis_hwm[i] = 0.0f;
    }
    load_controller_mappings();
    if (SDL_NumJoysticks() > 0) {
        //am_debug("detected joysticks = %d", SDL_NumJoysticks());
        char guid[100];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(0),
                                          guid, sizeof (guid));
        //am_debug("guid = %s", guid);
        SDL_GameController *controller = SDL_GameControllerOpen(0);
        if (controller != NULL) {
            //am_debug("detected controller");
            controller_present = true;
        } else {
            //am_debug("controller error: %s", SDL_GetError());
        }
    }
}

static void load_controller_mappings() {
    const char **mapping = &controller_mappings[0];
    while (*mapping != NULL) {
        SDL_GameControllerAddMapping(*mapping);
        mapping++;
    }
}

#endif // AM_BACKEND_SDL
