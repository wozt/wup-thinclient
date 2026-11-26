/* WUP ThinClient: a WUP ThinServer desktop on the Wii U GamePad.
 *
 * This is the WUP Stream pad receiver (wup-stream/pad/main.c, upstream
 * e1b0430) adapted into a standalone homebrew with its own identity and a
 * local connection menu: manual server IP entry, saved configuration and a
 * disconnect action. The wire protocol is unchanged — see
 * wup-thinserver/docs/PROTOCOL.md.
 *
 * Protocol (UDP, little endian). The pad listens on port 50000.
 *   pad -> broadcast:50001  'B' version 1 0                      every second while no server is connected
 *   server -> pad   'H' version flags(1 = send the microphone) light  hello / keepalive (every 250 ms); light: the
 *                                                                 backlight 1..6 (0 = leave it), bit 6 = off, bit 7 = keep it
 *                'V' flags(1 = keyframe) 0 0, u32 frame, u32 offset, u32 total, then <= 1400 bytes
 *                'A' 0 0 0, u32 seq, then 48 kHz 16-bit stereo PCM (a multiple of 16 bytes)
 *                'R' on(1)/off(0)                                 the rumble motor (said again with every frame while on)
 *                'Q'                                             the server is done
 *   pad -> server 'M' 0 0 0, u32 sample number, then the microphone: 16-bit mono 48 kHz
 *   pad -> server 'N' count 0 0, then count x {u32 frame, u16 packet (0xffff: all of it), u16 0}: send these again
 *   pad -> server 'I' flags(1 = send a keyframe; bits 1-3: frames waiting for the decoder), u16 clock ticks (~1 us)
 *                since the screen's last refresh, u8 backlight (1..6, 0 = off), 0 0 0, the 128-byte input report
 * Video is the frame's raw CABAC slice data: it goes straight into the hardware decoder's bitstream ring.
 * The app's own screen (RGB565 at 0x221400) is the decoder's frame buffers: it is never shown while video plays,
 * and drawn again from scratch when a stream ends.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "wup.h"
#include "armcpu.h"
#include "wup_hw.h"                                   /* (hw_backlight_percent: the table's levels) */
#include "fs.h"                                       /* saved server configuration */

WUP_APP("WUP ThinClient");
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define REG(a)  (*(volatile u32 *)(a))
#define LCD(o)  REG(0xf0009400u + (o))
#define CLOCK   REG(0xf0000408u)                              /* free-running, ~997.3 kHz */
#define REPORT  ((const volatile u8 *)0x00178d38u)            /* the latest 128-byte input report */

static u32 ms_, last_clk, frac;
static u32 clock_ms(void) { u32 c = CLOCK; frac += c - last_clk; last_clk = c; ms_ += frac / 997; frac %= 997; return ms_; }

/* ---- local configuration: the WUP ThinServer IP ("" = any) ----------------- */
#define CFG_PATH "/save/thinclient.cfg"
static char server_ip[16];          /* dotted quad or "" for any server */
static int auto_connect;            /* connect on start and after a lost stream */

