#include "home_keys.h"

/* combo partners masked for as long as HOME is physically held */
#define HK_PARTNERS ((u16)(HK_X | HK_UP | HK_DOWN | HK_B))

void hk_reset(home_keys_t *hk) {
    hk->prev = 0;
    hk->home_since = 0;
    hk->home_active = 0;
    hk->inject_until = 0;
    hk->leak = 0;
    hk->dead = 0;
}

void hk_poll(home_keys_t *hk, unsigned b, u32 now, unsigned *events) {
    unsigned ev = 0;
    unsigned pressed = (b & 0xffffu) & ~hk->prev;
    int home_held = (b & HK_HOME) != 0;

    /* combo partners that fired stay masked until physically released;
       everything still held keeps its mask, released buttons drop out */
    hk->leak &= (u16)b;

    int home_new = home_held && !hk->home_active;
    if (home_new) {
        hk->home_active = 1;
        hk->home_since = now;
        hk->dead = 0;
    }
    if (!home_held && hk->home_active) {
        /* release: a short unmodified press becomes a remote HOME pulse;
           a combo (or the 5 s menu) already cancelled that possibility */
        if (!hk->dead && (u32)(now - hk->home_since) < HK_MENU_MS)
            hk->inject_until = now + HK_HOME_PULSE_MS;
        hk->home_active = 0;
    }
    if (hk->home_active) {
        /* a combo fires on the partner's press edge, and also when HOME
           lands while the partner is already held (order independent) */
        unsigned combo = pressed & HK_PARTNERS;
        if (home_new) combo |= (b & HK_PARTNERS);   /* partners already held */
        if (combo & HK_X) {
            ev |= HK_EV_MODE;
            hk->dead = 1;
            hk->leak |= HK_X;
        }
        if (combo & HK_UP) {
            ev |= HK_EV_BRIGHT_UP;
            hk->dead = 1;
            hk->leak |= HK_UP;
        }
        if (combo & HK_DOWN) {
            ev |= HK_EV_BRIGHT_DN;
            hk->dead = 1;
            hk->leak |= HK_DOWN;
        }
        if (!hk->dead && (u32)(now - hk->home_since) >= HK_MENU_MS) {
            ev |= HK_EV_MENU;
            hk->dead = 1;      /* release must not inject a remote HOME */
        }
    }
    hk->prev = b & 0xffffu;
    if (events) *events = ev;
}

u16 hk_filter(const home_keys_t *hk, u16 report_buttons, u32 now_ms) {
    u16 masked = report_buttons;
    /* HOME is never forwarded while physically held: the release path
       injects a clean short pulse instead (hk_injecting below) */
    masked &= (u16)~HK_HOME;
    /* partners of a possible/current combo: stripped while HOME is held,
       plus any partner still covered by the leak guard after release */
    if (hk->home_active) masked &= (u16)~HK_PARTNERS;
    masked &= (u16)~hk->leak;
    if (hk_injecting(hk, now_ms)) masked |= HK_HOME;
    return masked;
}

int hk_injecting(const home_keys_t *hk, u32 now_ms) {
    /* wrap-safe: inject_until is always <= 80 ms in the future */
    return hk->inject_until && (u32)(hk->inject_until - now_ms) <= HK_HOME_PULSE_MS;
}
