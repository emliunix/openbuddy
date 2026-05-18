#include "session.h"
#include "stats.h"
#include "log.h"
#include <SDL.h>
#include <cstring>
#include <ctime>

// Nap idle timer — Candidate B from docs/counters.md.
// Nap starts after NAP_IDLE_MS of continuous base_state == P_IDLE.
// Wakes on any non-idle base_state (running/waiting/completed).
static const uint32_t NAP_IDLE_MS   = 10UL * 60 * 1000;  // 10 minutes
static bool     _napping        = false;
static uint32_t _idle_since_ms  = 0;   // when continuous idle began; 0 = not tracking
static uint32_t _nap_start_ms   = 0;   // when current nap began

const char* persona_name(PersonaVariant v) {
    static const char* names[] = {"sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"};
    return (v < 7) ? names[v] : "?";
}

// --------------------------------------------------------------------
// Pure function
// --------------------------------------------------------------------

PersonaVariant select_persona(const NetworkState& net) {
    if (!net.connected)    return P_IDLE;
    if (net.waiting > 0)   return P_ATTENTION;
    if (net.completed)     return P_CELEBRATE;
    if (net.running >= 1)  return P_BUSY;
    return P_IDLE;
}

// --------------------------------------------------------------------
// Time-of-day idle personality
// --------------------------------------------------------------------

// Returns the active_state to use when base_state == P_IDLE and no anim
// timer is running. Uses wall-clock hour to bias toward P_SLEEP at night.
static PersonaVariant time_of_day_idle(uint32_t now) {
    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    int h = lt->tm_hour;

    if (h >= 1 && h < 7)  return P_SLEEP;
    if (h >= 7 && h < 9)  return (now / 6000) % 4 == 0 ? P_IDLE  : P_SLEEP;
    if (h == 12)           return (now / 5000) % 3 == 0 ? P_HEART : P_IDLE;
    if (h >= 22 || h == 0) return (now / 7000) % 3 == 0 ? P_DIZZY : P_SLEEP;
    return P_IDLE;
}

// --------------------------------------------------------------------
// Message sender
// --------------------------------------------------------------------

static MsgSender g_sender;
void app_set_sender(MsgSender sender) { g_sender = std::move(sender); }

// --------------------------------------------------------------------
// apply_message
// --------------------------------------------------------------------

static void copy_truncated(char* dst, size_t dst_size, const char* src) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

bool apply_message(AppState* app, const DaemonMsg& msg) {
    NetworkState& net = app->net;
    PersonaState& persona = app->persona;
    UiState& ui = app->ui;
    bool changed = false;

    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;

        if constexpr (std::is_same_v<T, Heartbeat>) {
            uint32_t now = SDL_GetTicks();
            net.last_live_ms = now;

            if (m.running != net.running || m.waiting != net.waiting ||
                m.total != net.total || m.completed != net.completed) changed = true;
            net.running   = m.running;
            net.waiting   = m.waiting;
            net.total     = m.total;
            net.completed = m.completed;

            if (m.tokens_today != net.tokens_today) { net.tokens_today = m.tokens_today; changed = true; }

            if (m.tokens != net.tokens_bridge) {
                uint32_t delta = (m.tokens > net.tokens_bridge) ? (m.tokens - net.tokens_bridge) : 0;
                net.tokens_bridge = m.tokens;
                if (delta > 0) stats_on_bridge_tokens(delta);
                changed = true;
            }

            char truncated[24];
            copy_truncated(truncated, sizeof(truncated), m.msg.c_str());
            if (strcmp(net.msg, truncated) != 0) {
                memcpy(net.msg, truncated, sizeof(net.msg));
                changed = true;
            }

            // Update lines; bump line_gen if content changed
            uint8_t new_n = (uint8_t)(m.entries.size() < 8 ? m.entries.size() : 8);
            bool lines_changed = (new_n != net.n_lines);
            for (uint8_t i = 0; i < new_n && !lines_changed; i++) {
                char tmp[24];
                copy_truncated(tmp, sizeof(tmp), m.entries[i].c_str());
                if (strcmp(net.lines[i], tmp) != 0) lines_changed = true;
            }
            if (lines_changed) {
                net.n_lines = new_n;
                for (uint8_t i = 0; i < new_n; i++)
                    copy_truncated(net.lines[i], sizeof(net.lines[i]), m.entries[i].c_str());
                net.line_gen++;
                changed = true;
            }

            // Prompt
            if (m.prompt) {
                if (strcmp(net.prompt_id, m.prompt->id.c_str()) != 0) {
                    copy_truncated(net.prompt_id,   sizeof(net.prompt_id),   m.prompt->id.c_str());
                    copy_truncated(net.prompt_tool, sizeof(net.prompt_tool), m.prompt->tool.c_str());
                    copy_truncated(net.prompt_hint, sizeof(net.prompt_hint), m.prompt->hint.c_str());
                    net.response_sent    = false;
                    net.prompt_arrived_ms = now;
                    ui.display_mode = DISP_NORMAL;
                    ui.help_open    = false;
                    LOG_INFO("[msg] prompt: %s (%s)", net.prompt_tool, net.prompt_id);
                    changed = true;
                }
            } else if (net.prompt_id[0] != '\0') {
                net.prompt_id[0] = net.prompt_tool[0] = net.prompt_hint[0] = '\0';
                LOG_INFO("[msg] prompt cleared");
                changed = true;
            }

            LOG_INFO("[msg] hb run=%u wait=%u total=%u comp=%s",
                     net.running, net.waiting, net.total, net.completed ? "y" : "n");
        }

        else if constexpr (std::is_same_v<T, TimeSync>) {
            net.last_live_ms = SDL_GetTicks();
        }

        else if constexpr (std::is_same_v<T, OwnerCmd>) {
            owner_set(m.name.c_str());
        }

        else if constexpr (std::is_same_v<T, TransportDisconnect>) {
            net.last_live_ms = 0;
            net.prompt_id[0] = net.prompt_tool[0] = net.prompt_hint[0] = '\0';
            LOG_WARN("[msg] transport disconnected");
            changed = true;
        }

        // CharBegin/FileMeta/Chunk/FileEnd/CharEnd/TurnEvent/GenericCmd:
        // not yet implemented on desktop; silently accepted.

    }, msg);

    return changed;
}