static void cfg_load(void) {
    server_ip[0] = 0;
    auto_connect = 0;
    if (!wup_fs_ok()) return;
    char buf[96];
    memset(buf, 0, sizeof buf);
    int n = fs_read(CFG_PATH, 0, buf, (u32)sizeof buf - 1);
    if (n <= 0) return;
    buf[sizeof buf - 1] = 0;
    /* first whitespace-separated token is the IP ("any" = no filter);
       auto_connect is a later "auto=1" token for backward compatibility */
    int i = 0;
    while (i < (int)sizeof buf - 1 && buf[i] && buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\r') i++;
    if (i > 0 && i < (int)sizeof server_ip) {
        if (i == 3 && !memcmp(buf, "any", 3)) {
            server_ip[0] = 0;
        } else {
            memcpy(server_ip, buf, (size_t)i);
            server_ip[i] = 0;
        }
    }
    if (strstr(buf, "auto=1")) auto_connect = 1;
}

static void cfg_save(void) {
    if (!wup_fs_ok()) return;
    char buf[40];
    snprintf(buf, sizeof buf, "%s auto=%d",
             server_ip[0] ? server_ip : "any", auto_connect);
    fs_write(CFG_PATH, buf, (u32)strlen(buf) + 1);
}

static u32 conn_target;                          /* forward: used by ip_matches */
static int ip_matches(const struct sockaddr_in *from) {
    if (conn_target) return from->sin_addr.s_addr == conn_target;
    if (!server_ip[0]) return 1;                    /* any server */
    return from->sin_addr.s_addr == inet_addr(server_ip);
}

/* ---- local UI state --------------------------------------------------------- */
enum { ST_MENU, ST_EDIT, ST_CONNECTING, ST_STREAMING, ST_INGAME };
static int state = ST_MENU;
static int menu_sel;                             /* menu row / ingame row */
static int ingame_sub;                           /* 0 = actions, 1 = controls help */
static char status_msg[80];                      /* menu status line */
static u32 conn_deadline;                        /* connect attempt timeout */
static int next_state = ST_MENU;                 /* where end_session() lands */
static u32 last_peer_ip;                         /* last server that streamed */
static int quit_app;                             /* menu Exit / emergency quit */
static int edit_field;                           /* 0..3 octet, 4 = "any" toggle */
static int edit_val[4];
static int edit_any;                             /* editing "any server" mode */

/* ---- the server ------------------------------------------------------------- */
static int net_ok;
static int sock = -1;
static struct sockaddr_in peer_sa;
static u32 peer_on, last_ms, quiet_until, beacon_ms, in_ms, stat_ms;
static u16 in_seq;
static void send_to(const struct sockaddr_in *to, const void *d, u32 n) { sendto(sock, d, n, 0, (const struct sockaddr *)to, sizeof *to); }
static void send_peer(const void *d, u32 n) { if (peer_on) send_to(&peer_sa, d, n); }

/* ---- video: the hardware decoder. Every keyframe restarts it (the firmware's stream-start sequence: LVC 0x17a10 stop,
   ring restart, 0x17ee0 start): with no slice headers it cannot tell a keyframe otherwise. */
#define VDEC(o) REG(0xf0008480u + (o))
#define ENG(o)  REG(0xf0008c00u + (o))
#define RST844  REG(0xf0000844u)
#define DRV(o)  REG(0x000bb2f0u + (o))                       /* drvvout state: +8 row mode, +0xc force decoding */
static u32 rbase, rsize, wp, von, need_idr = 1, vlen, vstart, vidr, frames;
static u32 keys, lost, stalls, gap_max, poll_ms, full, qmax, t_start;   /* (counters, logged every 5 s) */
static u32 wd_rp, wd_ms, lcd80, shown, lit_at, lit_start, lit_len;
/* WUP_FLIGHT_0903: no MMIO reads or decoder mutations in the diagnostic thread. */
#define DIAG_COUNT 64
static volatile u32 diag_run, diag_active, diag_seq, diag_progress, diag_stage;
static int diag_sock = -1;
static struct { u32 seq, stage, a, b; } diag_ring[DIAG_COUNT];
static void diag_event(u32 stage, u32 a, u32 b) {
    u32 seq = diag_seq + 1;
    diag_ring[seq % DIAG_COUNT].seq = 0;
    __asm__ volatile("" ::: "memory");
    diag_ring[seq % DIAG_COUNT].stage = stage;
    diag_ring[seq % DIAG_COUNT].a = a;
    diag_ring[seq % DIAG_COUNT].b = b;
    __asm__ volatile("" ::: "memory");
    diag_ring[seq % DIAG_COUNT].seq = seq;
    diag_stage = stage;
    diag_seq = seq;
    ++diag_progress;
}
static void diag_worker(void *arg) {
    (void)arg;
    u32 prev = 0, unchanged = 0, serial = 0;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(28804), .sin_addr.s_addr = 0xffffffffu };
    static u32 msg[8 + DIAG_COUNT * 4];
    while (diag_run) {
        wup_thread_sleep(250);
        if (!diag_run) break;
        if (!diag_active) { unchanged = 0; prev = diag_progress; continue; }
        unsigned irq = wup_irq_off();
        u32 progress = diag_progress;
        unchanged = progress == prev ? unchanged + 1 : 0;
        prev = progress;
        msg[0] = 0x31444657u; /* WFD1 */
        msg[1] = ++serial; msg[2] = progress; msg[3] = diag_stage;
        msg[4] = unchanged; msg[5] = diag_seq;
        msg[6] = DIAG_COUNT; msg[7] = 0;
        memcpy(msg + 8, diag_ring, sizeof diag_ring);
        wup_irq_restore(irq);
        if (diag_sock >= 0) sendto(diag_sock, msg, sizeof msg, MSG_DONTWAIT,
                                  (const struct sockaddr *)&to, sizeof to);
    }
}

static void stop_engine(void) {
    diag_event(10, wp, von);
    u32 row = DRV(8), ack = row ? 0x200 : 0x218;
    ENG(0x00) |= 0x20000;
    ENG(0x74) |= row ? 0x200 : 0x21a;
    for (int i = 0; i < 200 && (ENG(0x74) & ack) != ack; i++) wup_thread_sleep(1);
    ENG(0x74) |= 0x80;
    ENG(0x74) &= row ? ~0x280u : 0xfffffd65u;
    VDEC(0x38) = 0;
    diag_event(11, wp, von);
}
static void start_engine(void) {
    diag_event(12, wp, vlen);
    VDEC(0x0c) = 2 | (DRV(0xc) ? 1 : 0);
    ENG(0x70) &= ~(DRV(8) ? 0xb000u : 0xa000u);
    VDEC(0x30) |= 0x00ff0000; VDEC(0x30) &= ~0x00030000u;
    if (!(LCD(0x84) & 4)) { ENG(0x20) = 0; RST844 |= 0x00300000; RST844 &= ~0x00300000u; }
    RST844 |= 0x30000000; RST844 &= ~0x30000000u;
    ENG(0x00) |= 0x10000;
    diag_event(13, wp, vlen);
}
static u32 ring_free(void) {
    if (!von) return rsize - 16;
    u32 rp = (VDEC(0x2c) & 0xffffff) - rbase;
    /* the decoder reads a little past the queued end: a read pointer just ahead of us means empty, not full */
    if ((rp + rsize - wp) % rsize < 4096) return rsize - 4096;
    u32 used = (wp + rsize - rp) % rsize;
    return used + 4096 < rsize ? rsize - used - 4096 : 0;
}
/* The decoder's own queue holds 4 frames (VDEC +0x34 low bits; Nintendo's driver feeds it only below that, LVC 0x18996)
   and it starts at most one frame per screen refresh (measured). What does not fit waits here; the count of all frames
   waiting goes to the computer with every input packet, and it skips a refresh when that reaches 2. */
