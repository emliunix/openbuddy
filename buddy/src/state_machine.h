#pragma once
#include "protocol_types.h"
#include <cstdint>
#include <optional>

// Persona state machine.
// No scattered if-statements; transitions are table-driven.

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };

const char* state_name(PersonaState s);

// ------------------------------------------------------------------
// State machine interface
// ------------------------------------------------------------------

class StateMachine {
public:
    void init();

    // Process a daemon message; returns true if visual state changed.
    bool on_message(const DaemonMsg& msg);

    // Periodic tick; returns true if visual state changed.
    bool on_tick(uint64_t now_ms);

    // Input; returns true if visual state changed.
    bool on_key(int key, bool pressed);

    PersonaState current() const { return active_state; }
    const std::optional<Prompt>& current_prompt() const { return current_prompt_; }

    void set_sender(std::function<void(const BuddyMsg&)> sender);

    // Render
    void render(class Renderer* r);

private:
    PersonaState base_state = P_IDLE;
    PersonaState active_state = P_IDLE;
    uint64_t one_shot_until = 0;

    bool help_open = false;

    enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO };
    DisplayMode display_mode = DISP_NORMAL;


    bool response_sent = false;
    uint64_t prompt_arrived_ms = 0;

    uint64_t last_live_ms = 0;
    bool connected = false;

    // Protocol state
    std::optional<Prompt> current_prompt_;
    std::vector<std::string> transcript_entries;
    std::string status_msg;
    uint32_t last_tokens = 0;

    // Fallback values for partial heartbeats
    uint32_t fallback_running = 0;
    uint32_t fallback_waiting = 0;
    uint32_t fallback_total = 0;

    std::function<void(const BuddyMsg&)> sender_;

    // Transitions --------------------------------------------------
    static PersonaState select_persona(uint32_t running, uint32_t waiting, bool completed);
    void transition_to(PersonaState s);
    void trigger_one_shot(PersonaState s, uint32_t dur_ms);

    // Render helpers
    void draw_help(Renderer* r);
    void draw_info(Renderer* r);
    void draw_pet(Renderer* r);
    void draw_prompt(Renderer* r);
    void draw_status(Renderer* r);
};