// --------------------------------------------------------------------
// apply_tick — ANIM_TICK handler
// --------------------------------------------------------------------

bool apply_tick(AppState* app, uint32_t now) {
    NetworkState& net = app->net;
    PersonaState& persona = app->persona;
    bool changed = false;

    // 1. Recompute connected from staleness
    bool conn = net.last_live_ms != 0 && (int32_t)(now - net.last_live_ms) < 30000;
    if (conn != net.connected) {
        net.connected = conn;
        if (!conn) {
            net.running = net.waiting = net.total = 0;
            net.completed = false;
            memcpy(net.msg, "No Claude connected", 20);
            net.prompt_id[0] = net.prompt_tool[0] = net.prompt_hint[0] = '\0';
            LOG_WARN("[tick] connection timed out");
        }
        changed = true;
    }

    // 2. Recompute base_state
    PersonaVariant base = select_persona(net);
    if (base != persona.base_state) {
        persona.base_state = base;
        // Latch celebration so it outlives the completed flag
        if (base == P_CELEBRATE && persona.anim_until == 0) {
            persona.active_state = P_CELEBRATE;
            persona.anim_until   = now + 3000;
        }
        changed = true;
    }

    // 3. Nap idle timer (Candidate B — inactivity idle timer)
    if (!_napping) {
        if (persona.base_state == P_IDLE) {
            if (_idle_since_ms == 0) _idle_since_ms = now;
            else if ((int32_t)(now - _idle_since_ms) >= (int32_t)NAP_IDLE_MS) {
                _napping      = true;
                _nap_start_ms = now;
                _idle_since_ms = 0;
                LOG_INFO("[nap] nap started");
                changed = true;
            }
        } else {
            _idle_since_ms = 0;
        }
    } else {
        // Napping — wake on any session activity
        if (persona.base_state != P_IDLE) {
            uint32_t nap_secs = (now - _nap_start_ms) / 1000;
            _napping = false;
            stats_on_nap_end(nap_secs);
            stats_on_wake(now);
            LOG_INFO("[nap] nap ended after %us", nap_secs);
            changed = true;
        }
    }
    if (persona.anim_until != 0 && (int32_t)(now - persona.anim_until) >= 0) {
        persona.anim_until = 0;
        changed = true;
    }

    // 4. Level-up celebrate
    if (stats_poll_level_up()) {
        persona.active_state = P_CELEBRATE;
        persona.anim_until   = now + 3000;
        changed = true;
    }

    // 5. Update active_state
    PersonaVariant desired;
    if (persona.anim_until != 0) {
        // Anim timer running: keep its state unless ATTENTION overrides
        desired = (persona.base_state == P_ATTENTION) ? P_ATTENTION : persona.active_state;
    } else if (persona.base_state == P_IDLE) {
        desired = time_of_day_idle(now);
    } else {
        desired = persona.base_state;
    }

    if (desired != persona.active_state) {
        persona.active_state = desired;
        changed = true;
    }

    return changed;
}

// --------------------------------------------------------------------
// apply_key
// --------------------------------------------------------------------

bool apply_key(AppState* app, int key) {
    NetworkState& net = app->net;
    PersonaState& persona = app->persona;
    UiState& ui = app->ui;
    uint32_t now = SDL_GetTicks();

    // Prompt approval/denial
    if (net.prompt_id[0] != '\0' && !net.response_sent && !ui.help_open) {
        if (key == SDLK_y) {
            if (g_sender) g_sender(PermissionCmd{"permission", "once", net.prompt_id});
            net.response_sent = true;
            uint32_t ms = now - net.prompt_arrived_ms;
            stats_on_approval(ms / 1000);
            if (ms < 5000) {
                persona.active_state = P_HEART;
                persona.anim_until   = now + 2000;
            }
            return true;
        }
        if (key == SDLK_n) {
            if (g_sender) g_sender(PermissionCmd{"permission", "deny", net.prompt_id});
            net.response_sent = true;
            stats_on_denial();
            return true;
        }
    }

    switch (key) {
        case SDLK_h:
            ui.help_open = !ui.help_open;
            return true;
        case SDLK_ESCAPE:
            if (ui.help_open) { ui.help_open = false; return true; }
            return false;
        case SDLK_w:
            ui.species_cycle = 1;
            return true;
        case SDLK_s:
            ui.species_cycle = -1;
            return true;
        case SDLK_a:
            ui.display_mode = (ui.display_mode == DISP_INFO) ? DISP_NORMAL : DISP_INFO;
            return true;
        case SDLK_d:
            ui.display_mode = (ui.display_mode == DISP_PET) ? DISP_NORMAL : DISP_PET;
            return true;
    }
    return false;
}
