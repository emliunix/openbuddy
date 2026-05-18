#pragma once
#include <cstdint>

// Shared geometry and helpers for species files.
// Ported from claude-desktop-buddy/src/buddy_common.h

inline constexpr int BUDDY_X_CENTER  = 67;
inline constexpr int BUDDY_CANVAS_W  = 135;
inline constexpr int BUDDY_Y_BASE    = 30;
inline constexpr int BUDDY_Y_OVERLAY = 6;
inline constexpr int BUDDY_CHAR_W    = 6;
inline constexpr int BUDDY_CHAR_H    = 8;

inline constexpr uint16_t BUDDY_BG     = 0x0000;
inline constexpr uint16_t BUDDY_HEART  = 0xF810;
inline constexpr uint16_t BUDDY_DIM    = 0x8410;
inline constexpr uint16_t BUDDY_YEL    = 0xFFE0;
inline constexpr uint16_t BUDDY_WHITE  = 0xFFFF;
inline constexpr uint16_t BUDDY_CYAN   = 0x07FF;
inline constexpr uint16_t BUDDY_GREEN  = 0x07E0;
inline constexpr uint16_t BUDDY_PURPLE = 0xA01F;
inline constexpr uint16_t BUDDY_RED    = 0xF800;
inline constexpr uint16_t BUDDY_BLUE   = 0x041F;

void buddy_print_line(const char* line, int y_px, uint16_t color, int x_off = 0);
void buddy_print_sprite(const char* const* lines, uint8_t n_lines, int y_offset, uint16_t color, int x_off = 0);
void buddy_set_cursor(int x, int y);
void buddy_set_color(uint16_t fg);
void buddy_print(const char* s);

// Per-species state function signature
typedef void (*StateFn)(uint32_t t);

struct Species {
    const char* name;
    uint16_t body_color;
    StateFn states[7];   // sleep, idle, busy, attention, celebrate, dizzy, heart
};