static u32 pend[8], npend;
static void feed(void) {
    while (npend && (VDEC(0x34) & 7) < 4) { VDEC(0x00) = pend[0]; npend--; for (u32 i = 0; i < npend; i++) pend[i] = pend[i + 1]; }
}
static u32 waiting(void) { u32 n = (VDEC(0x34) & 7) + npend; return n > 7 ? 7 : n; }
static void frame_done(void) {
    if (!vidr && !von) { need_idr = 1; return; }     /* (stopped: only a keyframe starts it; this one is dropped) */
    u32 end = rbase + (vstart + vlen) % rsize;
    if (vidr) {
        stop_engine();
        VDEC(0x0c) = 0;
        u32 b = VDEC(0x24);
        VDEC(0x24) = 0; VDEC(0x24) = (rbase + vstart) & 0xffffff; VDEC(0x24) = b;
        VDEC(0x00) = (end & 0xffffff) | 1u << 24;
        start_engine();
        von = 1; keys++; npend = 0;
        if (shown == 2) shown = 1;                        /* (not lit yet: count 3 again from here) */
        if (shown == 1) lit_at = 0;
    } else if (npend == 8) { von = 0; need_idr = 1; stalls++; return; }   /* (it is not taking them: start again) */
    else { pend[npend++] = (end & 0xffffff) | 1u << 24; feed(); }
    wp = (vstart + vlen) % rsize; need_idr = 0; frames++;
    if (!shown) {             /* the video layer, dark until the decoder has read 3 frames (no stale picture, no hang shown) */
        lcd80 = LCD(0x80); gfx_brightness(0); LCD(0x80) = (lcd80 | 4) & ~0x10u;
        shown = 1; lit_at = 0;
    }
    if (shown == 1 && ++lit_at == 3) { lit_start = vstart; lit_len = vlen; shown = 2; }   /* (light when it is read) */
}
/* Frames come in packets of 1400 bytes, and Wi-Fi loses some. The frames waiting for the decoder (the cushion) give the
   time to get a lost packet again: up to 4 frames are put together at once, each at its place in the ring, and go to
   the decoder in order as they become whole. What is missing is asked for again ('N'): when later packets show the
   gap, or nothing came for 6 ms. A frame still not whole after 45 ms is given up: a keyframe then. */
#define NASM 4
static struct { u32 len, npk, have, got[4], t_rx, t_nack, t_head; u8 known, idr; } as[NASM];
static u32 next_f, seen_f, sync, resent;
static void nack(u32 now);
static void as_reset(u32 f) { memset(as, 0, sizeof as); next_f = seen_f = f; sync = 1; }
static void video(const u8 *d16, u32 len, const u8 *h, u32 now) {      /* d16: the data after the 16-byte header */
    u32 fno = *(u32 *)(h + 4), off = *(u32 *)(h + 8), total = *(u32 *)(h + 12), n = len - 16u, idr = h[1] & 1;
    if (idr && (!sync || fno != next_f || !as[0].idr)) {       /* a keyframe: the decoder starts again with it, at the ring's start */
        if (sync && (int)(fno - next_f) < 0) return;           /* (an old one, again) */
        diag_event(20, fno, wp);
        as_reset(fno); wp = 0; as[0].idr = 1; as[0].t_head = now;
    }
    if (!sync || (need_idr && !as[0].idr)) return;             /* waiting for a keyframe */
    int d = (int)(fno - next_f);
    if (d < 0) return;                                         /* (already whole: a packet sent twice) */
    if (d >= NASM) { lost++; need_idr = 1; sync = 0; return; } /* too far ahead: we lost the thread */
    if ((int)(fno - seen_f) > 0) seen_f = fno;
    u32 start = wp;
    for (int i = 0; i < d; i++) { if (!as[i].known) { nack(now); return; } start += as[i].len; }   /* (its place is not known yet) */
    if (!as[d].known) {
        if (!total || total > rsize - 4096 || (!as[0].idr && start - wp + total > ring_free())) { full++; need_idr = 1; sync = 0; return; }
        as[d].known = 1; as[d].len = total; as[d].npk = (total + 1399) / 1400; as[d].have = 0; as[d].idr = (u8)idr;
        memset(as[d].got, 0, sizeof as[d].got);
        if (as[d].npk > 128) { need_idr = 1; sync = 0; return; }
    }
    u32 k = off / 1400;
    if (off % 1400 || k >= as[d].npk || off + n > as[d].len || (n != 1400 && off + n != as[d].len)) return;
    if (!(as[d].got[k / 32] >> (k % 32) & 1)) {
        u32 at = (start + off) % rsize, first = n < rsize - at ? n : rsize - at;
        diag_stage = 21;
        memcpy((u8 *)(rbase + at), d16, first); cache_clean((u8 *)(rbase + at), first);
        if (n > first) { memcpy((u8 *)rbase, d16 + first, n - first); cache_clean((u8 *)rbase, n - first); }
        diag_stage = 22;
        as[d].got[k / 32] |= 1u << (k % 32); as[d].have++;
    }
    as[d].t_rx = now;
    if (d > 0 && as[d - 1].have != as[d - 1].npk) nack(now);   /* a later frame has begun: what the earlier lacks is lost */
    while (as[0].known && as[0].have == as[0].npk) {           /* whole frames, in order, to the decoder */
        vstart = wp; vlen = as[0].len; vidr = as[0].idr;
        diag_event(30, next_f, vlen);
        frame_done();
        diag_stage = 31;                                          /* (moves wp past it) */
        if (!vidr && !von) { sync = 0; return; }
        for (int i = 0; i < NASM - 1; i++) as[i] = as[i + 1];
        memset(&as[NASM - 1], 0, sizeof as[0]);
        next_f++; as[0].t_head = now;
    }
}
/* The decoder stops for good when a screen refresh comes and it has nothing queued: its read position still, frames
   waiting in its queue. Only a keyframe restarts it (Nintendo's driver too: LVC 0x18138). Idle after 60 ms. */
