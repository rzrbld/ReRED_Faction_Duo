/*
 * input.c - the port's single SDL event pump and input state.
 *
 * State the guest reads through DirectInput (dinput8.c):
 *   - a 256-byte DIK key array, the OR of real keyboard keys and keys the
 *     controller mapping holds;
 *   - accumulated mouse deltas and buttons (real mouse + right stick/triggers);
 *   - a queue of key transitions for buffered reads.
 * Window messages for the guest (WM_KEYDOWN/UP, WM_CHAR, WM_CLOSE) are posted
 * to the device window, since menus and text entry read those.
 *
 * Controller: on PortMaster, gptokeyb turns the pad into keyboard + mouse
 * events (packaging/rered.gptk), so the game never opens the controller
 * itself (no duplicate input). RERED_PAD=1 enables the built-in table
 * below instead, for desktop testing without gptokeyb: SDL controller
 * inputs to DIK keys / mouse actions. Sticks use a dead zone; the right stick drives
 * mouse motion with an acceleration curve, scaled by RERED_LOOK_SPEED.
 */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "win32hle.h"
#include "input.h"
#include "d3d8_internal.h"

#define WM_CLOSE 0x0010
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102

static uint8_t g_kb_real[256], g_kb_pad[256];
static rr_mouse g_mouse;
static rr_key_event g_q[256];
static unsigned g_qh, g_qt;
static SDL_GameController *g_pad;
static uint32_t g_seq;
static int g_quit_posted;

