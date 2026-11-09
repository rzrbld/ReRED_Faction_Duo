/*
 * rered_launcher - ReRed Faction II's PortMaster launcher (C + SDL2 only).
 *
 *   rered-launcher --root GAMEDIR
 *
 * First run (no data/.installed): import page. It checks source/ for the two
 * discs and the unprotected rf2.exe and runs rered-import (with PortMaster's
 * 7zzs from RERED_7Z for archives) showing its progress. Later runs: the
 * settings page (FPS counter, frame limit, look speed, CPU, controls mode,
 * button mapping), then Save and Run / Cancel.
 *
 * Writes conf/rered.cfg (shell, sourced by the Ports script) and
 * conf/rered.gptk (the gptokeyb mapping). Runs before gptokeyb starts, so it
 * reads the controller itself.
 * Exit status: 0 start the game, 10 the user left, other values = error.
 *
 * Look: an original DOS text-mode screen in the spirit of the game's setting
 * (a resistance cell's terminal under a propaganda state): red on black, a
 * block-drawn star, a broadcast ticker the cell keeps "jamming". 40x30 cells
 * of 16x16 on a 640x480 canvas scaled to fit any screen.
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "font.h"

#define W 640
#define H 480
enum { EXIT_RUN = 0, EXIT_USER = 10, EXIT_ERR = 1 };

/* palette: phosphor red on black, amber for "go" */
static const SDL_Color C_BG = { 0x0A, 0x04, 0x04, 255 }, C_PANEL = { 0x16, 0x07, 0x06, 255 },
    C_FRAME = { 0xB0, 0x1C, 0x16, 255 }, C_FRAME_DIM = { 0x52, 0x10, 0x0D, 255 },
    C_TEXT = { 0xF2, 0xDC, 0xCE, 255 }, C_DIM = { 0x86, 0x48, 0x3E, 255 },
    C_HI_BG = { 0xC4, 0x22, 0x1A, 255 }, C_HI_FG = { 0xFF, 0xF4, 0xEA, 255 },
    C_RED = { 0xFF, 0x4C, 0x3C, 255 }, C_OK = { 0xF0, 0xB4, 0x4C, 255 }, C_STAR = { 0xE0, 0x2A, 0x20, 255 };

static SDL_Renderer *R;
static const char *g_root;
static char g_conf[4096], g_data[4096], g_source[4096];

/* ------------------------------------------------------------ drawing
 * Pixel layout on the 640x480 logical canvas. Text is the 8x8 block font at
 * an integer scale: 2 (16 px cells) for body text, 3 for the title. */