static void watchdog(u32 now) {
    diag_stage = 50;
    feed();
    u32 rp = VDEC(0x2c) & 0xffffff, at = rp - rbase, q = (wp + rsize - at) % rsize;
    if (q < rsize - 4096 && q >> 10 > qmax) qmax = q >> 10;
    if (shown == 2 && (at - lit_start + rsize) % rsize >= lit_len && (at - lit_start + rsize) % rsize < lit_len + 0x8000) { gfx_brightness(256); shown = 3; }
    static u32 wd_n;
    u32 n = VDEC(0x34) & 7;                                 /* (tiny frames barely move the read position: the count does) */
    if (!von || rp != wd_rp || n != wd_n || !n) { wd_rp = rp; wd_n = n; wd_ms = now; return; }
    if (now - wd_ms > 60) { von = 0; need_idr = 1; wd_ms = now; stalls++; }
}
/* ask for what is missing of the frames being put together: 'N' count 0 0, then per packet u32 frame, u16 index
   (0xffff: the whole frame: none of it came), u16 0. A frame at most every 8 ms. */
static void nack(u32 now) {
    u8 m[4 + 8 * 40] = { 'N' }; u32 c = 0;
    for (int i = 0; i < NASM && (int)(next_f + i - seen_f) <= 0 && c < 40; i++) {
        int newest = next_f + i == seen_f;
        if (as[i].known && as[i].have == as[i].npk) continue;
        if (now - as[i].t_nack < 8 && as[i].t_nack) continue;
        if (newest && as[i].known && now - as[i].t_rx < 6) continue;   /* (still coming) */
        u32 f = next_f + i;
        if (!as[i].known) { memcpy(m + 4 + 8 * c, &f, 4); m[8 + 8 * c] = m[9 + 8 * c] = 0xff; m[10 + 8 * c] = m[11 + 8 * c] = 0; c++; }
        else for (u32 k = 0; k < as[i].npk && c < 40; k++)
            if (!(as[i].got[k / 32] >> (k % 32) & 1)) { memcpy(m + 4 + 8 * c, &f, 4); m[8 + 8 * c] = (u8)k; m[9 + 8 * c] = (u8)(k >> 8); m[10 + 8 * c] = m[11 + 8 * c] = 0; c++; }
        as[i].t_nack = now ? now : 1;
    }
    if (!c || !peer_on) return;
    m[1] = (u8)c; resent += c;
    send_peer(m, 4 + 8 * c);
}

/* ---- sound: libwup's speaker ring, at most 128 ms queued */
static void audio(const u8 *d8, u32 len) {
    u32 n = (len - 8u) & ~15u;
    diag_stage = 51;
    if (n && wup_audio_queued() < 48000 * 128 / 1000) wup_audio_queue((const int16_t *)d8, (int)(n / 4));
    diag_stage = 52;
}
/* ---- the motor: on while the server keeps saying so (with every frame), off 100 ms after the last "on" */
static u32 motor_ms, motor_on;
static void motor(int on, u32 now) {
    if (on) { if (!motor_on) wup_rumble(1); motor_on = 1; motor_ms = now; } else if (motor_on) { wup_rumble(0); motor_on = 0; }
}
/* ---- the microphone */
static u32 mic_on, mic_seq;
static void mic(int on) { if (on == (int)mic_on) return; if (on) wup_mic_open(); else wup_mic_close(); mic_on = (u32)on; }
static void mic_poll(void) {
    static u8 m[8 + 1024] __attribute__((aligned(4))) = { 'M' };
    for (;;) {
        int n = wup_mic_read((int16_t *)(m + 8), 512);
        if (n <= 0) return;
        memcpy(m + 4, &mic_seq, 4); mic_seq += (u32)n;
        send_peer(m, 8 + 2 * (u32)n);
    }
}
/* ---- the backlight: the server's level while it streams (1..6: the pad's own table, as wupstream counts them; libwup
   takes a percentage), off and on. Told back to the server as a level (1..6), the menu's own taken to the nearest. */
/* Last remote command is separate from the locally selected level. */
static u32 light_now, light_off;
static int light_level, light_override;
static int level_now(void) {
    if (light_level) return light_level;
    int p = wup_backlight_level(), best = 1;
    for (int l = 2; l <= 6; l++) if (abs(hw_backlight_percent(l) - p) < abs(hw_backlight_percent(best) - p)) best = l;
    return best;
}
static void apply_light(void) {
    int percent = light_off ? 0 : hw_backlight_percent(level_now());
    wup_backlight(percent);
}
static void light(u32 v) {
    if (v == light_now) return; /* repeated keepalive is not a new choice */
    light_now = v;
    if (v & 0x40) { light_off = 1; light_override = 1; apply_light(); return; }
    int level = (int)(v & 7);
    if (level >= 1 && level <= 6) {
        light_level = level; light_off = 0; light_override = 1; apply_light();
    } else if (v == 0) {
        /* No remote override: retain any local selection. */
    }
}
static void local_light_step(int delta) {
    int level = level_now() + delta;
    if (level < 1) level = 1;
    if (level > 6) level = 6;
    light_level = level; light_off = 0; light_override = 1;
    apply_light();
}
/* No periodic backlight writes: only explicit commands and session exit. */

/* ---- local screens ------------------------------------------------------------ */
static void draw_brand(int y, const char *sub) {
    gfx_vgrad24(0, 0, GFX_W, GFX_H, C24(16, 32, 64), C24(5, 8, 20));
    gfx_text_c(&font_big, GFX_W / 2, y, "WUP ThinClient", RGB(120, 200, 255));
    if (sub) gfx_text_c(&font_body, GFX_W / 2, y + 44, sub, RGB(190, 200, 225));
}

/* ---- menu geometry (shared by drawing and touch hit-testing) ----------------- */
static void draw_edit(void);
static void edit_from_saved(void);
#define MI_MAX 8
static int mi_y[MI_MAX], mi_h[MI_MAX], mi_n;

