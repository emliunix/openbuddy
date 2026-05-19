#pragma once
#include "protocol_types.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// --------------------------------------------------------------------
// PersonaVariant — 7 animation states, fixed indices into Species.states[]
// --------------------------------------------------------------------

enum PersonaVariant {
    P_SLEEP = 0,
    P_IDLE,
    P_BUSY,
    P_ATTENTION,
    P_CELEBRATE,
    P_DIZZY,
    P_HEART,
};

const char* persona_name(PersonaVariant v);

// --------------------------------------------------------------------
// State objects
// --------------------------------------------------------------------

struct NetworkState {
    bool     connected       = false;
    uint64_t last_live_ms    = 0;
    uint32_t running         = 0;
    uint32_t waiting         = 0;
    uint32_t total           = 0;
    bool     completed       = false;
    uint32_t tokens_today    = 0;
    uint32_t tokens_bridge   = 0;
    char     msg[24]         = {};
    char     lines[8][92]    = {};
    uint8_t  n_lines         = 0;
    uint16_t line_gen        = 0;
    char     prompt_id[40]   = {};
    char     prompt_tool[20] = {};
    char     prompt_hint[44] = {};
    bool     response_sent   = false;
    uint64_t prompt_arrived_ms = 0;
};

struct PersonaState {
    PersonaVariant base_state   = P_IDLE;
    PersonaVariant active_state = P_IDLE;
    uint64_t       anim_until   = 0;
};

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO };

struct UiState {
    DisplayMode display_mode  = DISP_NORMAL;
    bool        help_open     = false;
    int8_t      species_cycle = 0;  // +1/−1 set by apply_key, consumed by renderer
};

struct AppState {
    NetworkState net;
    PersonaState persona;
    UiState      ui;
};

// --------------------------------------------------------------------
// Pure functions
// --------------------------------------------------------------------

// Priority-ordered persona selection from live network state.
// Never returns P_SLEEP — that is applied by the time-of-day rule in apply_tick.
PersonaVariant select_persona(const NetworkState& net);

// --------------------------------------------------------------------
// Event handlers
// --------------------------------------------------------------------

using MsgSender = std::function<void(const BuddyMsg&)>;
void app_set_sender(MsgSender sender);

// Process an incoming wire message. Returns true if visual state changed.
bool apply_message(AppState* app, const DaemonMsg& msg);

// ANIM_TICK handler: recomputes connected, calls select_persona, applies
// time-of-day rule, expires anim timers. Returns true if visual state changed.
bool apply_tick(AppState* app, uint64_t now);

// Keyboard input. Returns true if visual state changed.
bool apply_key(AppState* app, int key);
