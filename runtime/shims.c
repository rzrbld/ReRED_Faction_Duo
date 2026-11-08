/* shims.c - registers this port's shim modules on top of win32hle's. */
#include "win32hle.h"
#include "rered.h"
void rr_register_misc(void);
void rr_register_d3d8(void);
void rr_register_dinput(void);
void rr_register_miles(void);
void rr_register_shims(void) {
    rr_register_misc();
    rr_register_d3d8();
    rr_register_dinput();
    rr_register_miles();
}