static void menu_item(int i, int y, int h, const char *label, int sel) {
    mi_y[i] = y; mi_h[i] = h;
    if (sel) {
        gfx_rrect(GFX_W / 2 - 210, y, 420, h, 10, RGB(46, 96, 168), 220);
        gfx_rrect_outline(GFX_W / 2 - 210, y, 420, h, 10, 2, RGB(120, 190, 255));
        gfx_text_c(&font_body, GFX_W / 2, y + h / 2 + 7, label, RGB(255, 255, 255));
    } else {
        gfx_rrect(GFX_W / 2 - 210, y, 420, h, 10, RGB(24, 32, 52), 200);
        gfx_text_c(&font_body, GFX_W / 2, y + h / 2 + 7, label, RGB(190, 205, 230));
    }
}

static void draw_server_line(void) {
    char line[64];
    if (server_ip[0]) snprintf(line, sizeof line, "Server: %s", server_ip);
    else snprintf(line, sizeof line, "Server: any (first found)");
    gfx_text_c(&font_body, GFX_W / 2, 168, line, RGB(255, 255, 255));
}

static void draw_menu(void) {
    wup_screen();
    draw_brand(84, "Linux desktop on the GamePad");
    draw_server_line();
    gfx_text_c(&font_small, GFX_W / 2, 196, wup_net_ip_str(), RGB(140, 155, 190));
    if (status_msg[0])
        gfx_text_c(&font_body, GFX_W / 2, 232, status_msg, RGB(255, 210, 120));
    char ac[40];
    snprintf(ac, sizeof ac, "Auto-connect: %s", auto_connect ? "on" : "off");
    static const char *const labels[] = { "Connect", "Edit server address", NULL, "Exit" };
    mi_n = 0;
    for (int i = 0; i < 4; i++) {
        const char *lb = labels[i] ? labels[i] : ac;
        menu_item(mi_n++, 268 + i * 46, 38, lb, menu_sel == i);
    }
    gfx_text_c(&font_small, GFX_W / 2, 480 - 18,
               "D-pad + A   touch   B: quit   HOME+B: quit", RGB(120, 135, 170));
    gfx_present_all();
}

static void draw_connecting(void) {
    wup_screen();
    draw_brand(170, NULL);
    char line[64];
    if (conn_target)
        snprintf(line, sizeof line, "Reconnecting to %s ...", inet_ntoa((struct in_addr){ conn_target }));
    else if (server_ip[0])
        snprintf(line, sizeof line, "Looking for %s ...", server_ip);
    else
        snprintf(line, sizeof line, "Looking for any WUP ThinServer ...");
    gfx_text_c(&font_body, GFX_W / 2, 260, line, RGB(255, 255, 255));
    gfx_text_c(&font_small, GFX_W / 2, 300, wup_net_ip_str(), RGB(140, 155, 190));
    gfx_text_c(&font_small, GFX_W / 2, 340, status_msg[0] ? status_msg : "press B to cancel", RGB(120, 135, 170));
    gfx_present_all();
}

static const char *const ingame_labels[] = { "Resume", "Disconnect", "Reconnect", "Controls", "Exit" };

static void draw_ingame(void) {
    wup_screen();
    gfx_vgrad24(0, 0, GFX_W, GFX_H, C24(10, 16, 34), C24(4, 6, 16));
    gfx_text_c(&font_title, GFX_W / 2, 92, "WUP ThinClient", RGB(120, 200, 255));
    if (ingame_sub) {
        gfx_text_c(&font_body, GFX_W / 2, 140, "Controls (set on the server)", RGB(190, 205, 230));
        static const char *const rows[] = {
            "Touch   pointer / tap / drag / scroll",
            "Left stick   mouse move",
            "Right stick  scroll",
            "ZL / L3   left mouse     ZR / R3   right mouse",
            "A Enter   B Esc   X keyboard   Y Alt+Tab",
            "L / R   workspace prev/next   D-pad   arrows",
            "Plus / Minus   zoom   TV   fullscreen",
            "HOME   this menu   HOME+B   quit app",
        };
        for (int i = 0; i < 8; i++)
            gfx_text_c(&font_small, GFX_W / 2, 190 + i * 30, rows[i], RGB(170, 185, 215));
        gfx_text_c(&font_small, GFX_W / 2, 460, "B: back", RGB(120, 135, 170));
    } else {
        draw_server_line();
        mi_n = 0;
        for (int i = 0; i < 5; i++)
            menu_item(mi_n++, 170 + i * 48, 40, ingame_labels[i], menu_sel == i);
        gfx_text_c(&font_small, GFX_W / 2, 460, "stream paused - remote input suspended", RGB(120, 135, 170));
    }
    gfx_present_all();
}

static void start_connecting(u32 target, const char *msg) {
    conn_target = target;
    conn_deadline = clock_ms() + 15000;
    state = ST_CONNECTING;
    snprintf(status_msg, sizeof status_msg, "%s", msg ? msg : "");
    draw_connecting();
}

static void menu_activate(int sel) {
    switch (sel) {
    case 0:  /* Connect */
        if (!net_ok) { snprintf(status_msg, sizeof status_msg, "No Wi-Fi connection"); draw_menu(); return; }
        conn_target = server_ip[0] ? inet_addr(server_ip) : 0;
        start_connecting(conn_target, "press B to cancel");
        return;
    case 1:  /* Edit server address */
        edit_from_saved();
        state = ST_EDIT;
        draw_edit();
        return;
    case 2:  /* Auto-connect toggle */
        auto_connect = !auto_connect;
        cfg_save();
        status_msg[0] = 0;
        draw_menu();
        return;
    case 3:  /* Exit */
        wup_log("thinclient: exit from menu");
        state = ST_MENU;
        quit_app = 1;
        return;
    }
}

