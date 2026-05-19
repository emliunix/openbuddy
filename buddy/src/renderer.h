#pragma once
#include <SDL3/SDL.h>
#include <stdint.h>
#include <string>

// TFT_eSPI API compatibility layer for SDL3.
// Colors are RGB565 (16-bit) to match the reference.

uint32_t rgb565_to_sdl(uint16_t c);

class Renderer {
public:
    Renderer(SDL_Renderer* r, int w, int h);

    void fill_sprite(uint16_t c);
    void fill_rect(int x, int y, int w, int h, uint16_t c);
    void draw_rect(int x, int y, int w, int h, uint16_t c);
    void fill_round_rect(int x, int y, int w, int h, int r, uint16_t c);
    void draw_round_rect(int x, int y, int w, int h, int r, uint16_t c);
    void draw_fast_hline(int x, int y, int w, uint16_t c);
    void draw_line(int x1, int y1, int x2, int y2, uint16_t c);
    void fill_circle(int x, int y, int r, uint16_t c);
    void draw_circle(int x, int y, int r, uint16_t c);
    void fill_triangle(int x1, int y1, int x2, int y2, int x3, int y3, uint16_t c);

    void set_text_color(uint16_t fg, uint16_t bg);
    void set_cursor(int x, int y);
    void set_text_size(int s);
    void set_text_datum(int d);
    void print(const char* s);
    void draw_string(const char* s, int x, int y);
    void printf(const char* fmt, ...);

    void begin_frame();
    void push_sprite(int x, int y);

    int width() const { return w_; }
    int height() const { return h_; }

private:
    SDL_Renderer* ren_;
    int w_, h_;
    uint16_t fg_;
    uint16_t bg_;
    int cx_, cy_;
    int text_size_;
    int datum_;

    void set_draw_color(uint16_t c);
    void render_char(char c, int x, int y);
    void string_size(const char* s, int& w, int& h);
};

// Datum constants
constexpr int TL_DATUM = 0;
constexpr int MC_DATUM = 4;

// Common colors (RGB565)
constexpr uint16_t BLACK   = 0x0000;
constexpr uint16_t WHITE   = 0xFFFF;
constexpr uint16_t RED     = 0xF800;
constexpr uint16_t GREEN   = 0x07E0;
constexpr uint16_t BLUE    = 0x001F;
constexpr uint16_t YELLOW  = 0xFFE0;
constexpr uint16_t CYAN    = 0x07FF;
constexpr uint16_t MAGENTA = 0xF81F;
constexpr uint16_t ORANGE  = 0xFA20;