static const rr_glyph *glyph(unsigned char c) {
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
    for (size_t i = 0; i < sizeof k_font / sizeof *k_font; i++) if ((unsigned char)k_font[i].c == c) return &k_font[i];
    return &k_font[0];
}
static void fill(int x, int y, int w, int h, SDL_Color c) { SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255); SDL_Rect r = { x, y, w, h }; SDL_RenderFillRect(R, &r); }
static void glyph_at(int x, int y, unsigned char ch, int sc, SDL_Color fg) {
    const rr_glyph *g = glyph(ch);
    SDL_SetRenderDrawColor(R, fg.r, fg.g, fg.b, 255);
    for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++)
        if (g->rows[j][i] == '#') { SDL_Rect p = { x + i * sc, y + j * sc, sc, sc }; SDL_RenderFillRect(R, &p); }
}
static int text_w(const char *s, int sc) { return (int)strlen(s) * 8 * sc; }
static void text_at(int x, int y, const char *s, int sc, SDL_Color fg);
/* at most n characters (log lines are longer than the screen) */
static void text_n(int x, int y, const char *s, int n, int sc, SDL_Color fg) { char b[128]; snprintf(b, sizeof b, "%.*s", n, s); text_at(x, y, b, sc, fg); }
static void text_at(int x, int y, const char *s, int sc, SDL_Color fg) { for (; *s; s++, x += 8 * sc) glyph_at(x, y, (unsigned char)*s, sc, fg); }
static void text_c(int y, const char *s, int sc, SDL_Color fg) { text_at((640 - text_w(s, sc)) / 2, y, s, sc, fg); }
static void text_r(int xr, int y, const char *s, int sc, SDL_Color fg) { text_at(xr - text_w(s, sc), y, s, sc, fg); }
/* a five-pointed star in 4-pixel blocks, 48x48 */
static void star(int px, int py, SDL_Color c) {
    static const char *rows[12] = {
        "     ##     ", "     ##     ", "    ####    ", "############", " ########## ", "  ########  ",
        "   ######   ", "  ########  ", "  ###  ###  ", " ###    ### ", " ##      ## ", "#          #" };
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    for (int y = 0; y < 12; y++) for (int x = 0; x < 12; x++)
        if (rows[y][x] == '#') { SDL_Rect p = { px + x * 4, py + y * 4, 4, 4 }; SDL_RenderFillRect(R, &p); }
}
static void scanlines(void) {
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 80);
    for (int y = 1; y < 480; y += 2) SDL_RenderDrawLine(R, 0, y, 639, y);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_NONE);
}
/* the state broadcast, overwritten by the cell: a smooth-scrolling strip */
static const char *k_ticker =
    "*** COMMONWEALTH BROADCAST: THE CHANCELLOR PROTECTS. OBEY AND PROSPER.   "
    "*** CURFEW IN EFFECT AFTER DARK. REPORT DISSENT TO YOUR BLOCK WARDEN.   "
    "*** SIGNAL JAMMED *** THIS IS THE RED FACTION. THE STATE CANNOT HOLD US. "
    "WE ARE EVERYWHERE. TOMORROW BELONGS TO THE PEOPLE.   ";