static void draw_edit(void) {
    wup_screen();
    draw_brand(120, "Server address (dotted quad)");
    char line[48];
    if (edit_any) {
        gfx_text_c(&font_big, GFX_W / 2, 260, "*.*.*.*", RGB(120, 220, 140));
    } else {
        snprintf(line, sizeof line, "%3d . %3d . %3d . %3d",
                 edit_val[0], edit_val[1], edit_val[2], edit_val[3]);
        gfx_text_c(&font_big, GFX_W / 2, 260, line, RGB(255, 255, 255));
    }
    gfx_text_c(&font_body, GFX_W / 2, 340,
               edit_any ? "X: fixed address" : "X: any server",
               RGB(150, 165, 200));
    gfx_text_c(&font_body, GFX_W / 2, 400,
               "Left/Right: octet   Up/Down: +/-1   L/R: +/-10   A: next   B: save & back", RGB(150, 165, 200));
    gfx_present_all();
}

static void edit_from_saved(void) {
    memset(edit_val, 0, sizeof edit_val);
    edit_any = !server_ip[0];
    edit_field = 0;
    if (!edit_any) {
        int a = 0, b = 0, c = 0, d = 0;
        if (sscanf(server_ip, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
            edit_val[0] = a > 255 ? 255 : a < 0 ? 0 : a;
            edit_val[1] = b > 255 ? 255 : b < 0 ? 0 : b;
            edit_val[2] = c > 255 ? 255 : c < 0 ? 0 : c;
            edit_val[3] = d > 255 ? 255 : d < 0 ? 0 : d;
        } else {
            edit_any = 1;
        }
    }
}

static void edit_save(void) {
    if (edit_any) {
        server_ip[0] = 0;
    } else {
        snprintf(server_ip, sizeof server_ip, "%d.%d.%d.%d",
                 edit_val[0], edit_val[1], edit_val[2], edit_val[3]);
    }
    cfg_save();
}

static void end_session(void) {
    diag_event(40, wp, von);
    if (von) stop_engine();
    VDEC(0x0c) = 0;
    motor(0, 0); mic(0);
    light_now = 0;
    if (light_override) wup_backlight(wup_backlight_level());
    light_off = 0; light_level = 0; light_override = 0;
    peer_on = von = sync = npend = 0; need_idr = 1; wp = 0;
    diag_event(41, 0, 0);
    diag_active = 0;
    int was = shown;
    if (was) { gfx_brightness(0); LCD(0x80) = lcd80; shown = 0; }
    state = next_state;
    menu_sel = ingame_sub = 0;
    wup_log("thinclient: session ended");
    if (state == ST_CONNECTING) draw_connecting();
    else draw_menu();
    if (was) gfx_fade(0, 256);
}
static void packet(const u8 *d, u32 len, const struct sockaddr_in *from, u32 now) {
    u8 h[16] __attribute__((aligned(4)));
    u32 n = len < 16 ? len : 16;
    memcpy(h, d, n);
    if (n < 4 || (int)(now - quiet_until) < 0) return;
    if (!peer_on && h[0] == 'H' && state == ST_CONNECTING) {
        /* WUP ThinClient: only the configured server may start a session.
           The explicit IP is a selection filter, not authentication. */
        if (!ip_matches(from)) return;
        peer_sa = *from; peer_on = 1; need_idr = 1; frames = keys = lost = resent = stalls = full = gap_max = qmax = 0; stat_ms = t_start = now;
        last_peer_ip = from->sin_addr.s_addr;
        state = ST_STREAMING;
        rbase = VDEC(0x24) & 0xffffff; rsize = (VDEC(0x28) & 0xffffff) - rbase; wp = 0;
        diag_active = 1;
        diag_event(42, rbase, rsize);
        wup_log("thinclient: stream started");
    }
    if (!peer_on || from->sin_addr.s_addr != peer_sa.sin_addr.s_addr || from->sin_port != peer_sa.sin_port) return;
    last_ms = now;
    if (h[0] == 'H') { mic(h[2] & 1); light(h[3]); }      /* (hello: byte 2 bit 0 = the microphone, byte 3 = the backlight) */
    if (h[0] == 'V' && n == 16 && state != ST_INGAME) video(d + 16, len, h, now);
    else if (h[0] == 'A' && len > 8) audio(d + 8, len);
    else if (h[0] == 'R') motor(h[1] & 1, now);
    else if (h[0] == 'Q') {
        /* the server ended the session: reconnect when auto-connect is on */
        if (auto_connect) {
            next_state = ST_CONNECTING;
            conn_target = last_peer_ip;
            snprintf(status_msg, sizeof status_msg, "Server ended - reconnecting");
        } else {
            next_state = ST_MENU;
            snprintf(status_msg, sizeof status_msg, "Server ended the session");
        }
        end_session();
        quiet_until = now + 1000;
        conn_deadline = now + 15000;
    }
}
static void peer_lost(u32 now) {
    if (auto_connect) {
        next_state = ST_CONNECTING;
        conn_target = last_peer_ip;
        snprintf(status_msg, sizeof status_msg, "Connection lost - reconnecting");
    } else {
        next_state = ST_MENU;
        snprintf(status_msg, sizeof status_msg, "Connection to server lost");
    }
    end_session();
    quiet_until = now + 1000;
    conn_deadline = now + 15000;
}

static void tick(u32 now) {                               /* every 1-2 ms */
    if (now - poll_ms > gap_max && poll_ms) gap_max = now - poll_ms;
    poll_ms = now;
    if (peer_on && now - last_ms > 1500) peer_lost(now);
    if (!peer_on) {
        /* beacons only while a connection is actually being attempted:
           the disconnected menu must not broadcast readiness */
        if (state == ST_CONNECTING && now - beacon_ms >= 1000) {
            static const u8 b[4] = { 'B', 1 };
            struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(50001), .sin_addr.s_addr = 0xffffffffu };
            send_to(&to, b, 4); beacon_ms = now;
        }
        return;
    }
    if (sync && (int)(seen_f - next_f) >= 0 && as[0].t_head) {
        if (now - as[0].t_head > 45 && (as[0].known ? as[0].have != as[0].npk : seen_f != next_f)) { lost++; need_idr = 1; sync = 0; }   /* given up */
        else nack(now);
    }
    watchdog(now);
    if (motor_on && now - motor_ms > 100) motor(0, now);
    if (mic_on) mic_poll();
    if (now - stat_ms >= 5000) {                             /* every 5 s: what the stream did (tools/padlog.py) */
        static const char *const name[] = { "frames ", " keyframes ", " lost ", " resent ", " stalls ", " full ", " gap-ms ", " queue-kb " };
        u32 v[] = { frames, keys, lost, resent, stalls, full, gap_max, qmax };
        char l[200], *e = l;
        for (int i = 0; i < 8; i++) {
            for (const char *q = name[i]; *q; q++) *e++ = *q;
            char t[12]; int k = 0; u32 x = v[i];
            do t[k++] = (char)('0' + x % 10); while (x /= 10);
            while (k) *e++ = t[--k];
        }
        *e = 0; wup_log(l); stat_ms = now; gap_max = qmax = 0;
    }
    u16 seq = (u16)(REPORT[0] | REPORT[1] << 8);
    if (seq != in_seq || now - in_ms > 50) {
        u8 m[8 + 128] = { 'I', (u8)(need_idr | (von ? waiting() << 1 : 0)) };
        u32 since = CLOCK - LCD(0xfc);                       /* ticks since the screen's last refresh (it latches the clock) */
        m[2] = (u8)since; m[3] = (u8)(since >> 8); m[4] = (u8)(light_off ? 0 : level_now());
        const volatile u8 *rep = REPORT;
        u8 neutral[128];
        if (state == ST_INGAME) {                           /* local menu: release everything remote */
            for (int i = 0; i < 128; i++) neutral[i] = REPORT[i];
            memset(neutral + 2, 0, 2);                      /* buttons */
            neutral[0x50] = 0;                              /* L3 / R3 / TV clicks */
            for (int i = 0; i < 4; i++) {                   /* sticks back to rest (0x800) */
                neutral[6 + 2 * i] = 0x00; neutral[7 + 2 * i] = 0x08;
            }
            memset(neutral + 15, 0, 12);                    /* accel + gyro (gyro drives a stick) */
            memset(neutral + 36, 0, 40);                    /* touch samples: all invalid */
            rep = neutral;
        }
        for (int i = 0; i < 128; i++) m[8 + i] = rep[i];
        unsigned buttons = ((unsigned)m[10] << 8) | m[11];
        if (buttons & WUP_HOME) buttons &= ~(WUP_HOME | WUP_UP | WUP_DOWN);   /* HOME is local: never remote */
        m[10] = (u8)(buttons >> 8); m[11] = (u8)buttons;
        send_peer(m, sizeof m); in_seq = seq; in_ms = now;
    }
}

