#include "state_machine.h"
#include "renderer.h"
#include "buddy_renderer.h"
#include "stats.h"
#include <cstring>

static const int W = 135;
static const int H = 240;

// ------------------------------------------------------------------
// Persona selection — priority-ordered, extracted into pure function
// ------------------------------------------------------------------

PersonaState StateMachine::select_persona(uint32_t running, uint32_t waiting, bool completed) {
    if (waiting > 0) return P_ATTENTION;
    if (completed)     return P_CELEBRATE;
    if (running >= 1)  return P_BUSY;
    return P_IDLE;
}

const char* state_name(PersonaState s) {
    static const char* names[] = {"sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"};
    return (s < 7) ? names[s] : "?";
}

// ------------------------------------------------------------------
// State machine implementation
// ------------------------------------------------------------------

void StateMachine::init() {
    base_state = P_IDLE;
    active_state = P_IDLE;
    one_shot_until = 0;
    help_open = false;
    display_mode = DISP_NORMAL;
    response_sent = false;
    prompt_arrived_ms = 0;
    last_live_ms = 0;
    connected = false;
    current_prompt_ = std::nullopt;
    transcript_entries.clear();
    status_msg.clear();
    last_tokens = 0;
    fallback_running = 0;
    fallback_waiting = 0;
    fallback_total = 0;
    buddy_init();
    stats_init();
}

void StateMachine::transition_to(PersonaState s) {
    if (base_state == s) return;
    base_state = s;
    active_state = s;
    if (s == P_CELEBRATE) one_shot_until = SDL_GetTicks() + 2000;
    buddy_invalidate();
}

void StateMachine::trigger_one_shot(PersonaState s, uint32_t dur_ms) {
    active_state = s;
    one_shot_until = SDL_GetTicks() + dur_ms;
}

// ------------------------------------------------------------------
// Message handling — returns true if visual state changed
// ------------------------------------------------------------------

bool StateMachine::on_message(const DaemonMsg& msg) {
    bool changed = false;
    std::visit([&](const auto& m) {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, Heartbeat>) {
            last_live_ms = SDL_GetTicks();
            connected = true;
            fallback_running = m.running;
            fallback_waiting = m.waiting;
            fallback_total = m.total;
            status_msg = m.msg;
            if (status_msg.length() > 23) status_msg = status_msg.substr(0, 23);
            transcript_entries = m.entries;
            last_tokens = m.tokens;

            auto new_state = select_persona(m.running, m.waiting, m.completed);
            if (new_state != base_state) {
                transition_to(new_state);
                changed = true;
            }
            if (m.prompt) {
                if (!current_prompt_ || current_prompt_->id != m.prompt->id) {
                    current_prompt_ = m.prompt;
                    prompt_arrived_ms = SDL_GetTicks();
                    response_sent = false;
                    changed = true;
                }
            } else if (current_prompt_) {
                current_prompt_ = std::nullopt;
                changed = true;
            }
        }
        else if constexpr (std::is_same_v<T, OwnerCmd>) {
            owner_set(m.name.c_str());
            changed = true;
        }
        else if constexpr (std::is_same_v<T, TimeSync>) {
            last_live_ms = SDL_GetTicks();
            connected = true;
        }
        // TurnEvent, GenericCmd, CharBegin, etc. ignored (matches upstream)
    }, msg);
    return changed;
}

// ------------------------------------------------------------------
// Periodic tick — returns true if visual state changed
// ------------------------------------------------------------------

bool StateMachine::on_tick(uint64_t now) {
    bool changed = false;

    // One-shot timeout (celebrate etc.)
    if (one_shot_until > 0 && (int64_t)(now - one_shot_until) >= 0) {
        active_state = base_state;
        one_shot_until = 0;
        changed = true;
    }

    // Demo mode auto-cycle — removed
    // Connection timeout (30s)
    if (connected && (int64_t)(now - last_live_ms) > 30000) {
        connected = false;
        transition_to(P_IDLE);
        status_msg = "No Claude connected";
        current_prompt_ = std::nullopt;
        changed = true;
    }

    return changed;
}

// ------------------------------------------------------------------
// Input handling — returns true if visual state changed
// ------------------------------------------------------------------