/* ---- SDL scancode -> DIK (set-1 scan codes) and Win32 VK ---- */
static uint8_t dik_of(SDL_Scancode s) {
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t L[26] = { 0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                       0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C };
        return L[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return (uint8_t)(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F; case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B; case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37; case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A; case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49; case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C; case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_KP_1: return 0x4F; case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52; case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58; case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_KP_DIVIDE: return 0xB5; case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8; case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD; case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1; case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
    default: return 0;
    }
}
/* DIK -> Win32 virtual key, for the WM_KEY* messages a synthesized key posts */
static uint32_t vk_of_dik(uint8_t d) {
    static const char row1[] = "1234567890", rowq[] = "QWERTYUIOP", rowa[] = "ASDFGHJKL", rowz[] = "ZXCVBNM";
    if (d >= 0x02 && d <= 0x0B) return (uint32_t)row1[d - 0x02];
    if (d >= 0x10 && d <= 0x19) return (uint32_t)rowq[d - 0x10];
    if (d >= 0x1E && d <= 0x26) return (uint32_t)rowa[d - 0x1E];
    if (d >= 0x2C && d <= 0x32) return (uint32_t)rowz[d - 0x2C];
    if (d >= 0x3B && d <= 0x44) return 0x70u + (d - 0x3Bu);
    switch (d) {
    case 0x01: return 0x1B; case 0x0E: return 0x08; case 0x0F: return 0x09; case 0x1C: case 0x9C: return 0x0D;
    case 0x1D: case 0x9D: return 0x11; case 0x2A: case 0x36: return 0x10; case 0x38: case 0xB8: return 0x12;
    case 0x39: return 0x20; case 0xC8: return 0x26; case 0xD0: return 0x28; case 0xCB: return 0x25; case 0xCD: return 0x27;
    case 0xC9: return 0x21; case 0xD1: return 0x22; case 0xC7: return 0x24; case 0xCF: return 0x23;
    case 0xD2: return 0x2D; case 0xD3: return 0x2E; case 0x57: return 0x7A; case 0x58: return 0x7B;
    }
    return 0;
}

static void key_event(uint8_t dik, int down) {
    unsigned n = (g_qt - g_qh) & 255;
    if (n == 255) g_qh++;                        /* overload: drop the oldest */
    g_q[g_qt & 255] = (rr_key_event){ dik, (uint8_t)down, SDL_GetTicks(), g_seq++ };
    g_qt++;
    uint32_t vk = vk_of_dik(dik);
    if (g_dev.hwnd && vk) {
        uint32_t lp = 1u | ((uint32_t)dik << 16) | (down ? 0 : 0xC0000000u);
        hle_post_message(g_dev.hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    }
}
static void set_key(uint8_t *arr, uint8_t dik, int down) {
    uint8_t before = g_kb_real[dik] | g_kb_pad[dik];
    arr[dik] = down ? 0x80 : 0;
    uint8_t after = g_kb_real[dik] | g_kb_pad[dik];
    if (before != after) key_event(dik, after != 0);
}

/* ---- controller mapping ----
 * Defaults target the game's stock keyboard layout; docs/CONTROLS.md lists
 * them. Kinds: KEY = hold a DIK key; MB = mouse button; AXIS_KEY = an axis
 * direction past the dead zone holds a key. */
enum { M_KEY, M_MB, M_AXIS_NEG, M_AXIS_POS };
typedef struct { int kind; int src; uint8_t dik; int mb; } pad_map;
#define BTN(b, k)    { M_KEY, SDL_CONTROLLER_BUTTON_##b, k, -1 }
#define BTNMB(b, m)  { M_MB, SDL_CONTROLLER_BUTTON_##b, 0, m }
#define AXN(a, k)    { M_AXIS_NEG, SDL_CONTROLLER_AXIS_##a, k, -1 }
#define AXP(a, k)    { M_AXIS_POS, SDL_CONTROLLER_AXIS_##a, k, -1 }
static pad_map k_pad_map[] = {
    AXN(LEFTY, 0xC8), AXP(LEFTY, 0xD0), AXN(LEFTX, 0xCB), AXP(LEFTX, 0xCD),   /* move / drive: arrows */
    BTN(DPAD_UP, 0xC8), BTN(DPAD_DOWN, 0xD0), BTN(DPAD_LEFT, 0xCB), BTN(DPAD_RIGHT, 0xCD),
    BTN(A, 0x1C),            /* A: enter / action */
    BTN(B, 0x39),            /* B: space (jump / handbrake) */
    BTN(X, 0x13),            /* X: R (reload) */
    BTN(Y, 0x12),            /* Y: E (enter / exit car, use) */
    BTN(START, 0x01),        /* Start: Esc (pause menu) */
    BTN(BACK, 0x0F),         /* Select: Tab (map) */
    BTN(LEFTSHOULDER, 0x10), /* LB: Q (previous weapon / look left) */
    BTN(RIGHTSHOULDER, 0x11),/* RB: W? reserved; remapped in config */
    BTN(LEFTSTICK, 0x2A),    /* L3: shift (walk / run toggle) */
    BTN(RIGHTSTICK, 0x1D),   /* R3: ctrl (crouch) */
    AXP(TRIGGERLEFT, 0x00),  /* LT: right mouse (aim) - see mouse handling below */
    AXP(TRIGGERRIGHT, 0x00), /* RT: left mouse (fire) */
};
#define NMAP (sizeof k_pad_map / sizeof *k_pad_map)

static int g_pad_keys;   /* RERED_PAD=1: the built-in key/mouse table below */
static void pad_update(void) {
    if (!g_pad || !g_pad_keys) return;
    const int dz = 9000;
    uint8_t want[256] = {0};
    for (size_t i = 0; i < NMAP; i++) {
        const pad_map *m = &k_pad_map[i];
        int on = 0;
        if (m->kind == M_KEY) on = SDL_GameControllerGetButton(g_pad, (SDL_GameControllerButton)m->src);
        else if (m->kind == M_AXIS_NEG) on = SDL_GameControllerGetAxis(g_pad, (SDL_GameControllerAxis)m->src) < -dz;
        else if (m->kind == M_AXIS_POS) on = SDL_GameControllerGetAxis(g_pad, (SDL_GameControllerAxis)m->src) > dz;
        if (on && m->dik) want[m->dik] = 0x80;
    }
    for (int k = 0; k < 256; k++) if (want[k] != g_kb_pad[k]) set_key(g_kb_pad, (uint8_t)k, want[k] != 0);
    /* triggers -> mouse buttons; right stick -> mouse motion */
    int lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
    int rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
    g_mouse.pad_buttons = (uint8_t)((rt ? 1 : 0) | (lt ? 2 : 0));
    static float speed = -1;
    if (speed < 0) speed = (float)rr_config_int("look_speed", 100) / 100.f;
    float rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f;
    float ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f;
    float mag = sqrtf(rx * rx + ry * ry);
    if (mag > 0.18f) {
        float t = (mag - 0.18f) / 0.82f, curve = t * t * 18.f * speed;   /* pixels per poll at full tilt */
        g_mouse.acc_x += rx / mag * curve;
        g_mouse.acc_y += ry / mag * curve;
    }
}

/* RERED_SCRIPT (tests): timed input, "T:CMD;T:CMD;..." with T in seconds
 * since the input layer started and CMD one of
 *   m DX DY   relative mouse move (accumulated like a real mouse)
 *   c         left click (press now, release 150 ms later)
 *   k DIK     tap a key (DirectInput scan code, decimal; press + release)
 * Used to drive menus on the device for unattended profiling. */
static void script_step(void) {
    static const char *s; static uint32_t t0; static int init;
    static uint32_t release_click, release_key; static int key_down = -1;
    if (!init) { init = 1; s = getenv("RERED_SCRIPT"); t0 = SDL_GetTicks(); }
    uint32_t now = SDL_GetTicks();
    if (release_click && now >= release_click) { g_mouse.real_buttons &= (uint8_t)~1; release_click = 0; }
    if (key_down >= 0 && now >= release_key) { set_key(g_kb_real, (uint8_t)key_down, 0); key_down = -1; }
    while (s && *s) {
        double t = strtod(s, NULL);
        if (now - t0 < (uint32_t)(t * 1000)) return;
        const char *c = strchr(s, ':'); if (!c) { s = NULL; return; }
        c++;
        if (*c == 'm') { int dx = 0, dy = 0; sscanf(c + 1, "%d %d", &dx, &dy); g_mouse.acc_x += (float)dx; g_mouse.acc_y += (float)dy; }
        else if (*c == 'c') { g_mouse.real_buttons |= 1; release_click = now + 150; }
        else if (*c == 'k') { int k = 0; sscanf(c + 1, "%d", &k); if (k > 0 && k < 256) { set_key(g_kb_real, (uint8_t)k, 1); key_down = k; release_key = now + 150; } }
        fprintf(stderr, "[script] %.1fs: %.*s\n", (now - t0) / 1000.0, (int)strcspn(c, ";"), c);
        s = strchr(s, ';'); if (s) s++;
    }
}

void rr_input_pump(void) {
    script_step();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) rr_update_output_rect();
            break;
        case SDL_QUIT:
            if (!g_quit_posted && g_dev.hwnd) { hle_post_message(g_dev.hwnd, WM_CLOSE, 0, 0); g_quit_posted = 1; }
            break;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            if (e.key.repeat) break;
            /* gptokeyb cannot send mouse-wheel events; rered.gptk sends Pause
             * for "next weapon" and it becomes one wheel-up step here. */
            if (e.key.keysym.scancode == SDL_SCANCODE_PAUSE && rr_config_int("pause_wheel", 1)) {
                if (e.type == SDL_KEYDOWN) g_mouse.wheel += 120;
                break;
            }
            uint8_t d = dik_of(e.key.keysym.scancode);
            if (d) set_key(g_kb_real, d, e.type == SDL_KEYDOWN);
            break;
        }
        case SDL_TEXTINPUT:
            if (g_dev.hwnd) for (const char *c = e.text.text; *c; c++) if ((unsigned char)*c < 0x80) hle_post_message(g_dev.hwnd, WM_CHAR, (uint8_t)*c, 1);
            break;
        case SDL_MOUSEMOTION: g_mouse.acc_x += (float)e.motion.xrel; g_mouse.acc_y += (float)e.motion.yrel; break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            if (b >= 0) { if (e.type == SDL_MOUSEBUTTONDOWN) g_mouse.real_buttons |= (uint8_t)(1 << b); else g_mouse.real_buttons &= (uint8_t)~(1 << b); }
            break;
        }
        case SDL_MOUSEWHEEL: g_mouse.wheel += e.wheel.y * 120; break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!g_pad) { g_pad = SDL_GameControllerOpen(e.cdevice.which); if (g_pad) fprintf(stderr, "[input] controller: %s\n", SDL_GameControllerName(g_pad)); }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad); g_pad = NULL;
                memset(g_kb_pad, 0, sizeof g_kb_pad); g_mouse.pad_buttons = 0;
            }
            break;
        }
    }
    pad_update();
}