static int menu_hit(int x, int y) {
    for (int i = 0; i < mi_n; i++)
        if (x >= GFX_W / 2 - 210 && x <= GFX_W / 2 + 210 && y >= mi_y[i] && y < mi_y[i] + mi_h[i])
            return i;
    return -1;
}

static void enter_ingame(void) {
    state = ST_INGAME; menu_sel = 0; ingame_sub = 0;
    if (von) stop_engine();
    VDEC(0x0c) = 0;
    von = 0; need_idr = 1; npend = 0; sync = 0;      /* decoder idle; resync on resume */
    in_ms = 0;                                    /* neutral report out within a tick */
    if (shown) { gfx_brightness(0); LCD(0x80) = lcd80; }   /* UI layer back over the frozen frame */
    draw_ingame();
    gfx_fade(0, 256);
}

static void resume_stream(void) {
    state = ST_STREAMING;
    need_idr = 1;                                 /* next 'I' asks for a keyframe: decoder restarts */
    if (shown) LCD(0x80) = (lcd80 | 4) & ~0x10;  /* video scan path back: last frame until new ones */
}

static void ingame_activate(int sel) {
    switch (sel) {
    case 0: resume_stream(); break;
    case 1: next_state = ST_MENU; snprintf(status_msg, sizeof status_msg, "Disconnected");
            end_session(); break;
    case 2: next_state = ST_CONNECTING; conn_target = last_peer_ip;
            snprintf(status_msg, sizeof status_msg, "Reconnecting ...");
            conn_deadline = clock_ms() + 15000;
            end_session(); break;
    case 3: ingame_sub = 1; draw_ingame(); break;
    case 4: quit_app = 1; break;
    }
}