bool StateMachine::on_key(int key, bool pressed) {
    if (!pressed) return false;

    // Prompt approval (only when prompt active and not yet responded)
    if (current_prompt_ && !help_open && !response_sent) {
        if (key == SDLK_Y) {
            if (sender_) sender_(PermissionCmd{"permission", "once", current_prompt_->id});
            response_sent = true;
            stats_on_approval((uint32_t)((SDL_GetTicks() - prompt_arrived_ms) / 1000));
            return true;
        }
        if (key == SDLK_N) {
            if (sender_) sender_(PermissionCmd{"permission", "deny", current_prompt_->id});
            response_sent = true;
            stats_on_denial();
            return true;
        }
    }

    switch (key) {
        case SDLK_H:
            help_open = !help_open;
            return true;
        case SDLK_ESCAPE:
            if (help_open) { help_open = false; return true; }
            return false;
        case SDLK_A:
            display_mode = (display_mode == DISP_INFO) ? DISP_NORMAL : DISP_INFO;
            return true;
        case SDLK_D:
            display_mode = (display_mode == DISP_PET) ? DISP_NORMAL : DISP_PET;
            return true;
        case SDLK_W:
            buddy_next_species();
            buddy_invalidate();
            return true;
        case SDLK_S:
            buddy_prev_species();
            buddy_invalidate();
            return true;
    }
    return false;
}

// ------------------------------------------------------------------
// Render
// ------------------------------------------------------------------

void StateMachine::render(Renderer* r) {
    r->begin_frame();
    r->fill_sprite(BLACK);

    buddy_set_renderer(r);
    buddy_set_scale(2);
    buddy_tick(active_state);  // draws pet at current animation frame

    switch (display_mode) {
        case DISP_NORMAL:
            draw_status(r);
            if (current_prompt_) draw_prompt(r);
            else {
                r->set_text_size(1);
                r->set_text_color(0x8410, BLACK);
                r->set_cursor(4, H - 10);
                r->print(state_name(active_state));
                r->set_cursor(W - 60, H - 10);
                r->printf("sp:%u", buddy_species_idx());
            }
            break;
        case DISP_INFO:  draw_info(r); break;
        case DISP_PET:   draw_pet(r); break;
    }

    if (help_open) draw_help(r);

    r->push_sprite(0, 0);
}

// ------------------------------------------------------------------
// Render helpers
// ------------------------------------------------------------------

void StateMachine::draw_help(Renderer* r) {
    static const struct { const char* key; const char* desc; } rows[] = {
        {"W/S", "species"},
        {"A/D", "info/pet"},
        {"Y/N", "approve"},
        {"H",   "help"},
    };
    constexpr int NR = 4;
    uint16_t panel = 0x2104, text = WHITE, text_dim = 0x8410;
    int mw = 180, mh = 16 + NR * 12 + 8;
    int mx = (W - mw) / 2, my = (H - mh) / 2;
    r->fill_round_rect(mx, my, mw, mh, 4, panel);
    r->draw_round_rect(mx, my, mw, mh, 4, text_dim);
    r->set_text_size(1);
    r->set_text_color(text, panel);
    r->set_cursor(mx + 6, my + 6);
    r->print("Keys");
    for (int i = 0; i < NR; i++) {
        r->set_text_color(text_dim, panel);
        r->set_cursor(mx + 6, my + 18 + i * 12);
        r->print(rows[i].key);
        r->set_text_color(text, panel);
        r->set_cursor(mx + 40, my + 18 + i * 12);
        r->print(rows[i].desc);
    }
}

void StateMachine::draw_info(Renderer* r) {
    uint16_t text = WHITE, text_dim = 0x8410, body = 0xC2A6;
    r->fill_rect(0, 70, W, H - 70, BLACK);
    r->set_text_size(1);
    int y = 72;
    r->set_text_color(text, BLACK); r->set_cursor(4, y); r->print("Info"); y += 12;
    r->set_text_color(body, BLACK); r->set_cursor(4, y); r->print("ABOUT"); y += 12;
    r->set_text_color(text_dim, BLACK);
    r->set_cursor(4, y); r->print("OpenBuddy for OpenCode"); y += 8;
    r->set_cursor(4, y); r->print("TCP desktop buddy"); y += 8;
    y += 6;
    r->set_cursor(4, y); r->print("18 ASCII species"); y += 8;
    r->set_cursor(4, y); r->print("135x240 logical res"); y += 8;
    y += 6;
    r->set_text_color(text, BLACK); r->set_cursor(4, y); r->print("Keys:"); y += 8;
    r->set_text_color(text_dim, BLACK);
    r->set_cursor(4, y); r->print("W/S = cycle species"); y += 8;
    r->set_cursor(4, y); r->print("A/D = info/pet view"); y += 8;
    r->set_cursor(4, y); r->print("M = menu"); y += 8;
    r->set_cursor(4, y); r->print("Y/N = approve/deny"); y += 8;
}

