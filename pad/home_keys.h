/* home_keys: the ThinClient HOME-button shortcut state machine.
 *
 * Pure logic, no hardware: the caller feeds wup_buttons() bits (the same
 * WUP_* encoding as the 128-byte input report) and a millisecond clock;
 * hk_filter() produces the button bits allowed into the remote 'I' report.
 *
 *   short HOME press   -> remote HOME press+release (injected on release,
 *                         so a long press never leaks a premature remote
 *                         press and release ordering stays observable)
 *   HOME held 5 s      -> HK_EV_MENU once (local pause menu)
 *   HOME + X           -> HK_EV_MODE (live Desktop/Gamepad request)
 *   HOME + D-pad Up    -> HK_EV_BRIGHT_UP   (physical backlight step)
 *   HOME + D-pad Down  -> HK_EV_BRIGHT_DN
 *   HOME + B 2 s       -> handled by the caller (emergency quit, every state)
 *
 * While HOME is physically held, X / Up / Down / B are stripped from the
 * remote report so a recognised combination cannot leak its constituents
 * (HOME+X must not toggle Onboard, HOME+Up must not arrow-key the desktop).
 * A partner that already fired a combo stays masked until it is physically
 * released, so releasing HOME first cannot resurrect a remote press.
 */
#pragma once
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* must match wup.h / the input report's button encoding */
enum {
    HK_A = 0x8000, HK_B = 0x4000, HK_X = 0x2000, HK_Y = 0x1000,
    HK_LEFT = 0x0800, HK_RIGHT = 0x0400, HK_UP = 0x0200, HK_DOWN = 0x0100,
    HK_ZL = 0x0080, HK_ZR = 0x0040, HK_L = 0x0020, HK_R = 0x0010,
    HK_PLUS = 0x0008, HK_MINUS = 0x0004, HK_HOME = 0x0002
};

enum {
    HK_EV_MENU = 1,        /* 5 s hold elapsed: open the local menu */
    HK_EV_MODE = 2,        /* HOME+X edge: request a Desktop/Gamepad switch */
    HK_EV_BRIGHT_UP = 4,   /* HOME+Up edge: one backlight step up */
    HK_EV_BRIGHT_DN = 8    /* HOME+Down edge: one backlight step down */
};

#define HK_MENU_MS       5000u
#define HK_HOME_PULSE_MS 80u    /* remote HOME press duration injected on release */

typedef struct {
    u32 prev;            /* last physical button bits seen by hk_poll */
    u32 home_since;      /* clock_ms() of the HOME press */
    int home_active;     /* HOME is physically held */
    u32 inject_until;    /* remote HOME pulse end (clock_ms) */
    u16 leak;            /* combo partners masked until physical release */
    int dead;            /* standalone press + long-press cancelled by a combo */
} home_keys_t;

void hk_reset(home_keys_t *hk);
/* call once per main-loop iteration with current physical buttons */
void hk_poll(home_keys_t *hk, unsigned buttons, u32 now_ms, unsigned *events);
/* button bits allowed into the remote report (HOME masked unless injecting) */
u16 hk_filter(const home_keys_t *hk, u16 report_buttons, u32 now_ms);
/* nonzero while a remote HOME pulse is being injected */
int hk_injecting(const home_keys_t *hk, u32 now_ms);