int main(void) {
    last_clk = CLOCK;
    cfg_load();
    net_ok = wup_net_init() == 0;
    if (net_ok) {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(50000), .sin_addr.s_addr = INADDR_ANY };
        if (sock < 0 || bind(sock, (struct sockaddr *)&me, sizeof me)) net_ok = 0;
    }
    wup_audio_open();
    int diag_thread = -1;
    if (net_ok) {
        int one = 1;
        diag_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (diag_sock >= 0 && !setsockopt(diag_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one)) {
            diag_run = 1;
            diag_thread = wup_thread_start(diag_worker, NULL, 2048, 1);
        }
    }
    /* start in the connection menu; auto-connect skips straight to dialing */
    menu_sel = 0;
    if (net_ok && auto_connect) {
        state = ST_CONNECTING;
        conn_target = server_ip[0] ? inet_addr(server_ip) : 0;
        conn_deadline = clock_ms() + 15000;
        status_msg[0] = 0;
        draw_connecting();
    } else {
        state = ST_MENU;
        status_msg[0] = 0;
        if (!net_ok) snprintf(status_msg, sizeof status_msg, "No Wi-Fi - use the Homebrew Menu Settings");
        draw_menu();
    }
    unsigned held = wup_buttons();
    int touch_prev = 0;
    u32 quit_combo_since = 0;
    int quit_combo_active = 0;
    static u8 buf[1536] __attribute__((aligned(4)));
    for (;;) {
        ++diag_progress;
        diag_stage = 1;
        wup_sleep(peer_on ? 1 : 16);                         /* (the firmware's tasks run in here; the network in its own threads) */
        u32 now = clock_ms();
        if (net_ok) {
            for (;;) {
                struct sockaddr_in from; socklen_t fl = sizeof from;
                diag_stage = 2;
                int n = (int)recvfrom(sock, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
                if (n <= 0) break;
                ++diag_progress;
                diag_stage = 3;
                packet(buf, (u32)n, &from, clock_ms());
            }
            diag_stage = 4;
            tick(clock_ms());
        }
        unsigned b = wup_buttons(), pressed = b & ~held;
        int tx = 0, ty = 0, tnow = wup_touch(&tx, &ty);
        int tpress = tnow && !touch_prev;
        touch_prev = tnow;

        /* Local emergency exit: HOME + B continuously for 2 seconds.
           Never intercept either key alone; reset timer when either is released.
           u32 subtraction handles clock_ms() wraparound. */
        if ((b & (WUP_HOME | WUP_B)) == (WUP_HOME | WUP_B)) {
            if (!quit_combo_active) {
                quit_combo_active = 1;
                quit_combo_since = now;
            } else if ((u32)(now - quit_combo_since) >= 2000u) {
                wup_log("thinclient: quit via HOME+B");
                quit_app = 1;
            }
        } else {
            quit_combo_active = 0;
        }
        if (quit_app) break;

        /* connect attempt timed out: back to the menu */
        if (state == ST_CONNECTING && !peer_on && (int)(now - conn_deadline) >= 0) {
            state = ST_MENU; menu_sel = 0;
            snprintf(status_msg, sizeof status_msg, "No answer from server");
            draw_menu();
        }

        /* ---- local UI ---------------------------------------------------- */
        if (state == ST_EDIT) {
            if (pressed & WUP_B) { edit_save(); state = ST_MENU; menu_sel = 0; status_msg[0] = 0; draw_menu(); }
            else if (pressed & WUP_X) { edit_any = !edit_any; draw_edit(); }
            else if (pressed & WUP_LEFT)  { if (edit_field > 0) edit_field--; draw_edit(); }
            else if (pressed & WUP_RIGHT) { if (edit_field < 3) edit_field++; draw_edit(); }
            else if (pressed & WUP_A)     { if (edit_field < 3) edit_field++; else edit_field = 0; draw_edit(); }
            else if (pressed & WUP_UP || pressed & WUP_DOWN) {
                if (!edit_any) {
                    int d = (pressed & WUP_UP) ? 1 : -1;
                    /* L/R are checked as HELD buttons: they modify the
                       up/down step even when pressed before the d-pad. */
                    if (b & (WUP_L | WUP_R)) d *= 10;
                    edit_val[edit_field] += d;
                    if (edit_val[edit_field] > 255) edit_val[edit_field] = 255;
                    if (edit_val[edit_field] < 0) edit_val[edit_field] = 0;
                    draw_edit();
                }
            }
            held = b;
            continue;
        }
        if (state == ST_MENU) {
            int hit = tpress ? menu_hit(tx, ty) : -1;
            if (hit >= 0) { menu_sel = hit; menu_activate(hit); }
            else if (pressed & WUP_UP)    { menu_sel = (menu_sel + 3) % 4; draw_menu(); }
            else if (pressed & WUP_DOWN)  { menu_sel = (menu_sel + 1) % 4; draw_menu(); }
            else if (pressed & WUP_A)     { menu_activate(menu_sel); }
            else if (pressed & WUP_B)     { quit_app = 1; }
        } else if (state == ST_CONNECTING) {
            if (pressed & WUP_B) {
                state = ST_MENU; menu_sel = 0;
                snprintf(status_msg, sizeof status_msg, "Cancelled");
                draw_menu();
            }
        } else if (state == ST_STREAMING) {
            if (pressed & WUP_HOME) enter_ingame();
        } else if (state == ST_INGAME) {
            if (ingame_sub) {
                if (pressed & WUP_B) { ingame_sub = 0; draw_ingame(); }
            } else {
                int hit = tpress ? menu_hit(tx, ty) : -1;
                if (hit >= 0) { menu_sel = hit; ingame_activate(hit); }
                else if (pressed & WUP_UP)   { menu_sel = (menu_sel + 4) % 5; draw_ingame(); }
                else if (pressed & WUP_DOWN) { menu_sel = (menu_sel + 1) % 5; draw_ingame(); }
                else if (pressed & WUP_A)    { ingame_activate(menu_sel); }
                else if (pressed & WUP_B)    { resume_stream(); }
            }
        }
        held = b;
    }
    if (peer_on) end_session();
    diag_run = 0;
    if (diag_thread >= 0 && wup_thread_join(diag_thread, 1000)) wup_thread_kill(diag_thread);
    return 0;
}