#define TICK_Y 450
static void ticker(void) {
    fill(10, TICK_Y - 6, 620, 28, C_PANEL);
    int len = text_w(k_ticker, 2), jam = (int)(strstr(k_ticker, "*** SIGNAL") - k_ticker) * 16;
    int off = (int)((SDL_GetTicks() / 16) % (Uint32)len);       /* ~60 px per second */
    SDL_Rect clip = { 14, TICK_Y - 4, 612, 24 };
    SDL_RenderSetClipRect(R, &clip);
    for (int x = 14 - off; x < 626; x += len)
        for (const char *c = k_ticker; *c; c++) {
            int gx = x + (int)(c - k_ticker) * 16;
            if (gx < -16 || gx > 640) continue;
            int in_jam = (int)(c - k_ticker) * 16 >= jam;
            glyph_at(gx, TICK_Y, (unsigned char)*c, 2, in_jam ? C_RED : C_DIM);
        }
    SDL_RenderSetClipRect(R, NULL);
}
/* the frame, header and page title shared by every page */
static void frame_begin(const char *title) {
    fill(0, 0, 640, 480, C_BG);
    fill(4, 4, 632, 2, C_FRAME); fill(4, 474, 632, 2, C_FRAME); fill(4, 4, 2, 472, C_FRAME); fill(634, 4, 2, 472, C_FRAME);
    fill(8, 8, 624, 1, C_FRAME_DIM); fill(8, 471, 624, 1, C_FRAME_DIM); fill(8, 8, 1, 464, C_FRAME_DIM); fill(631, 8, 1, 464, C_FRAME_DIM);
    star(22, 16, C_STAR); star(570, 16, C_STAR);
    text_c(18, "RERED FACTION II", 3, C_RED);
    text_c(50, "CELL TERMINAL // SECURE LINE", 1, C_DIM);
    fill(12, 72, 616, 2, C_FRAME);
    if (title) {
        int w = text_w(title, 2) + 24, x = (640 - w) / 2;
        fill(x, 84, w, 24, C_FRAME);
        text_at(x + 12, 88, title, 2, C_HI_FG);
    }
    fill(12, 436, 616, 2, C_FRAME);
    ticker();
}
/* a button "chip": inverted key, then its meaning; returns the next x */
static int chip(int x, int y, const char *key, const char *what) {
    int kw = text_w(key, 2) + 8;
    fill(x, y - 2, kw, 20, C_DIM);
    text_at(x + 4, y, key, 2, C_BG);
    text_at(x + kw + 6, y, what, 2, C_DIM);
    return x + kw + 6 + text_w(what, 2) + 22;
}
/* chips row centred above the ticker: pairs of key, meaning, NULL-terminated */
static void hints(const char *const *kv) {
    int w = 0;
    for (int i = 0; kv[i]; i += 2) w += text_w(kv[i], 2) + 8 + 6 + text_w(kv[i + 1], 2) + 22;
    int x = (640 - (w - 22)) / 2;
    for (int i = 0; kv[i]; i += 2) x = chip(x, 410, kv[i], kv[i + 1]);
}
/* RERED_SHOT=file.bmp (tests): save the 20th presented frame and exit */
static void present(void) {
    scanlines();
    static int n; const char *shot = getenv("RERED_SHOT");
    if (shot && ++n == 20) {
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(R, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
        SDL_SaveBMP(sf, shot); exit(EXIT_USER);
    }
    SDL_RenderPresent(R);
}

/* ------------------------------------------------------------- input */
enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_QUIT };
static SDL_GameController *g_pad;
static int g_capture = 0;             /* bindings page: next pad button is reported raw */
static int g_captured = -1;           /* SDL button / 100+axis for triggers */
static int poll_key(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: return K_QUIT;
        case SDL_CONTROLLERDEVICEADDED: if (!g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which); break;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN; case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT;
            case SDLK_RETURN: case SDLK_SPACE: return K_A; case SDLK_ESCAPE: case SDLK_BACKSPACE: return K_B;
            }
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (g_capture && (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) && e.caxis.value > 20000) {
                g_captured = 100 + e.caxis.axis; return K_NONE;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            if (g_capture) { g_captured = e.cbutton.button; return K_NONE; }
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UP; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWN;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFT; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHT;
            case SDL_CONTROLLER_BUTTON_A: return K_A; case SDL_CONTROLLER_BUTTON_B: return K_B;
            }
            break;
        }
    }
    return K_NONE;
}

/* ---------------------------------------------------------- settings */
/* What each pad button sends (gptokeyb key names). Defaults follow the
 * community Xpadder layout for the game's default keys (packaging/rered.gptk).
 * "pause" is turned into a mouse-wheel step by the port (next weapon). */
typedef struct { const char *label, *key; char button[16]; const char *def; } action;
static action g_act[] = {
    { "FIRE",              "mouse_left",  "", "r2" },
    { "ALT FIRE / ZOOM",   "mouse_right", "", "l2" },
    { "JUMP (SPACE)",      "space",       "", "a" },
    { "USE (E)",           "e",           "", "b" },
    { "RELOAD (R)",        "r",           "", "x" },
    { "NEXT WEAPON",       "pause",       "", "y" },
    { "KEY '",             "'",           "", "l1" },
    { "GRENADE (G)",       "g",           "", "r1" },
    { "LEFT SHIFT",        "leftshift",   "", "l3" },
    { "CROUCH (C)",        "c",           "", "r3" },
    { "KEY N",             "n",           "", "up" },
    { "MENU (ESC)",        "esc",         "", "start" },
    { "OBJECTIVES (TAB)",  "tab",         "", "back" },
};
#define NACT (int)(sizeof g_act / sizeof *g_act)
static int g_fps = 1, g_limit = 0, g_look = 100, g_cpu_perf = 1, g_joy = 0;

