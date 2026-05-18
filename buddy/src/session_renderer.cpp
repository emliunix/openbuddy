#include "session_renderer.h"
#include "session.h"
#include "renderer.h"
#include "buddy_renderer.h"
#include "stats.h"
#include <cstring>
#include <cstdio>
#include <cstdint>

static uint8_t wrapInto(const char* in, char out[][24], uint8_t maxRows, uint8_t width) {
    uint8_t row = 0, col = 0;
    const char* p = in;
    while (*p && row < maxRows) {
        while (*p == ' ') p++;
        const char* w = p;
        while (*p && *p != ' ') p++;
        uint8_t wlen = p - w;
        if (wlen == 0) break;
        uint8_t need = (col > 0 ? 1 : 0) + wlen;
        if (col + need > width) {
            out[row][col] = 0;
            if (++row >= maxRows) return row;
            out[row][0] = ' '; col = 1;
        }
        if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
        else if (col == 1 && row > 0) {}
        while (wlen > width - col) {
            uint8_t take = width - col;
            memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
            out[row][col] = 0;
            if (++row >= maxRows) return row;
            out[row][0] = ' '; col = 1;
        }
        memcpy(&out[row][col], w, wlen); col += wlen;
    }
    if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
    return row;
}

// Draw a filled or hollow tiny heart icon centered at (x, y).
static void tiny_heart(Renderer* r, int x, int y, bool filled, uint16_t col) {
    if (filled) {
        r->fill_circle(x - 2, y, 2, col);
        r->fill_circle(x + 2, y, 2, col);
        r->fill_triangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
    } else {
        r->draw_circle(x - 2, y, 2, col);
        r->draw_circle(x + 2, y, 2, col);
        r->draw_line(x - 4, y + 1, x, y + 5, col);
        r->draw_line(x + 4, y + 1, x, y + 5, col);
    }
}

