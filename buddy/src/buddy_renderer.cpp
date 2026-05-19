#include "species.h"
#include "renderer.h"
#include <string>

static Renderer* _tgt = nullptr;
static uint8_t _scale = 1;

void buddy_set_renderer(Renderer* r) { _tgt = r; }
void buddy_set_scale(uint8_t s) { _scale = s; }

void buddy_print_line(const char* line, int y_px, uint16_t color, int x_off) {
    if (!_tgt) return;
    int len = (int)strlen(line);
    if (_scale > 1) {
        while (len && line[len-1] == ' ') len--;
        while (len && *line == ' ') { line++; len--; }
    }
    int w = len * BUDDY_CHAR_W * _scale;
    int x = BUDDY_X_CENTER - w / 2 + x_off * _scale;
    _tgt->set_text_color(color, BUDDY_BG);
    _tgt->set_cursor(x, y_px);
    for (int i = 0; i < len; i++) {
        char buf[2] = {line[i], 0};
        _tgt->print(buf);
    }
}

void buddy_print_sprite(const char* const* lines, uint8_t n_lines, int y_offset, uint16_t color, int x_off) {
    if (!_tgt) return;
    _tgt->set_text_size(_scale);
    int y_base = BUDDY_Y_BASE * _scale - (_scale - 1) * 14;
    for (uint8_t i = 0; i < n_lines; i++) {
        buddy_print_line(lines[i], y_base + (y_offset + i * BUDDY_CHAR_H) * _scale, color, x_off);
    }
}

void buddy_set_cursor(int x, int y) {
    if (!_tgt) return;
    _tgt->set_cursor(BUDDY_X_CENTER + (x - BUDDY_X_CENTER) * _scale, y * _scale);
}

void buddy_set_color(uint16_t fg) {
    if (!_tgt) return;
    _tgt->set_text_color(fg, BUDDY_BG);
}

void buddy_print(const char* s) {
    if (!_tgt) return;
    _tgt->set_text_size(_scale);
    _tgt->print(s);
}

#include "buddy_renderer.h"
#include <cstring>

extern const Species CAPYBARA_SPECIES;
extern const Species DUCK_SPECIES;
extern const Species GOOSE_SPECIES;
extern const Species BLOB_SPECIES;
extern const Species CAT_SPECIES;
extern const Species DRAGON_SPECIES;
extern const Species OCTOPUS_SPECIES;
extern const Species OWL_SPECIES;
extern const Species PENGUIN_SPECIES;
extern const Species TURTLE_SPECIES;
extern const Species SNAIL_SPECIES;
extern const Species GHOST_SPECIES;
extern const Species AXOLOTL_SPECIES;
extern const Species CACTUS_SPECIES;
extern const Species ROBOT_SPECIES;
extern const Species RABBIT_SPECIES;
extern const Species MUSHROOM_SPECIES;
extern const Species CHONK_SPECIES;

static const Species* SPECIES_TABLE[] = {
    &CAPYBARA_SPECIES, &DUCK_SPECIES, &GOOSE_SPECIES, &BLOB_SPECIES,
    &CAT_SPECIES, &DRAGON_SPECIES, &OCTOPUS_SPECIES, &OWL_SPECIES,
    &PENGUIN_SPECIES, &TURTLE_SPECIES, &SNAIL_SPECIES, &GHOST_SPECIES,
    &AXOLOTL_SPECIES, &CACTUS_SPECIES, &ROBOT_SPECIES, &RABBIT_SPECIES,
    &MUSHROOM_SPECIES, &CHONK_SPECIES,
};
static const uint8_t N_SPECIES = sizeof(SPECIES_TABLE) / sizeof(SPECIES_TABLE[0]);
static uint8_t current_species_idx = 0;

static uint64_t tick_count = 0;
static uint64_t next_tick_at = 0;
static const uint64_t TICK_MS = 200;

static uint8_t last_drawn_state = 0xFF;
static uint8_t last_drawn_species = 0xFF;

void buddy_init() {
    tick_count = 0;
    next_tick_at = 0;
    current_species_idx = 0;
}

void buddy_set_species_idx(uint8_t idx) {
    if (idx < N_SPECIES) current_species_idx = idx;
}

void buddy_next_species() {
    current_species_idx = (current_species_idx + 1) % N_SPECIES;
}

void buddy_prev_species() {
    current_species_idx = (current_species_idx + N_SPECIES - 1) % N_SPECIES;
}

const char* buddy_species_name() {
    return SPECIES_TABLE[current_species_idx]->name;
}

uint8_t buddy_species_count() { return N_SPECIES; }
uint8_t buddy_species_idx() { return current_species_idx; }

void buddy_invalidate() { last_drawn_state = 0xFF; }

void buddy_set_peek(bool peek) {
    buddy_set_scale(peek ? 1 : 2);
    buddy_invalidate();
}

void buddy_tick(uint8_t persona_state) {
    uint64_t now = SDL_GetTicks();
    if ((int64_t)(now - next_tick_at) >= 0) {
        next_tick_at = now + TICK_MS;
        tick_count++;
    }

    if (persona_state >= 7) persona_state = 1; // B_IDLE
    last_drawn_state = persona_state;
    last_drawn_species = current_species_idx;

    const Species* sp = SPECIES_TABLE[current_species_idx];
    if (sp->states[persona_state]) sp->states[persona_state](tick_count);
}
