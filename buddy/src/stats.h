#pragma once
#include <cstdint>

// Simplified stats system for desktop (no NVS persistence).
// Ported from claude-desktop-buddy/src/stats.h

struct Stats {
    uint32_t nap_seconds;
    uint16_t approvals;
    uint16_t denials;
    uint16_t velocity[8];
    uint8_t  vel_idx;
    uint8_t  vel_count;
    uint8_t  level;
    uint32_t tokens;
};

struct Settings {
    uint8_t species_idx;
};

void stats_init();
void stats_on_approval(uint32_t seconds_to_respond);
void stats_on_denial();
uint8_t stats_mood_tier();
uint8_t stats_energy_tier(uint32_t now_ms);
uint8_t stats_fed_progress();
bool stats_poll_level_up();
void stats_on_bridge_tokens(uint32_t tokens);
void stats_on_nap_end(uint32_t seconds);  // accumulate nap_seconds; call before stats_on_wake
void stats_on_wake(uint32_t now_ms);      // reset energy to full; record wake time

Settings& settings();
Stats& stats();

const char* pet_name();
const char* owner_name();
void owner_set(const char* name);
void pet_name_set(const char* name);