void rr_input_init(void) {
    static int done;
    if (done) return;
    done = 1;
    SDL_InitSubSystem(SDL_INIT_EVENTS);
    g_pad_keys = rr_config_int("pad", 0);
    /* RERED_JOY=1 (opt-in): the controller goes to the game as a DirectInput
     * joystick (dinput8.c, Xbox 360 layout), with PortMaster's SDL mapping.
     * Both off: gptokeyb supplies keys and mouse only. */
    if (!g_pad_keys && !rr_config_int("joy", 0)) return;
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    const char *db = getenv("SDL_GAMECONTROLLERCONFIG_FILE");
    if (db) SDL_GameControllerAddMappingsFromFile(db);
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++)
        if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); if (g_pad) fprintf(stderr, "[input] controller: %s\n", SDL_GameControllerName(g_pad)); }
}

/* Raw controller state for the DirectInput joystick (dinput8.c). 0: none. */
int rr_input_joy(rr_joy *j) {
    if (!g_pad) return 0;
    SDL_GameController *c = g_pad;
    j->lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);  j->ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    j->rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX); j->ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    j->lt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT); j->rt = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    static const SDL_GameControllerButton order[10] = {   /* XInput-through-DirectInput button order */
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK };
    j->buttons = 0;
    for (int i = 0; i < 10; i++) if (SDL_GameControllerGetButton(c, order[i])) j->buttons |= 1u << i;
    j->up = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP);   j->down = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    j->left = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT); j->right = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    return 1;
}
int rr_input_has_joy(void) { rr_input_init(); rr_input_pump(); return g_pad != NULL; }

void rr_input_keys(uint8_t out[256]) { for (int i = 0; i < 256; i++) out[i] = g_kb_real[i] | g_kb_pad[i]; }
void rr_input_mouse(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t *buttons) {
    int32_t x = (int32_t)g_mouse.acc_x, y = (int32_t)g_mouse.acc_y;
    g_mouse.acc_x -= (float)x; g_mouse.acc_y -= (float)y;     /* keep the fraction */
    *dx = x; *dy = y; *dz = g_mouse.wheel; g_mouse.wheel = 0;
    *buttons = g_mouse.real_buttons | g_mouse.pad_buttons;
}
int rr_input_next_key(rr_key_event *ev) {
    if (g_qh == g_qt) return 0;
    *ev = g_q[g_qh++ & 255];
    return 1;
}
void rr_input_flush(void) { g_qh = g_qt; }