static void cfg_load(void) {
    for (int i = 0; i < NACT; i++) snprintf(g_act[i].button, sizeof g_act[i].button, "%s", g_act[i].def);
    char p[4200]; snprintf(p, sizeof p, "%s/rered.cfg", g_conf);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int v; char k[64], b[16];
        if (sscanf(line, "RERED_FPS=%d", &v) == 1) g_fps = v;
        else if (sscanf(line, "RERED_LIMIT=%d", &v) == 1) g_limit = v;
        else if (sscanf(line, "RERED_LOOK=%d", &v) == 1) g_look = v;
        else if (sscanf(line, "RERED_JOY=%d", &v) == 1) g_joy = v;
        else if (!strncmp(line, "RERED_CPU=", 12)) g_cpu_perf = strncmp(line + 12, "system", 6) != 0;
        else if (sscanf(line, "# bind %63s %15s", k, b) == 2)
            for (int i = 0; i < NACT; i++) if (!strcmp(g_act[i].key, k)) snprintf(g_act[i].button, sizeof g_act[i].button, "%s", b);
    }
    fclose(f);
}
static int cfg_save(void) {
    char p[4200], t[4300];
    snprintf(p, sizeof p, "%s/rered.cfg", g_conf); snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w"); if (!f) return -1;
    fprintf(f, "# ReRed Faction II settings (written by the launcher)\nRERED_FPS=%d\nRERED_LIMIT=%d\nRERED_LOOK=%d\nRERED_CPU=%s\nRERED_JOY=%d\n",
            g_fps, g_limit, g_look, g_cpu_perf ? "perf" : "system", g_joy);
    for (int i = 0; i < NACT; i++) fprintf(f, "# bind %s %s\n", g_act[i].key, g_act[i].button);
    if (fclose(f) || rename(t, p)) return -1;
    /* gptokeyb mapping: sticks fixed (WASD, mouse), buttons as bound */
    snprintf(p, sizeof p, "%s/rered.gptk", g_conf); snprintf(t, sizeof t, "%s.new", p);
    f = fopen(t, "w"); if (!f) return -1;
    fprintf(f, "# generated by the launcher from conf/rered.cfg\n"
               "left_analog_up = w\nleft_analog_down = s\nleft_analog_left = a\nleft_analog_right = d\n"
               "right_analog_up = mouse_movement_up\nright_analog_down = mouse_movement_down\n"
               "right_analog_left = mouse_movement_left\nright_analog_right = mouse_movement_right\n"
               "mouse_delay = 16\n");
    /* gptokeyb's mouse_scale divides stick travel: 4096 is the speed tuned as 100 % */
    fprintf(f, "mouse_scale = %d\n", 4096 * 100 / (g_look > 0 ? g_look : 100));
    for (int i = 0; i < NACT; i++) if (g_act[i].button[0]) fprintf(f, "%s = %s\n", g_act[i].button, g_act[i].key);
    if (fclose(f) || rename(t, p)) return -1;
    return 0;
}
static const char *button_name(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "a"; case SDL_CONTROLLER_BUTTON_B: return "b"; case SDL_CONTROLLER_BUTTON_X: return "x";
    case SDL_CONTROLLER_BUTTON_Y: return "y"; case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "l1"; case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "r1";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "l3"; case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "r3";
    case SDL_CONTROLLER_BUTTON_START: return "start"; case SDL_CONTROLLER_BUTTON_BACK: return "back";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "up"; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "left"; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "right";
    case 100 + SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "l2"; case 100 + SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "r2";
    }
    return NULL;
}
static void upper(char *o, const char *s, size_t n) { size_t i = 0; for (; s[i] && i + 1 < n; i++) o[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]); o[i] = 0; }

