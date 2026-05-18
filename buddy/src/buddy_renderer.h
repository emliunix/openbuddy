#pragma once
#include "species.h"
#include <cstdint>

void buddy_set_renderer(class Renderer* r);
void buddy_set_scale(uint8_t s);

void buddy_init();
void buddy_tick(uint8_t persona_state);
void buddy_invalidate();
void buddy_set_peek(bool peek);
void buddy_set_species_idx(uint8_t idx);
void buddy_next_species();
void buddy_prev_species();
uint8_t buddy_species_idx();
uint8_t buddy_species_count();
const char* buddy_species_name();