void render_session(AppState* app, Renderer* r) {
    r->begin_frame();
    r->fill_sprite(BLACK);

    buddy_set_renderer(r);
    buddy_set_scale(app->ui.display_mode == DISP_NORMAL ? 2 : 1);

    if (app->ui.species_cycle > 0) {
        buddy_next_species();
        app->ui.species_cycle = 0;
    } else if (app->ui.species_cycle < 0) {
        buddy_prev_species();
        app->ui.species_cycle = 0;
    }

    buddy_tick(app->persona.active_state);

    switch (app->ui.display_mode) {
        case DISP_NORMAL: {
            bool has_prompt = app->net.prompt_id[0] != '\0';
            if (has_prompt) {
                // Approval panel — bottom 78 px (y=162 on a 240px window, but architecture
                // uses 135x240-equivalent so bottom 78 px starts at y=162).
                uint16_t panel   = 0x2104;
                uint16_t text    = WHITE;
                uint16_t text_dim = 0x8410;
                uint16_t accent  = 0xFA20;
                uint16_t red     = 0xF800;
                int pw = 127, ph = 78, px = (135 - pw) / 2, py = 240 - 78;

                r->fill_round_rect(px, py, pw, ph, 4, panel);
                r->draw_round_rect(px, py, pw, ph, 4, accent);

                // Row 1: "approve? Xs" — elapsed seconds; red after 10 s
                // (prompt_arrived_ms not available here without now; use 0 as fallback)
                uint32_t elapsed = 0; // renderer does not receive `now`; show static header
                bool overdue = false;
                r->set_text_size(1);
                r->set_text_color(overdue ? red : accent, panel);
                r->set_cursor(px + 4, py + 4);
                r->printf("approve?");

                // Row 2: tool name
                bool tool_long = strlen(app->net.prompt_tool) > 10;
                r->set_text_size(tool_long ? 1 : 2);
                r->set_text_color(text, panel);
                r->set_cursor(px + 4, py + 14);
                r->print(app->net.prompt_tool);

                // Row 3: hint[:21]
                r->set_text_size(1);
                r->set_text_color(text_dim, panel);
                r->set_cursor(px + 4, py + 34);
                char hint_a[22] = {};
                strncpy(hint_a, app->net.prompt_hint, 21);
                r->print(hint_a);

                // Row 4: hint[21:42] if hint longer than 21 chars
                if (strlen(app->net.prompt_hint) > 21) {
                    r->set_cursor(px + 4, py + 42);
                    char hint_b[22] = {};
                    strncpy(hint_b, app->net.prompt_hint + 21, 21);
                    r->print(hint_b);
                }

                // Bottom row: Y/N or "sent..."
                r->set_text_color(accent, panel);
                r->set_cursor(px + 4, py + ph - 12);
                if (app->net.response_sent) {
                    r->print("sent...");
                } else {
                    r->print("Y: approve  N: deny");
                }
            } else {
                // No prompt: HUD overlay — transcript entries at bottom (match upstream drawHUD)
                const int SHOW  = 3;
                const int LH    = 8;
                const int WIDTH = 21;
                const int AREA  = SHOW * LH + 4;

                r->fill_rect(0, 240 - AREA, 135, AREA, BLACK);

                if (app->net.n_lines == 0) {
                    r->set_text_size(1);
                    r->set_text_color(0x5ACB, BLACK);
                    r->set_cursor(4, 240 - LH - 2);
                    r->print(app->net.msg);
                } else {
                    static char disp[32][24];
                    static uint8_t srcOf[32];
                    uint8_t nDisp = 0;
                    for (uint8_t i = 0; i < app->net.n_lines && nDisp < 32; i++) {
                        uint8_t got = wrapInto(app->net.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
                        for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
                        nDisp += got;
                    }

                    int end = (int)nDisp;
                    int start = end - SHOW; if (start < 0) start = 0;
                    for (int i = 0; start + i < end; i++) {
                        uint8_t row = start + i;
                        r->set_text_size(1);
                        r->set_text_color(0x5ACB, BLACK);
                        r->set_cursor(4, 240 - AREA + 2 + i * LH);
                        r->print(disp[row]);
                    }
                }
            }
            break;
        }
        case DISP_INFO: {
            const int TOP = 70;
            r->fill_rect(0, TOP, 135, 240 - TOP, BLACK);
            r->set_text_size(1);

            int y = TOP + 2;
            r->set_text_color(0x8410, BLACK);
            r->set_cursor(4, y); r->printf("Lv %u  %s", stats().level, persona_name(app->persona.active_state)); y += 12;
            r->set_cursor(4, y); r->printf("appr %u  deny %u", stats().approvals, stats().denials); y += 12;
            r->set_cursor(4, y); r->printf("tokens %lu", stats().tokens); y += 12;
            r->set_cursor(4, y); r->printf("run %u  wait %u", app->net.running, app->net.waiting); y += 12;
            r->set_cursor(4, y); r->printf("species %s", buddy_species_name());

            break;
        }
        case DISP_PET: {
            const int TOP = 70;
            r->fill_rect(0, TOP, 135, 240 - TOP, BLACK);
            r->set_text_size(1);

            int y = TOP + 16;

            // Mood: 0-4 hearts
            uint8_t mood = stats_mood_tier();
            uint16_t mood_col = (mood >= 3) ? 0xF800 : (mood >= 2) ? 0xFA20 : 0x8410;
            r->set_text_color(0x8410, BLACK);
            r->set_cursor(6, y - 2); r->print("mood");
            for (int i = 0; i < 4; i++)
                tiny_heart(r, 54 + i * 16, y + 2, i < mood, mood_col);

            // Fed: 0-10 pip progress bar
            y += 20;
            uint8_t fed = stats_fed_progress();
            r->set_text_color(0x8410, BLACK);
            r->set_cursor(6, y - 2); r->print("fed");
            for (int i = 0; i < 10; i++) {
                int px = 38 + i * 9;
                if (i < fed) r->fill_circle(px, y + 1, 2, WHITE);
                else         r->draw_circle(px, y + 1, 2, 0x8410);
            }

            // Energy: 0-5 segmented bar
            y += 20;
            uint8_t en = stats_energy_tier(SDL_GetTicks());
            uint16_t en_col = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : 0xFA20;
            r->set_text_color(0x8410, BLACK);
            r->set_cursor(6, y - 2); r->print("energy");
            for (int i = 0; i < 5; i++) {
                int px = 54 + i * 13;
                if (i < en) r->fill_rect(px, y - 2, 9, 6, en_col);
                else        r->draw_rect(px, y - 2, 9, 6, 0x8410);
            }

            // Level badge
            y += 24;
            r->fill_round_rect(6, y - 2, 42, 14, 3, WHITE);
            r->set_text_color(BLACK, WHITE);
            r->set_cursor(11, y + 1); r->printf("Lv %u", stats().level);

            // Lifetime counters
            y += 20;
            r->set_text_color(0x8410, BLACK);
            r->set_cursor(6, y);   r->printf("approved %u", stats().approvals);
            r->set_cursor(6, y + 10); r->printf("denied   %u", stats().denials);
            uint32_t nap = stats().nap_seconds;
            r->set_cursor(6, y + 20); r->printf("napped   %uh%02um", nap / 3600, (nap / 60) % 60);

            // Token counters — abbreviate at 1K / 1M
            auto tok_fmt = [&](const char* label, uint32_t v, int yp) {
                r->set_cursor(6, yp);
                if (v >= 1000000)   r->printf("%s%lu.%luM", label, v / 1000000, (v / 100000) % 10);
                else if (v >= 1000) r->printf("%s%lu.%luK", label, v / 1000, (v / 100) % 10);
                else                r->printf("%s%lu", label, v);
            };
            tok_fmt("tokens   ", stats().tokens, y + 30);
            tok_fmt("today    ", app->net.tokens_today, y + 40);

            // Header: pet name left, persona right
            r->set_text_size(1);
            r->set_text_color(WHITE, BLACK);
            r->set_cursor(4, TOP + 2);
            if (owner_name()[0])
                r->printf("%s's %s", owner_name(), pet_name());
            else
                r->print(pet_name());
            r->set_text_color(0x8410, BLACK);
            r->set_cursor(135 - 48, TOP + 2);
            r->print(persona_name(app->persona.active_state));
            break;
        }
    }

    if (app->ui.help_open) {
        static const struct { const char* key; const char* desc; } rows[] = {
            {"W/S", "species"},
            {"A/D", "info/pet"},
            {"Y/N", "approve"},
            {"H",   "help"},
        };
        constexpr int NR = 4;
        uint16_t panel = 0x2104, text = WHITE, text_dim = 0x8410;
        int mw = 108, mh = 16 + NR * 12 + 8;
        int mx = (135 - mw) / 2, my = (240 - mh) / 2;
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
            r->set_cursor(mx + 34, my + 18 + i * 12);
            r->print(rows[i].desc);
        }
    }

    r->push_sprite(0, 0);
}