static int page_bindings(void) {
    int sel = 0;
    for (;;) {
        frame_begin("WEAPONS DRILL");
        text_at(40, 118, "LEFT STICK", 2, C_DIM);  text_r(600, 118, "MOVE (WASD)", 2, C_DIM);
        text_at(40, 136, "RIGHT STICK", 2, C_DIM); text_r(600, 136, "LOOK / CURSOR", 2, C_DIM);
        fill(36, 156, 568, 1, C_FRAME_DIM);
        for (int i = 0; i < NACT; i++) {
            int y = 162 + i * 18 + (i > 0 ? 1 : 0) * 0;
            char b[16]; upper(b, g_act[i].button[0] ? g_act[i].button : "-", sizeof b);
            if (i == sel) fill(32, y - 1, 576, 18, C_HI_BG);
            SDL_Color fg = i == sel ? C_HI_FG : C_TEXT;
            text_at(40, y, g_act[i].label, 2, fg);
            text_r(600, y, b, 2, i == sel ? C_HI_FG : g_act[i].button[0] ? C_OK : C_DIM);
        }
        if (g_capture) {
            fill(120, 200, 400, 80, C_BG); fill(120, 200, 400, 2, C_FRAME); fill(120, 278, 400, 2, C_FRAME);
            fill(120, 200, 2, 80, C_FRAME); fill(518, 200, 2, 80, C_FRAME);
            text_c(222, "PRESS A BUTTON FOR", 2, C_TEXT);
            text_c(246, g_act[sel].label, 2, C_OK);
        }
        static const char *const h[] = { "A", "SET", "<", "CLEAR", ">", "DEFAULT", "B", "BACK", NULL };
        if (!g_capture) hints(h);
        present();
        int k = poll_key();
        if (g_capture && g_captured >= 0) {
            const char *nm = button_name(g_captured);
            g_capture = 0; g_captured = -1;
            if (nm) {
                for (int i = 0; i < NACT; i++) if (i != sel && !strcmp(g_act[i].button, nm))   /* swap a clash */
                    snprintf(g_act[i].button, sizeof g_act[i].button, "%s", g_act[sel].button);
                snprintf(g_act[sel].button, sizeof g_act[sel].button, "%s", nm);
            }
            SDL_Delay(200);
            continue;
        }
        if (g_capture) { SDL_Delay(16); continue; }
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_UP) sel = (sel + NACT - 1) % NACT;
        if (k == K_DOWN) sel = (sel + 1) % NACT;
        if (k == K_A) { g_capture = 1; g_captured = -1; SDL_Delay(150); }
        if (k == K_LEFT) g_act[sel].button[0] = 0;
        if (k == K_RIGHT) snprintf(g_act[sel].button, sizeof g_act[sel].button, "%s", g_act[sel].def);
        if (k == K_B) return -1;
        SDL_Delay(16);
    }
}

