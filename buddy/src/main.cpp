#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "log.h"
#include "renderer.h"
#include "session.h"
#include "session_renderer.h"
#include "transport.h"
#include "asio_transport.h"
#include "mock_transport.h"
#include "persistence.h"
#include "stats.h"
#include "buddy_renderer.h"

static const int LOGIC_W = 135;
static const int LOGIC_H = 240;
static const int DEFAULT_SCALE = 3;

// SDL custom events. Register 2: [0]=timer, [1]=transport
// Must not use SDL_USEREVENT directly to avoid collision.
static Uint32 g_event_timer = 0;
static Uint32 g_event_transport_msg = 0;

static SDL_Window*   g_window = nullptr;
static SDL_Renderer* g_sdl_renderer = nullptr;
static Renderer*     g_renderer = nullptr;
static int           g_scale = DEFAULT_SCALE;

static AppState              g_app;
static std::unique_ptr<ITransport> g_transport;

static bool g_needs_render = true;

// ------------------------------------------------------------------
// Init / shutdown
// ------------------------------------------------------------------

static bool init_sdl() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        LOG_ERROR("SDL_Init: %s", SDL_GetError());
        return false;
    }

    Uint32 events = SDL_RegisterEvents(2);
    if (events == (Uint32)-1) {
        LOG_ERROR("SDL_RegisterEvents failed");
        return false;
    }
    g_event_timer = events;
    g_event_transport_msg = events + 1;

    int window_w = LOGIC_W * g_scale;
    int window_h = LOGIC_H * g_scale;

    g_window = SDL_CreateWindow(
        "OpenBuddy",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        window_w, window_h,
        SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
    if (!g_window) { LOG_ERROR("SDL_CreateWindow: %s", SDL_GetError()); return false; }

    g_sdl_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_sdl_renderer) { LOG_ERROR("SDL_CreateRenderer: %s", SDL_GetError()); return false; }

    SDL_RenderSetLogicalSize(g_sdl_renderer, LOGIC_W, LOGIC_H);

    float ddpi, hdpi, vdpi;
    if (SDL_GetDisplayDPI(0, &ddpi, &hdpi, &vdpi) == 0) {
        float detected = ddpi / 96.0f;
        if (detected > 1.5f) {
            g_scale = (int)(detected + 0.5f);
            if (g_scale < 2) g_scale = 2;
            if (g_scale > 6) g_scale = 6;
            SDL_SetWindowSize(g_window, LOGIC_W * g_scale, LOGIC_H * g_scale);
        }
    }

    g_renderer = new Renderer(g_sdl_renderer, LOGIC_W, LOGIC_H);
    SDL_EnableScreenSaver();  // SDL disables screen saver by default; buddy must not hold a wake lock
    SDL_ShowWindow(g_window);
    return true;
}

static void shutdown_sdl() {
    delete g_renderer;
    if (g_sdl_renderer) SDL_DestroyRenderer(g_sdl_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
}

// ------------------------------------------------------------------
// Transport message bridge: callback -> SDL_UserEvent
// ------------------------------------------------------------------

static void on_transport_msg(DaemonMsg msg) {
    // Ownership transferred to SDL event queue via unique_ptr
    auto ptr = std::make_unique<DaemonMsg>(std::move(msg));
    SDL_Event e;
    e.type = g_event_transport_msg;
    e.user.data1 = ptr.release();
    SDL_PushEvent(&e);
}

// ------------------------------------------------------------------
// Event loop
// ------------------------------------------------------------------

static Uint32 timer_callback(Uint32, void*) {
    SDL_Event e;
    e.type = g_event_timer;
    SDL_PushEvent(&e);
    return 50;  // 20 Hz logic tick
}

static void process_timer(uint32_t now) {
    apply_tick(&g_app, now);
    g_needs_render = true;  // always redraw: animation runs every tick regardless of state changes
}

static void process_transport_msg(const DaemonMsg& msg) {
    if (apply_message(&g_app, msg)) g_needs_render = true;
}

static SDL_LogPriority parse_log_level(const char* s) {
    if (std::strcmp(s, "verbose") == 0) return SDL_LOG_PRIORITY_VERBOSE;
    if (std::strcmp(s, "debug") == 0)   return SDL_LOG_PRIORITY_DEBUG;
    if (std::strcmp(s, "info") == 0)    return SDL_LOG_PRIORITY_INFO;
    if (std::strcmp(s, "warn") == 0)    return SDL_LOG_PRIORITY_WARN;
    if (std::strcmp(s, "error") == 0)   return SDL_LOG_PRIORITY_ERROR;
    if (std::strcmp(s, "critical") == 0) return SDL_LOG_PRIORITY_CRITICAL;
    return SDL_LOG_PRIORITY_INFO; // default
}

int main(int argc, char** argv) {
    bool test_mode = false;
    bool use_mock = false;
    SDL_LogPriority log_level = SDL_LOG_PRIORITY_INFO;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--test") == 0) test_mode = true;
        else if (std::strcmp(argv[i], "--mock") == 0) use_mock = true;
        else if (std::strncmp(argv[i], "--log-level=", 12) == 0)
            log_level = parse_log_level(argv[i] + 12);
    }

    SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, log_level);

    if (!init_sdl()) return EXIT_FAILURE;

    g_app = {};
    stats_init();
    persistence::load(&g_app);
    buddy_set_species_idx(settings().species_idx);  // restore persisted species

    // Env var override for pet name (owner name is set via wire OwnerCmd from plugin)
    if (const char* v = std::getenv("BUDDY_PET_NAME")) pet_name_set(v);

    // Wire session sender to transport
    app_set_sender([](const BuddyMsg& msg) {
        if (g_transport) g_transport->send(msg);
    });

    // Create transport
    if (use_mock) {
        g_transport = std::make_unique<MockTransport>(on_transport_msg);
        g_transport->start();
    } else {
        g_transport = std::make_unique<AsioTransport>(on_transport_msg, 7887);
        g_transport->start();
    }

    SDL_TimerID timer_id = SDL_AddTimer(50, timer_callback, nullptr);
    Uint32 start = SDL_GetTicks();
    bool running = true;

    while (running) {
        SDL_Event e;
        if (!SDL_WaitEvent(&e)) continue;

        do {
            if (e.type == SDL_QUIT) { running = false; break; }
            if (e.type == SDL_KEYDOWN) {
                if (apply_key(&g_app, e.key.keysym.sym)) g_needs_render = true;
            }
            if (e.type == g_event_timer) {
                uint32_t now = SDL_GetTicks();
                process_timer(now);
                if (test_mode && now - start > 3000) { running = false; break; }
            }
            if (e.type == g_event_transport_msg) {
                std::unique_ptr<DaemonMsg> msg(static_cast<DaemonMsg*>(e.user.data1));
                if (msg) {
                    process_transport_msg(*msg);
                }
            }
        } while (SDL_PollEvent(&e));

        if (g_needs_render) {
            render_session(&g_app, g_renderer);
            SDL_RenderPresent(g_sdl_renderer);
            g_needs_render = false;
        }
    }

    SDL_RemoveTimer(timer_id);
    g_transport.reset();  // RAII: emits disconnect if connected
    settings().species_idx = buddy_species_idx();  // capture current selection before save
    persistence::save(g_app);
    shutdown_sdl();
    return EXIT_SUCCESS;
}
