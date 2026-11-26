/* Host-side unit tests for the HOME shortcut state machine (pad/home_keys.c).
 * Simulates physical button timelines and checks remote-report filtering,
 * events and leak prevention. No hardware required. */
#include "../pad/home_keys.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %d: %s\n", __LINE__, msg); failures++; } \
} while (0)

static home_keys_t hk;
static unsigned ev;

static void step(unsigned b, u32 now) { hk_poll(&hk, b, now, &ev); }

int main(void) {
    /* 1. standalone short HOME press -> remote press+release pulse, no events */
    hk_reset(&hk); ev = 0;
    step(HK_HOME, 1000);
    CHECK(hk.home_since == 1000, "home_since recorded");
    CHECK(hk_filter(&hk, HK_HOME | HK_A, 1010) == HK_A, "HOME masked while held, A passes");
    step(0, 1200);                       /* release after 200 ms */
    CHECK(ev == 0, "no events for short press");
    u16 f = hk_filter(&hk, HK_A, 1201);
    CHECK(f == (HK_A | HK_HOME), "remote HOME pulse injected on release");
    CHECK(hk_filter(&hk, HK_A, 1290) == HK_A, "pulse ends after ~80 ms");

    /* 2. 5 s hold -> HK_EV_MENU once, no remote HOME on release */
    hk_reset(&hk); ev = 0;
    step(HK_HOME, 0);
    step(HK_HOME, 4999);
    CHECK(ev == 0, "no menu before 5 s");
    step(HK_HOME, 5000);
    CHECK(ev == HK_EV_MENU, "menu event at 5 s");
    ev = 0;
    step(HK_HOME, 6000);
    CHECK(ev == 0, "menu fires only once");
    step(0, 6100);
    CHECK(hk_filter(&hk, HK_A, 6101) == HK_A, "no remote HOME after menu");

    /* 3. HOME+X -> HK_EV_MODE; X never reaches remote, even after HOME release */
    hk_reset(&hk); ev = 0;
    step(HK_HOME, 0);
    step(HK_HOME | HK_X, 50);
    CHECK(ev == HK_EV_MODE, "mode event on HOME+X");
    CHECK(hk_filter(&hk, HK_HOME | HK_X | HK_A, 60) == HK_A, "X masked during hold");
    step(HK_X, 200);                     /* release HOME, keep X held */
    CHECK(hk_filter(&hk, HK_X | HK_A, 210) == HK_A, "X stays masked after HOME release (leak guard)");
    step(0, 300);                        /* X physically released too */
    CHECK(hk_filter(&hk, 0, 310) == 0, "nothing remote once all released");
    step(0, 400);
    CHECK(ev == 0, "no extra events");

    /* 4. HOME+Up / HOME+Down -> brightness events, arrows masked */
    hk_reset(&hk); ev = 0;
    step(HK_HOME, 0);
    step(HK_HOME | HK_UP, 50);
    CHECK(ev == HK_EV_BRIGHT_UP, "brightness up event");
    CHECK(hk_filter(&hk, HK_HOME | HK_UP, 60) == 0, "Up masked");
    ev = 0;
    step(HK_HOME, 60);                   /* release Up, keep HOME */
    step(HK_HOME | HK_DOWN, 100);
    CHECK(ev == HK_EV_BRIGHT_DN, "brightness down event");
    step(0, 200);                        /* all released */
    CHECK(hk_filter(&hk, 0, 210) == 0, "nothing remote after release");

    /* 5. combo cancels pending standalone press AND long-press */
    hk_reset(&hk); ev = 0;
    step(HK_HOME, 0);
    step(HK_HOME | HK_X, 1000);
    step(HK_X, 2000);                    /* HOME released while X still held */
    step(0, 3000);
    CHECK(hk_filter(&hk, 0, 3010) == 0, "no remote HOME after combo release");
    /* and the long-press must not fire either once a combo consumed it */
    hk_reset(&hk); ev = 0;
    step(HK_HOME, 0);
    step(HK_HOME | HK_UP, 100);          /* combo: dead=1 */
    ev = 0;
    step(HK_HOME | HK_UP, 5001);
    CHECK((ev & HK_EV_MENU) == 0, "no menu after combo consumed the hold");
    step(0, 5100);
    CHECK(hk_filter(&hk, 0, 5110) == 0, "no remote HOME after combo+release");

    /* 6. button ordering: X held BEFORE HOME still cannot leak after combo */
    hk_reset(&hk); ev = 0;
    step(HK_X, 0);                       /* X goes remote first */
    CHECK(hk_filter(&hk, HK_X, 10) == HK_X, "X remote before HOME");
    step(HK_X | HK_HOME, 50);            /* HOME joins: combo fires */
    CHECK(ev == HK_EV_MODE, "mode event when HOME joins held X");
    CHECK(hk_filter(&hk, HK_X | HK_HOME, 60) == 0, "X+HOME both masked");
    step(HK_X, 200);
    CHECK(hk_filter(&hk, HK_X, 210) == 0, "held X released remotely by mask");

    /* 7. unrelated buttons pass through while HOME held */
    hk_reset(&hk); ev = 0;
    step(HK_HOME | HK_A | HK_LEFT, 0);
    u16 f2 = hk_filter(&hk, HK_HOME | HK_A | HK_LEFT | HK_Y, 10);
    CHECK(f2 == (HK_A | HK_LEFT | HK_Y), "A/Left/Right/Y unaffected by HOME");
    step(0, 100);

    /* 8. rapid press/release shorter than one report still pulses */
    hk_reset(&hk); ev = 0;
    step(HK_HOME, 0);
    step(0, 30);
    CHECK(hk_injecting(&hk, 31), "inject window open");
    CHECK(hk_filter(&hk, 0, 40) == HK_HOME, "HOME bit present in report");

    /* 9. menu entry suppresses remote input: neutral report path check —
       caller sends zeroed report in ST_INGAME; hk just must not inject */
    hk_reset(&hk); ev = 0;
    step(HK_HOME, 0);
    step(HK_HOME, 5100);                 /* menu event */
    step(0, 5200);
    CHECK(hk_filter(&hk, 0, 5210) == 0, "no inject after menu entry");

    /* 10. a plain short press still works after an earlier combo */
    hk_reset(&hk); ev = 0;
    step(HK_HOME | HK_X, 0);
    CHECK(ev == HK_EV_MODE, "combo fired");
    step(HK_X, 100);                     /* HOME released, X held */
    step(0, 200);
    ev = 0;
    step(HK_HOME, 300);
    step(0, 400);
    CHECK(ev == 0, "plain short press: no events");
    CHECK(hk_filter(&hk, 0, 401) == HK_HOME, "short press still injects");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("home_keys: all tests passed\n");
    return 0;
}
