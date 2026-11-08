#pragma once
#include <stdint.h>
typedef struct { float acc_x, acc_y; int32_t wheel; uint8_t real_buttons, pad_buttons; } rr_mouse;
typedef struct { uint8_t dik, down; uint32_t time, seq; } rr_key_event;
void rr_input_init(void);
void rr_input_pump(void);
void rr_input_keys(uint8_t out[256]);
void rr_input_mouse(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t *buttons);
int  rr_input_next_key(rr_key_event *ev);
void rr_input_flush(void);
typedef struct { int lx, ly, rx, ry, lt, rt; uint32_t buttons; int up, down, left, right; } rr_joy;
int  rr_input_joy(rr_joy *j);     /* SDL controller state; 0 when none */
int  rr_input_has_joy(void);