void StateMachine::draw_pet(Renderer* r) {
    uint16_t text = WHITE, text_dim = 0x8410, body = 0xC2A6;
    r->fill_rect(0, 70, W, H - 70, BLACK);
    r->set_text_size(1);
    int y = 90;

    r->set_text_color(text_dim, BLACK); r->set_cursor(6, y - 2); r->print("mood");
    uint8_t mood = stats_mood_tier();
    uint16_t mood_col = (mood >= 3) ? 0xF800 : (mood >= 2) ? 0xFA20 : text_dim;
    for (int i = 0; i < 4; i++) {
        int px = 54 + i * 16;
        (i < mood) ? r->fill_circle(px, y + 2, 2, mood_col) : r->draw_circle(px, y + 2, 2, text_dim);
    }

    y += 20;
    r->set_cursor(6, y - 2); r->print("fed");
    uint8_t fed = stats_fed_progress();
    for (int i = 0; i < 10; i++) {
        int px = 38 + i * 9;
        (i < fed) ? r->fill_circle(px, y + 1, 2, body) : r->draw_circle(px, y + 1, 2, text_dim);
    }

    y += 20;
    r->set_cursor(6, y - 2); r->print("energy");
    uint8_t en = stats_energy_tier(SDL_GetTicks());
    uint16_t en_col = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : 0xFA20;
    for (int i = 0; i < 5; i++) {
        int px = 54 + i * 13;
        (i < en) ? r->fill_rect(px, y - 2, 9, 6, en_col) : r->draw_rect(px, y - 2, 9, 6, text_dim);
    }

    y += 24;
    r->fill_round_rect(6, y - 2, 42, 14, 3, body);
    r->set_text_color(BLACK, body);
    r->set_cursor(11, y + 1); r->printf("Lv %u", stats().level);

    y += 20;
    r->set_text_color(text_dim, BLACK);
    r->set_cursor(6, y); r->printf("approved %u", stats().approvals); y += 10;
    r->set_cursor(6, y); r->printf("denied   %u", stats().denials); y += 10;
    r->set_cursor(6, y); r->printf("tokens   %lu", stats().tokens); y += 10;

    r->set_text_color(text, BLACK);
    r->set_cursor(4, 72);
    if (owner_name()[0]) r->printf("%s's %s", owner_name(), pet_name());
    else r->print(pet_name());
    r->set_text_color(text_dim, BLACK);
    r->set_cursor(W - 36, 72);
    r->printf("1/2");
}

void StateMachine::draw_prompt(Renderer* r) {
    if (!current_prompt_) return;
    uint16_t panel = 0x2104, text = WHITE, text_dim = 0x8410, accent = 0xFA20;
    int pw = 220, ph = 60, px = (W - pw) / 2, py = 80;
    r->fill_round_rect(px, py, pw, ph, 4, panel);
    r->draw_round_rect(px, py, pw, ph, 4, accent);
    r->set_text_size(1);
    r->set_text_color(accent, panel);
    r->set_cursor(px + 6, py + 6); r->print("approve: ");
    r->set_text_color(text, panel);
    r->print(current_prompt_->tool.c_str());
    r->set_text_color(text_dim, panel);
    r->set_cursor(px + 6, py + 22);
    std::string hint = current_prompt_->hint;
    if (hint.length() > 32) hint = hint.substr(0, 29) + "...";
    r->print(hint.c_str());
    r->set_text_color(accent, panel);
    r->set_cursor(px + 6, py + 44); r->print("Y = once  N = deny");
}

void StateMachine::draw_status(Renderer* r) {
    if (status_msg.empty()) return;
    r->set_text_size(1);
    r->set_text_color(0x8410, BLACK);
    r->set_cursor(4, H - 20);
    r->print(status_msg.c_str());
}
