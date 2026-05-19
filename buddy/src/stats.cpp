#include "stats.h"
#include <string>

static const uint32_t TOKENS_PER_LEVEL = 50000;

static Stats _stats = {};
static Settings _settings = {};
static char _pet_name[24] = "Buddy";
static char _owner_name[32] = "";
static bool _level_up_pending = false;

void stats_init() {
    _stats = {};
}

void stats_on_approval(uint32_t seconds_to_respond) {
    _stats.approvals++;
    _stats.velocity[_stats.vel_idx] = (uint16_t)(seconds_to_respond > 65535 ? 65535 : seconds_to_respond);
    _stats.vel_idx = (_stats.vel_idx + 1) % 8;
    if (_stats.vel_count < 8) _stats.vel_count++;
}

void stats_on_denial() {
    _stats.denials++;
}

uint16_t stats_median_velocity() {
    if (_stats.vel_count == 0) return 0;
    uint16_t tmp[8];
    memcpy(tmp, _stats.velocity, sizeof(tmp));
    uint8_t n = _stats.vel_count;
    for (uint8_t i = 1; i < n; i++) {
        uint16_t k = tmp[i]; int8_t j = i - 1;
        while (j >= 0 && tmp[j] > k) { tmp[j+1] = tmp[j]; j--; }
        tmp[j+1] = k;
    }
    return tmp[n/2];
}

uint8_t stats_mood_tier() {
    uint16_t vel = stats_median_velocity();
    int8_t tier;
    if (vel == 0) tier = 2;
    else if (vel < 15) tier = 4;
    else if (vel < 30) tier = 3;
    else if (vel < 60) tier = 2;
    else if (vel < 120) tier = 1;
    else tier = 0;
    uint16_t a = _stats.approvals, d = _stats.denials;
    if (a + d >= 3) {
        if (d > a) tier -= 2;
        else if (d * 2 > a) tier -= 1;
    }
    if (tier < 0) tier = 0;
    return (uint8_t)tier;
}

static uint64_t _wake_ms   = 0;
static uint8_t  _energy_at_wake = 3;

uint8_t stats_energy_tier(uint64_t now_ms) {
    if (_wake_ms == 0) return _energy_at_wake;
    uint64_t awake_ms = now_ms - _wake_ms;
    uint64_t drained  = awake_ms / (2ULL * 60 * 60 * 1000);
    int8_t tier = (int8_t)_energy_at_wake - (int8_t)drained;
    return (uint8_t)(tier < 0 ? 0 : tier);
}

uint8_t stats_fed_progress() {
    return (uint8_t)((_stats.tokens % TOKENS_PER_LEVEL) / (TOKENS_PER_LEVEL / 10));
}

bool stats_poll_level_up() {
    bool r = _level_up_pending;
    _level_up_pending = false;
    return r;
}

void stats_on_bridge_tokens(uint32_t tokens) {
    uint32_t prev_level = _stats.tokens / TOKENS_PER_LEVEL;
    _stats.tokens += tokens;
    if (_stats.tokens / TOKENS_PER_LEVEL > prev_level)
        _level_up_pending = true;
}

void stats_on_nap_end(uint32_t seconds) {
    _stats.nap_seconds += seconds;
}

void stats_on_wake(uint64_t now_ms) {
    _wake_ms        = now_ms;
    _energy_at_wake = 5;
}

Settings& settings() { return _settings; }
Stats& stats() { return _stats; }

const char* pet_name() { return _pet_name; }
const char* owner_name() { return _owner_name; }

void owner_set(const char* name) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j < sizeof(_owner_name) - 1; i++) {
        char c = name[i];
        if (c != '"' && c != '\\' && c >= 0x20) _owner_name[j++] = c;
    }
    _owner_name[j] = 0;
}

void pet_name_set(const char* name) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j < sizeof(_pet_name) - 1; i++) {
        char c = name[i];
        if (c != '"' && c != '\\' && c >= 0x20) _pet_name[j++] = c;
    }
    _pet_name[j] = 0;
}