static int page_settings(void) {
    enum { S_FPS, S_LIMIT, S_LOOK, S_CPU, S_JOY, S_BIND, S_RUN, S_CANCEL, S_N };
    static const char *lab[S_N] = { "FPS COUNTER", "FRAME LIMIT", "LOOK SPEED", "CPU", "CONTROLS", "BUTTON MAPPING", "DEPLOY", "STAND DOWN" };
    int sel = S_RUN;
    for (;;) {
        frame_begin("STANDING ORDERS");
        char v[S_N][32] = {{0}};
        snprintf(v[S_FPS], 32, "%s", g_fps ? "ON" : "OFF");
        snprintf(v[S_LIMIT], 32, "%s", g_limit == 30 ? "30 FPS" : g_limit == 60 ? "60 FPS" : "OFF");
        snprintf(v[S_LOOK], 32, "%d%%", g_look);
        snprintf(v[S_CPU], 32, "%s", g_cpu_perf ? "MAX" : "SYSTEM");
        snprintf(v[S_JOY], 32, "%s", g_joy == 1 ? "PAD" : g_joy == 2 ? "BOTH" : "KEYS");
        for (int i = 0; i < S_N; i++) {
            int y = 124 + i * 28 + (i >= S_RUN ? 14 : 0);
            int on = i == sel;
            if (on) fill(32, y - 5, 576, 26, i == S_RUN ? C_OK : C_HI_BG);
            SDL_Color fg = on ? (i == S_RUN ? C_BG : C_HI_FG) : (i == S_RUN ? C_OK : C_TEXT);
            text_at(60, y, lab[i], 2, fg);
            if (on) glyph_at(38, y, '>', 2, fg);
            if (v[i][0]) {
                if (on) { char t[40]; snprintf(t, sizeof t, "< %s >", v[i]); text_r(596, y, t, 2, fg); }
                else text_r(580, y, v[i], 2, C_OK);
            } else if (i == S_BIND) text_r(580, y, ">>", 2, on ? fg : C_DIM);
        }
        static const char *desc[S_N] = {
            "FRAME RATE IN THE TOP-LEFT CORNER", "CAP THE FRAME RATE (SAVES BATTERY)", "RIGHT-STICK AIM SPEED",
            "MAX: FULL CPU SPEED  SYSTEM: FIRMWARE", "", "ASSIGN PAD BUTTONS TO GAME KEYS",
            "SAVE AND START THE GAME", "BACK TO THE FRONTEND" };
        const char *d = desc[sel];
        if (sel == S_JOY) d = g_joy == 1 ? "THE GAME'S OWN XBOX PAD SUPPORT" : g_joy == 2 ? "PAD + KEYS (SWITCH IN GAME MENU)" : "PAD AS KEYBOARD AND MOUSE";
        text_c(378, d, 2, C_DIM);
        static const char *const h[] = { "A", "OK", "<>", "CHANGE", "B", "EXIT", NULL };
        hints(h);
        present();
        int k = poll_key();
        if (k == K_QUIT) return EXIT_USER;
        if (k == K_UP) sel = (sel + S_N - 1) % S_N;
        if (k == K_DOWN) sel = (sel + 1) % S_N;
        if (k == K_LEFT || k == K_RIGHT) {
            int dd = k == K_RIGHT ? 1 : -1;
            if (sel == S_FPS) g_fps = !g_fps;
            if (sel == S_LIMIT) { static const int L[3] = { 0, 30, 60 }; int i = g_limit == 30 ? 1 : g_limit == 60 ? 2 : 0; g_limit = L[(i + 3 + dd) % 3]; }
            if (sel == S_LOOK) { g_look += dd * 10; if (g_look < 30) g_look = 30; if (g_look > 300) g_look = 300; }
            if (sel == S_CPU) g_cpu_perf = !g_cpu_perf;
            if (sel == S_JOY) g_joy = (g_joy + 3 + dd) % 3;
        }
        if (k == K_A) {
            if (sel == S_BIND) { int r = page_bindings(); if (r >= 0) return r; }
            else if (sel == S_RUN) return cfg_save() == 0 ? EXIT_RUN : EXIT_ERR;
            else if (sel == S_CANCEL) return EXIT_USER;
        }
        if (k == K_B) return EXIT_USER;
        SDL_Delay(16);
    }
}

/* ------------------------------------------------------------ import */
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }
typedef struct { int discs, exe, archive, gog; } found;
static void scan_dir(const char *dir, found *f, int depth) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[4200]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 4) scan_dir(p, f, depth + 1); continue; }
        if (!depth && !strncasecmp(e->d_name, "setup_red_faction", 17) && ends_with(e->d_name, ".exe")) f->gog = 1;
        else if (!depth && (ends_with(e->d_name, ".bin") || ends_with(e->d_name, ".iso") || ends_with(e->d_name, ".img"))) f->discs++;
        else if (!strcasecmp(e->d_name, "rf2.exe")) f->exe = 1;
        else if (ends_with(e->d_name, ".zip") || ends_with(e->d_name, ".rar") || ends_with(e->d_name, ".7z")) f->archive = 1;
    }
    closedir(d);
}
#define LOGN 9
static char g_log[LOGN][64];
static void log_push(const char *s) {
    memmove(g_log[0], g_log[1], sizeof g_log[0] * (LOGN - 1));
    snprintf(g_log[LOGN - 1], sizeof g_log[0], "%s", strncmp(s, "[import] ", 9) ? s : s + 9);
}
static int run_import(void) {
    char tool[4200], seven[4200], inno[4200];
    snprintf(tool, sizeof tool, "%s/rered-import.aarch64", g_root);
    const char *z = getenv("RERED_7Z"), *ie = getenv("RERED_INNO");
    snprintf(seven, sizeof seven, "%s", z ? z : "7zzs.aarch64");
    snprintf(inno, sizeof inno, "%s", ie ? ie : "innoextract.aarch64");
    int pfd[2]; if (pipe(pfd)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1); dup2(pfd[1], 2); close(pfd[0]);
        execl(tool, tool, "--source", g_source, "--dest", g_data, "--innoextract", inno, "--7z", seven, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    memset(g_log, 0, sizeof g_log);
    Uint32 t0 = SDL_GetTicks();
    char buf[512]; size_t bl = 0; int status = -1, exited = 0;
    for (;;) {
        ssize_t r = read(pfd[0], buf + bl, sizeof buf - 1 - bl);
        if (r > 0) {
            bl += (size_t)r; buf[bl] = 0;
            char *nl;
            while ((nl = strchr(buf, '\n'))) { *nl = 0; printf("%s\n", buf); log_push(buf); memmove(buf, nl + 1, strlen(nl + 1) + 1); bl = strlen(buf); }
            if (bl == sizeof buf - 1) { log_push(buf); bl = 0; }
        }
        /* The child is reaped once; after that waitpid() no longer reports
         * it, so remember the exit and leave when the pipe is drained (EOF,
         * or nothing left to read). Waiting for "exit and nothing read in
         * the same pass" hung forever when the last line arrived with the
         * exit. */
        if (!exited) {
            int st; pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) { exited = 1; status = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
            else if (w < 0) { exited = 1; }          /* lost the child: do not wait forever */
        }
        if (exited && r <= 0) { if (bl) { log_push(buf); bl = 0; } break; }
        Uint32 s = (SDL_GetTicks() - t0) / 1000;
        frame_begin("SMUGGLING SUPPLIES IN");
        char el[40]; snprintf(el, sizeof el, "ELAPSED %02u:%02u", s / 60, s % 60);
        text_c(126, el, 2, C_OK);
        {   /* an indeterminate bar: a block sweeping back and forth */
            int t = (int)(SDL_GetTicks() / 8 % 960), pos = t < 480 ? t : 960 - t;
            fill(80, 154, 480, 14, C_PANEL); fill(80 + pos * 400 / 480, 154, 80, 14, C_FRAME);
        }
        for (int i = 0; i < LOGN; i++) text_n(36, 190 + i * 22, g_log[i], 35, 2, i == LOGN - 1 ? C_TEXT : C_DIM);
        text_c(410, "KEEP THE HANDHELD POWERED", 2, C_DIM);
        present();
        SDL_Event e; while (SDL_PollEvent(&e)) {}
        if (r <= 0) SDL_Delay(50);
    }
    close(pfd[0]);
    return status;
}
static int page_import(void) {
    for (;;) {
        found f = {0}; scan_dir(g_source, &f, 0);
        int fallback = f.discs >= 2 && (f.exe || f.archive), ready = f.gog || fallback;
        frame_begin("CELL INITIATION");
        text_c(126, "BRING YOUR OWN COPY OF THE GAME:", 2, C_TEXT);
        text_c(152, "THE GOG.COM INSTALLER", 2, C_OK);
        text_c(184, "COPY IT TO", 2, C_DIM);
        text_c(206, "PORTS/RERED_FACTION2/SOURCE/", 2, C_TEXT);
        fill(56, 240, 528, 44, C_PANEL);
        fill(70, 252, 20, 20, ready ? C_OK : C_FRAME_DIM);
        if (ready) text_at(72, 254, "X", 2, C_BG);
        text_at(104, 248, "SETUP_RED_FACTION2_*.EXE", 2, ready ? C_OK : C_RED);
        text_at(104, 266, ready ? "FOUND - READY TO IMPORT" : "NOT FOUND", 1, C_DIM);
        text_c(306, "TAKES A FEW MINUTES, 1.5 GB FREE", 2, C_DIM);
        text_c(330, "INSTALLER CAN BE DELETED AFTER", 2, C_DIM);
        static const char *const hr[] = { "A", "IMPORT", "B", "EXIT", NULL }, *const hs[] = { "A", "SCAN AGAIN", "B", "EXIT", NULL };
        hints(ready ? hr : hs);
        present();
        int k = poll_key();
        if (ready && getenv("RERED_AUTOIMPORT")) k = K_A;      /* tests */
        if (k == K_QUIT || k == K_B) return EXIT_USER;
        if (k == K_A && ready) {
            int rc = run_import();
            if (rc == 0) {
                char m[4200]; snprintf(m, sizeof m, "%s/.installed", g_data);
                FILE *fm = fopen(m, "w"); if (fm) fclose(fm);
                return -1;   /* continue to settings */
            }
            for (;;) {      /* error page */
                frame_begin("IMPORT FAILED");
                for (int i = 0; i < LOGN; i++) text_n(36, 126 + i * 22, g_log[i], 35, 2, i == LOGN - 1 ? C_RED : C_DIM);
                text_c(340, "FULL LOG: RERED_FACTION2/LOG.TXT", 2, C_DIM);
                static const char *const he[] = { "A", "RETRY", "B", "EXIT", NULL };
                hints(he);
                present();
                int k2 = poll_key();
                if (k2 == K_A) break;
                if (k2 == K_B || k2 == K_QUIT) return EXIT_USER;
                SDL_Delay(16);
            }
        }
        SDL_Delay(16);
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--root")) g_root = argv[i + 1];
    if (!g_root) { fprintf(stderr, "usage: rered-launcher --root GAMEDIR\n"); return EXIT_ERR; }
    snprintf(g_conf, sizeof g_conf, "%s/conf", g_root);
    snprintf(g_data, sizeof g_data, "%s/data", g_root);
    snprintf(g_source, sizeof g_source, "%s/source", g_root);
    mkdir(g_conf, 0755); mkdir(g_source, 0755);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return EXIT_ERR; }
    uint32_t fl = SDL_WINDOW_RESIZABLE | (getenv("RERED_WINDOWED") ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_Window *win = SDL_CreateWindow("ReRed Faction II", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, fl);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return EXIT_ERR; }
    R = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!R) R = SDL_CreateRenderer(win, -1, 0);
    if (!R) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return EXIT_ERR; }
    SDL_RenderSetLogicalSize(R, W, H);     /* 4:3, scaled to any screen, letterboxed */
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++) if (SDL_IsGameController(i)) g_pad = SDL_GameControllerOpen(i);
    cfg_load();
    int rc = -1;
    char marker[4200]; snprintf(marker, sizeof marker, "%s/.installed", g_data);
    if (access(marker, F_OK) != 0) rc = page_import();
    else if (getenv("RERED_AUTOSTART")) rc = cfg_save() == 0 ? EXIT_RUN : EXIT_ERR;   /* automated tests */
    if (rc < 0 && getenv("RERED_PAGE") && !strcmp(getenv("RERED_PAGE"), "bindings")) rc = page_bindings();
    if (rc < 0) rc = page_settings();
    SDL_DestroyRenderer(R); SDL_DestroyWindow(win); SDL_Quit();
    return rc;
}
