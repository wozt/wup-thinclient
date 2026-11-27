/* WUP ThinClient: a WUP ThinServer desktop on the Wii U GamePad.
 *
 * This is the WUP Stream pad receiver (wup-stream/pad/main.c, upstream
 * e1b0430) adapted into a standalone homebrew with its own identity and a
 * local connection menu: manual server IP entry, saved configuration and a
 * disconnect action. The wire protocol is unchanged — see
 * wup-thinserver/docs/PROTOCOL.md.
 *
 * Protocol (UDP, little endian). The pad listens on port 50000.
 *   pad -> broadcast:50002  'B' version 1 0                      every second while no server is connected
 *     (direct endpoints: unicast 'B' to host:port instead)
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
 * Video is each frame's raw CABAC slice data, put together here from its packets and handed whole to the hardware
 * decoder (libwup's wup_video_*: the kernel's driver). While video plays the screen's memory is the decoder's: the
 * menu is drawn again from scratch when a stream ends or pauses.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "wup.h"
#include "armcpu.h"
#include "wup_hw.h"                                   /* (hw_backlight_percent: the table's levels) */
#include "fs.h"                                       /* saved connection profiles */
#include "ip_editor.h"
#include "profiles.h"                                /* saved connection profiles */
#include "home_keys.h"                               /* HOME shortcut state machine */

WUP_APP("WUP ThinClient");
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define REG(a)  (*(volatile u32 *)(a))
#define CLOCK   REG(0xf0000408u)                              /* free-running, ~997.3 kHz */

static u32 ms_, last_clk, frac;
static u32 clock_ms(void) { u32 c = CLOCK; frac += c - last_clk; last_clk = c; ms_ += frac / 997; frac %= 997; return ms_; }

/* ---- saved connection profiles ---------------------------------------------
   /save/thinclient.cfg holds a versioned profile list (profiles.c): up to
   PROF_MAX named endpoints, each a DNS name or dotted quad plus a UDP port.
   Legacy single-endpoint files are migrated on first load. The active
   endpoint globals below are only set while a connection is attempted;
   browsing and editing never touch the network. */
#define CFG_PATH "/save/thinclient.cfg"
static profiles_t profs;
static int cfg_dirty;

/* active endpoint (set when Connect / auto-connect / reconnect fires) */
static char server_host[PROF_HOST_MAX];
static int server_port = 50002;
static u32 server_addr;             /* resolved IPv4 for direct mode, 0 = none */
static int auto_connect;            /* auto-reconnect after a lost stream */

static void cfg_load(void) {
    prof_init(&profs);
    cfg_dirty = 0;
    server_host[0] = 0;
    server_port = 50002;
    auto_connect = 0;
    server_addr = 0;
    if (!wup_fs_ok()) return;
    static char buf[3400];
    memset(buf, 0, sizeof buf);
    int n = fs_read(CFG_PATH, 0, buf, (u32)sizeof buf - 1);
    if (n <= 0) return;
    buf[sizeof buf - 1] = 0;
    prof_parse(&profs, buf, (size_t)n);
    /* a migrated or stored discovery port, if present */
    char *dp = strstr(buf, "\ndport=");
    if (dp) {
        int v = atoi(dp + 7);                  /* skip '\n' + "dport=" */
        if (v >= 1 && v <= 65535) profs.disc_port = v;
    }
    /* migrate the old shared-discovery default: ThinClient/ThinServer now
       use 50002 so WUP Stream's 50001 beacons are never answered by them */
    if (profs.disc_port == 50001) profs.disc_port = 50002;
}

static void cfg_save(void) {
    if (!wup_fs_ok() || !cfg_dirty) return;
    static char buf[3400];
    int n = prof_serialize(&profs, buf, sizeof buf);
    if (n <= 0) return;
    /* append the LAN discovery port line (kept out of profiles.c: it is a
       property of the discovery action, not of any profile) */
    size_t off = (size_t)n;
    if (off + 24 < sizeof buf) {
        int w = snprintf(buf + off, sizeof buf - off, "dport=%d\n", profs.disc_port);
        if (w > 0) off += (size_t)w < sizeof buf - off ? (size_t)w : sizeof buf - off - 1;
        buf[off < sizeof buf ? off : sizeof buf - 1] = 0;
    }
    if (fs_write(CFG_PATH, buf, (u32)strlen(buf) + 1) >= 0) cfg_dirty = 0;
}

static void cfg_touch(void) { cfg_dirty = 1; }

/* Resolve the configured endpoint once per connect attempt. Numeric IPv4
   literals resolve locally; names go through lwIP DNS (blocking, bounded
   by the resolver's own timeouts). Returns 1 when server_addr is usable
   or when LAN discovery mode is selected. */
static int resolve_server(void) {
    server_addr = 0;
    if (!server_host[0]) return 1;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port[6];
    snprintf(port, sizeof port, "%d", server_port);
    if (getaddrinfo(server_host, port, &hints, &res) != 0 || !res) return 0;
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    server_addr = sa->sin_addr.s_addr;
    freeaddrinfo(res);
    return server_addr != 0;
}

static u32 conn_target;                          /* forward: used by ip_matches */
static int ip_matches(const struct sockaddr_in *from) {
    if (conn_target) return from->sin_addr.s_addr == conn_target;
    if (!server_host[0]) return 1;                    /* any server */
    return server_addr && from->sin_addr.s_addr == server_addr;
}

/* ---- local UI state --------------------------------------------------------- */
enum { ST_MENU, ST_PE, ST_EDIT, ST_KBD, ST_DELCONF,
       ST_CONNECTING, ST_STREAMING, ST_INGAME };
static int state = ST_MENU;
static int menu_sel;                             /* menu row / ingame row */
static int menu_prof = -1;                       /* profile the menu actions act on */
static int ingame_sub;                           /* 0 = actions, 1 = controls help */
static char status_msg[80];                      /* menu status line */
static u32 conn_deadline;                        /* connect attempt timeout */
static int next_state = ST_MENU;                 /* where end_session() lands */
static u32 last_peer_ip;                         /* last server that streamed */
static int quit_app;                             /* menu Exit / emergency quit */
static int pending_resolve;                      /* re-resolve endpoint on next loop */
static char edit_ip[PROF_HOST_MAX];             /* numeric keypad buffer (IPv4/port) */
static int edit_sel;                             /* optional D-pad keypad selection */
static int edit_touch = -1, edit_hover = -1;     /* touch-down key and current key */
static int edit_mode;                            /* 0 = IPv4 host, 1 = UDP port */

/* profile editor (ST_PE) + alphanumeric keyboard (ST_KBD) + delete confirm */
static prof_t pe_prof;                           /* draft under edit */
static char pe_port[8];                          /* port digits while editing */
static int pe_idx = -1;                          /* profile index, -1 = new */
static int pe_sel;                               /* focused field/button */
static int pe_err;                               /* 0 none, 1 name, 2 host, 3 port */
static char pe_err_msg[72];
static int del_idx = -1;                         /* profile pending delete confirm */
static char kbd_buf[PROF_HOST_MAX];              /* keyboard edit buffer */
static int kbd_len, kbd_target;                  /* 0 name, 1 host */

static void draw_pe(void);
static void draw_kbd(void);
static void draw_delconf(void);
static void kbd_open(void);
static void pe_save(void);

/* ---- HOME shortcuts + live input-mode switching ('T' packets) -------------- */
static home_keys_t hk;
static u8 remote_mode = 0xff;                    /* 'H' byte 4: 0 gamepad, 1 desktop, 0xff unknown */
static u8 mode_reqid;                            /* 'T' request sequence (0 = unused) */
static u32 mode_req_deadline;                    /* ack timeout */
static int mode_req_pending;
static char mode_notice[96];                     /* last mode-switch result, shown in menus */
static u32 mode_notice_ms;

/* ---- the server ------------------------------------------------------------- */
static int net_ok;
static int sock = -1;
static struct sockaddr_in peer_sa;
static u32 peer_on, last_ms, quiet_until, beacon_ms, in_ms, stat_ms;
static u16 in_seq;
static void send_to(const struct sockaddr_in *to, const void *d, u32 n) { sendto(sock, d, n, 0, (const struct sockaddr *)to, sizeof *to); }
static void send_peer(const void *d, u32 n) { if (peer_on) send_to(&peer_sa, d, n); }

/* ---- video: whole frames to the hardware decoder (wup_video_frame: the kernel's driver owns the engine, the
   bitstream ring and the screen's video layer). Every keyframe restarts it; after a lost or late frame only a
   keyframe goes on (wup_video_need_key); the kernel watches it for stalls. The screen stays dark until the decoder
   has taken 3 frames, then lights up. */
static u32 von, need_idr = 1, frames, shown, lit_at, lit_ms;
static u32 keys, lost, stalls, gap_max, poll_ms, full, qmax, t_start;   /* (counters, logged every 5 s) */
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

static int frame_done(const u8 *data, u32 len, int idr, u32 now) {   /* 0, or < 0: dropped */
    if (!idr && (need_idr || wup_video_need_key())) { need_idr = 1; return -1; }
    if (!shown) { gfx_brightness(0); if (wup_video_open()) return -1; shown = 1; lit_at = 0; lit_ms = now; }
    if (wup_video_frame(data, len, idr)) { need_idr = 1; if (von) stalls++; von = 0; diag_event(31, len, -1); return -1; }
    diag_event(30, len, idr);
    von = 1; need_idr = 0; frames++; if (idr) keys++;
    if (shown == 1 && ++lit_at == 3) { shown = 2; lit_ms = now; }
    return 0;
}
static u32 waiting(void) { return von ? (u32)wup_video_waiting() : 0; }
/* Frames come in packets of 1400 bytes, and Wi-Fi loses some. Up to 4 frames are put together at once, each in its
   own buffer, and go to the decoder in order as they become whole. What is missing is asked for again ('N'): when
   later packets show the gap, or nothing came for 6 ms. A frame still not whole after 45 ms is given up: a keyframe
   then. */
#define NASM 4
#define FRAME_MAX (128 * 1400u)                               /* (a frame of up to 128 packets) */
static struct { u32 len, npk, have, got[4], t_rx, t_nack, t_head; u8 known, idr; u8 *data; } as[NASM];
static u32 next_f, seen_f, sync, resent;
static void nack(u32 now);
static void as_clear(int i) { u8 *d = as[i].data; memset(&as[i], 0, sizeof as[i]); as[i].data = d; }
static void as_reset(u32 f) { for (int i = 0; i < NASM; i++) as_clear(i); next_f = seen_f = f; sync = 1; }
static void video(const u8 *d16, u32 len, const u8 *h, u32 now) {      /* d16: the data after the 16-byte header */
    u32 fno = *(u32 *)(h + 4), off = *(u32 *)(h + 8), total = *(u32 *)(h + 12), n = len - 16u, idr = h[1] & 1;
    if (idr && (!sync || fno != next_f || !as[0].idr)) {       /* a keyframe: the decoder starts again with it */
        if (sync && (int)(fno - next_f) < 0) return;           /* (an old one, again) */
        diag_event(20, fno, seen_f);
        as_reset(fno); as[0].idr = 1; as[0].t_head = now;
    }
    if (!sync || (need_idr && !as[0].idr)) return;             /* waiting for a keyframe */
    int d = (int)(fno - next_f);
    if (d < 0) return;                                         /* (already whole: a packet sent twice) */
    if (d >= NASM) { lost++; need_idr = 1; sync = 0; return; } /* too far ahead: we lost the thread */
    if ((int)(fno - seen_f) > 0) seen_f = fno;
    if (!as[d].known) {
        if (!total || total > FRAME_MAX) { full++; need_idr = 1; sync = 0; return; }
        as[d].known = 1; as[d].len = total; as[d].npk = (total + 1399) / 1400; as[d].have = 0; as[d].idr = (u8)idr;
        memset(as[d].got, 0, sizeof as[d].got);
    }
    u32 k = off / 1400;
    if (off % 1400 || k >= as[d].npk || off + n > as[d].len || (n != 1400 && off + n != as[d].len)) return;
    if (!(as[d].got[k / 32] >> (k % 32) & 1)) {
        diag_stage = 21;
        memcpy(as[d].data + off, d16, n);
        diag_stage = 22;
        as[d].got[k / 32] |= 1u << (k % 32); as[d].have++;
    }
    as[d].t_rx = now;
    if (d > 0 && as[d - 1].have != as[d - 1].npk) nack(now);   /* a later frame has begun: what the earlier lacks is lost */
    while (as[0].known && as[0].have == as[0].npk) {           /* whole frames, in order, to the decoder */
        diag_event(30, next_f, as[0].len);
        if (frame_done(as[0].data, as[0].len, as[0].idr, now) && !as[0].idr) { sync = 0; return; }
        diag_stage = 31;
        u8 *d0 = as[0].data;
        for (int i = 0; i < NASM - 1; i++) as[i] = as[i + 1];
        as[NASM - 1].data = d0; as_clear(NASM - 1);
        next_f++; as[0].t_head = now;
    }
}
/* the screen lit once the decoder has had its first 3 frames a moment (its stalls: the kernel's) */
static void watchdog(u32 now) {
    diag_stage = 50;
    if (shown == 2 && now - lit_ms > 50) { gfx_brightness(256); shown = 3; }
    u32 w = waiting(); if (w > qmax) qmax = w;
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

/* ---- 'T' input-mode requests (HOME+X) ---------------------------------------
   mode byte: 0 gamepad, 1 desktop (the server's INMODE_* values), 0xff toggle.
   The ack carries the EFFECTIVE mode; remote_mode tracks the 'H' byte 4
   hint so repeated presses always request the opposite of what runs. */
static void mode_request(u32 now) {
    u8 want = remote_mode == 0xff ? 0xff : (u8)(remote_mode == 1 ? 0 : 1);
    mode_reqid++;
    if (!mode_reqid) mode_reqid++;
    u8 t[4] = { 'T', mode_reqid, want, 0 };
    send_peer(t, sizeof t);
    mode_req_pending = 1;
    mode_req_deadline = now + 2500;
    snprintf(mode_notice, sizeof mode_notice, "Requesting %s mode...",
             want == 0xff ? "other input" : want == 0 ? "Gamepad" : "Desktop");
    mode_notice_ms = now;
    wup_log("thinclient: input mode request sent");
}
static void mode_timeout(u32 now) {
    if (!mode_req_pending || (int)(now - mode_req_deadline) < 0) return;
    mode_req_pending = 0;
    snprintf(mode_notice, sizeof mode_notice,
             "Mode switch unanswered (old server?)");
    mode_notice_ms = now;
    wup_log("thinclient: mode request timed out");
}

/* ---- local screens ------------------------------------------------------------ */
static void draw_brand(int y, const char *sub) {
    gfx_vgrad24(0, 0, GFX_W, GFX_H, C24(16, 32, 64), C24(5, 8, 20));
    gfx_text_c(&font_big, GFX_W / 2, y, "WUP ThinClient", RGB(120, 200, 255));
    if (sub) gfx_text_c(&font_body, GFX_W / 2, y + 44, sub, RGB(190, 200, 225));
}

/* ---- menu geometry (shared by drawing and touch hit-testing) ----------------- */
static void draw_edit(void);
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
    char line[96];
    if (server_host[0]) {
        size_t l = strlen(server_host);
        const char *h = server_host;
        if (l > 60) { h = server_host + l - 60; }   /* keep the tail: port stays visible */
        snprintf(line, sizeof line, "Server: %s:%d", h, server_port);
    } else {
        snprintf(line, sizeof line, "Server: any (first found)");
    }
    gfx_text_c(&font_body, GFX_W / 2, 168, line, RGB(255, 255, 255));
}

/* ---- profile list geometry -------------------------------------------------
   Rows: up to PROF_MAX profiles, then 7 action buttons in two rows.
   Item indices: 0..n-1 profiles, then ACT_* offsets. */
enum { ACT_CONNECT, ACT_ADD, ACT_EDIT, ACT_DEL, ACT_LAN, ACT_AUTO, ACT_EXIT, ACT_COUNT };
#define PROF_ROW_Y 162
#define PROF_ROW_H 27
static int pm_y[PROF_MAX + ACT_COUNT], pm_h[PROF_MAX + ACT_COUNT], pm_n;
static int pm_is_act[PROF_MAX + ACT_COUNT];

static void pm_item(int i, int y, int h, int is_act) {
    pm_y[i] = y; pm_h[i] = h; pm_is_act[i] = is_act;
}

static void draw_profile_row(int i, int sel, int cur) {
    const prof_t *p = &profs.p[i];
    int y = PROF_ROW_Y + i * PROF_ROW_H;
    if (sel) {
        gfx_rrect(20, y, GFX_W - 40, PROF_ROW_H - 3, 6, RGB(46, 96, 168), 220);
        gfx_rrect_outline(20, y, GFX_W - 40, PROF_ROW_H - 3, 6, 1, RGB(120, 190, 255));
    } else if (cur) {
        gfx_rrect(20, y, GFX_W - 40, PROF_ROW_H - 3, 6, RGB(30, 42, 66), 200);
    }
    gfx_text(&font_body, 36, y + 20, p->name,
             sel ? RGB(255, 255, 255) : RGB(215, 225, 245));
    /* endpoint right-aligned; keep the tail so ":port" is always visible */
    char ep[72];
    char full[PROF_HOST_MAX + 8];
    snprintf(full, sizeof full, "%s:%u", p->host, (unsigned)p->port);
    size_t fl = strlen(full);
    if (fl >= sizeof ep) {
        memcpy(ep, full + fl - sizeof ep + 1, sizeof ep - 1);
        ep[0] = '<'; ep[sizeof ep - 1] = 0;
    } else strcpy(ep, full);
    int w = gfx_text_w(&font_small, ep);
    gfx_text(&font_small, GFX_W - 36 - w, y + 20, ep,
             sel ? RGB(190, 220, 255) : RGB(140, 155, 190));
    if (p->auto_connect)
        gfx_text(&font_small, GFX_W - 36 - w - 46, y + 20, "AUTO",
                 RGB(255, 210, 120));
}

static void draw_act_button(int x, int y, int w, int h, const char *label,
                            int sel, int enabled) {
    px_t fill = !enabled ? RGB(24, 28, 38) : sel ? RGB(46, 96, 168) : RGB(24, 38, 62);
    px_t text = !enabled ? RGB(95, 105, 125) : RGB(240, 246, 255);
    gfx_rrect(x, y, w, h, 8, fill, 230);
    if (sel && enabled)
        gfx_rrect_outline(x, y, w, h, 8, 2, RGB(130, 205, 255));
    gfx_text_c(&font_body, x + w / 2, y + h / 2 + 7, label, text);
}

static void draw_menu(void) {
    wup_screen();
    draw_brand(46, "Linux desktop on the GamePad");
    gfx_text_c(&font_small, GFX_W / 2, 104, wup_net_ip_str(), RGB(140, 155, 190));
    if (status_msg[0])
        gfx_text_c(&font_body, GFX_W / 2, 124, status_msg, RGB(255, 210, 120));
    gfx_text_c(&font_small, GFX_W / 2, 144, "Connections", RGB(150, 170, 205));

    pm_n = 0;
    for (int i = 0; i < profs.n && i < PROF_MAX; i++) {
        pm_item(pm_n, PROF_ROW_Y + i * PROF_ROW_H, PROF_ROW_H, 0);
        pm_n++;
    }
    if (!profs.n) {
        gfx_text_c(&font_body, GFX_W / 2, 250,
                   "No saved connections", RGB(190, 205, 230));
        gfx_text_c(&font_small, GFX_W / 2, 278,
                   "Add one below, or use LAN discovery", RGB(140, 155, 190));
    } else {
        for (int i = 0; i < profs.n && i < PROF_MAX; i++)
            draw_profile_row(i, menu_sel == i, menu_prof == i && menu_sel >= profs.n);
    }
    /* action rows */
    static const char *const a1[] = { "Connect", "Add", "Edit", "Delete" };
    static const char *const a2[] = { "LAN discovery", NULL, "Exit" };   /* [1] = Auto (dynamic) */
    int ay1 = 388, ay2 = 424;
    for (int i = 0; i < ACT_COUNT; i++) {
        int row = i < 4 ? 0 : 1;
        int col = i < 4 ? i : i - 4;
        int ncol = i < 4 ? 4 : 3;
        int w = (GFX_W - 40 - (ncol - 1) * 10) / ncol;
        int x = 20 + col * (w + 10);
        int y = row ? ay2 : ay1;
        pm_item(pm_n, y, 32, 1); pm_n++;
        const char *label = row ? a2[col] : a1[col];
        char auto_lbl[24];
        int enabled = 1;
        if (row && col == 1) {                            /* Auto-connect toggle */
            int on = menu_prof >= 0 && menu_prof < profs.n
                         ? profs.p[menu_prof].auto_connect : profs.disc_auto;
            snprintf(auto_lbl, sizeof auto_lbl, "Auto: %s", on ? "on" : "off");
            label = auto_lbl;
            enabled = menu_prof >= 0 || profs.n == 0;
        } else if (!row) {
            /* Connect/Add always live; Edit/Delete need a profile; Connect
               additionally needs at least one saved profile */
            enabled = col <= 1 ? 1 : menu_prof >= 0 && menu_prof < profs.n;
            if (col == 0 && profs.n == 0) enabled = 0;
        }
        draw_act_button(x, y, w, 32, label, menu_sel == pm_n - 1, enabled);
    }
    gfx_text_c(&font_small, GFX_W / 2, 474,
               "D-pad + A   touch   B: quit   HOME+B: quit   hold HOME 5 s: menu",
               RGB(120, 135, 170));
    gfx_present_all();
}

/* hit test for the profile list: returns item index or -1 */
static int prof_hit(int x, int y) {
    for (int i = 0; i < pm_n; i++)
        if (y >= pm_y[i] && y < pm_y[i] + pm_h[i]) {
            if (pm_is_act[i]) {
                /* recompute the button rect the same way draw_menu did */
                int ai = i - profs.n;
                int row = ai < 4 ? 0 : 1, col = ai < 4 ? ai : ai - 4;
                int ncol = row ? 3 : 4;
                int w = (GFX_W - 40 - (ncol - 1) * 10) / ncol;
                int bx = 20 + col * (w + 10);
                if (x >= bx && x < bx + w) return i;
            } else if (x >= 20 && x <= GFX_W - 20) {
                return i;
            }
        }
    return -1;
}

static void draw_connecting(void) {
    wup_screen();
    draw_brand(170, NULL);
    char line[96];
    if (server_host[0])
        snprintf(line, sizeof line, "Looking for %s:%d ...", server_host, server_port);
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
            "Left stick   mouse move     Right stick  scroll",
            "ZL / L3   left mouse     ZR / R3   right mouse",
            "A Enter   B Esc   X keyboard   Y Alt+Tab",
            "L / R   workspace prev/next   D-pad   arrows",
            "Plus / Minus   zoom   TV   fullscreen",
            "HOME tap   remote HOME (bindable on server)",
            "HOME 5 s   this menu     HOME+X   Desktop/Gamepad",
            "HOME+Up/Down   screen brightness     HOME+B   quit",
        };
        for (int i = 0; i < 9; i++)
            gfx_text_c(&font_small, GFX_W / 2, 178 + i * 28, rows[i], RGB(170, 185, 215));
        gfx_text_c(&font_small, GFX_W / 2, 460, "B: back", RGB(120, 135, 170));
    } else {
        draw_server_line();
        /* effective input mode + last mode-switch result ('T' ack / 'H' hint) */
        char mline[100];
        const char *mname = remote_mode == 0 ? "Gamepad" : remote_mode == 1 ? "Desktop" : "?";
        u32 now = clock_ms();
        if (mode_notice[0] &&
            (mode_req_pending || (int)(now - mode_notice_ms) < 15000))
            snprintf(mline, sizeof mline, "Mode: %s - %s", mname, mode_notice);
        else
            snprintf(mline, sizeof mline, "Mode: %s", mname);
        gfx_text_c(&font_small, GFX_W / 2, 148, mline, RGB(140, 190, 235));
        mi_n = 0;
        for (int i = 0; i < 5; i++)
            menu_item(mi_n++, 176 + i * 46, 38, ingame_labels[i], menu_sel == i);
        gfx_text_c(&font_small, GFX_W / 2, 460, "stream paused - remote input suspended", RGB(120, 135, 170));
    }
    gfx_present_all();
}

/* Begin a connection attempt against the endpoint already copied into
   server_host/server_port ("" host = LAN discovery on profs.disc_port).
   DNS resolution runs while the connecting screen is up; a failure returns
   to the profile list with an error instead of entering ST_CONNECTING. */
static void begin_connect(void) {
    state = ST_CONNECTING;
    conn_target = 0;
    server_addr = 0;
    conn_deadline = clock_ms() + 30000;   /* covers DNS + beacon window */
    snprintf(status_msg, sizeof status_msg, "press B to cancel");
    draw_connecting();
    if (!resolve_server()) {
        state = ST_MENU;
        snprintf(status_msg, sizeof status_msg, "Cannot resolve %s", server_host);
        draw_menu();
        return;
    }
    conn_deadline = clock_ms() + 15000;
}

static void use_profile_endpoint(int idx) {
    const prof_t *p = &profs.p[idx];
    snprintf(server_host, sizeof server_host, "%s", p->host);
    server_port = p->port;
    auto_connect = p->auto_connect;
}

static void menu_activate(int sel) {
    if (sel < profs.n) {                     /* profile row: select only */
        menu_prof = sel;
        if (profs.sel != sel) { profs.sel = sel; cfg_touch(); }
        draw_menu();
        return;
    }
    int act = sel - profs.n;                 /* ACT_* */
    if (act < 0 || act >= ACT_COUNT) return;
    /* actions that need a profile resolve menu_prof first */
    if ((act == ACT_CONNECT || act == ACT_EDIT || act == ACT_DEL || act == ACT_AUTO) &&
        (menu_prof < 0 || menu_prof >= profs.n)) {
        menu_prof = profs.sel >= 0 && profs.sel < profs.n ? profs.sel
                                                         : (profs.n ? 0 : -1);
    }
    switch (act) {
    case ACT_CONNECT:
        if (menu_prof < 0 || menu_prof >= profs.n) {
            snprintf(status_msg, sizeof status_msg, "No profile selected");
            draw_menu(); return;
        }
        if (!net_ok) { snprintf(status_msg, sizeof status_msg, "No Wi-Fi connection"); draw_menu(); return; }
        profs.sel = menu_prof; cfg_touch();
        use_profile_endpoint(menu_prof);
        begin_connect();
        return;
    case ACT_ADD:
        memset(&pe_prof, 0, sizeof pe_prof);
        pe_prof.type = PROF_TYPE_UDP;
        pe_port[0] = 0;
        snprintf(pe_port, sizeof pe_port, "50002");
        pe_idx = -1; pe_sel = 0; pe_err = 0; pe_err_msg[0] = 0;
        state = ST_PE;
        draw_pe();
        return;
    case ACT_EDIT:
        if (menu_prof < 0 || menu_prof >= profs.n) {
            snprintf(status_msg, sizeof status_msg, "No profile selected");
            draw_menu(); return;
        }
        pe_prof = profs.p[menu_prof];
        pe_idx = menu_prof;
        snprintf(pe_port, sizeof pe_port, "%u", (unsigned)pe_prof.port);
        pe_sel = 0; pe_err = 0; pe_err_msg[0] = 0;
        state = ST_PE;
        draw_pe();
        return;
    case ACT_DEL:
        if (menu_prof < 0 || menu_prof >= profs.n) {
            snprintf(status_msg, sizeof status_msg, "No profile selected");
            draw_menu(); return;
        }
        del_idx = menu_prof;
        pe_sel = 0;
        state = ST_DELCONF;
        draw_delconf();
        return;
    case ACT_LAN:
        if (!net_ok) { snprintf(status_msg, sizeof status_msg, "No Wi-Fi connection"); draw_menu(); return; }
        server_host[0] = 0;
        server_port = profs.disc_port;
        auto_connect = profs.disc_auto;
        begin_connect();
        return;
    case ACT_AUTO:
        if (menu_prof >= 0 && menu_prof < profs.n) {
            profs.p[menu_prof].auto_connect ^= 1;
            cfg_touch();
        } else {
            profs.disc_auto ^= 1;
            cfg_touch();
        }
        cfg_save();
        status_msg[0] = 0;
        draw_menu();
        return;
    case ACT_EXIT:
        wup_log("thinclient: exit from menu");
        cfg_save();
        state = ST_MENU;
        quit_app = 1;
        return;
    }
}

/* ---- numeric keypad (ST_EDIT): IPv4 host entry or UDP port --------------- */
enum { EKEY_BACK, EKEY_CLEAR, EKEY_SAVE, EKEY_CANCEL };
#define EDIT_MAXKEYS 16
static int edit_nkeys, edit_ndig;
static int edit_ret;                              /* state to return to: ST_PE */

static void edit_key_rect(int key, int *x, int *y, int *w, int *h) {
    if (key < edit_ndig) {                         /* digit grid, 3 columns */
        *x = 80 + (key % 3) * 120;
        *y = 168 + (key / 3) * 56;
        *w = 108; *h = 48;
    } else {                                       /* action column */
        *x = 470; *y = 168 + (key - edit_ndig) * 74;
        *w = 304; *h = 66;
    }
}

static int edit_button_at(int x, int y) {
    for (int key = 0; key < edit_nkeys; key++) {
        int bx, by, bw, bh;
        edit_key_rect(key, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) return key;
    }
    return -1;
}

static const char *edit_key_label(int key) {
    static const char *const digs_ip = "123456789.0";
    static const char *const digs_pt = "1234567890";
    static const char *const acts[] = { "Backspace", "Clear", "Save", "Cancel" };
    const char *d = edit_mode ? digs_pt : digs_ip;
    if (key < edit_ndig) {
        static char one[2];
        one[0] = d[key]; one[1] = 0;
        return one;
    }
    return acts[key - edit_ndig];
}

static int edit_valid(void) {
    return edit_mode ? prof_port_valid(edit_ip) : ip_editor_valid(edit_ip);
}

static void draw_edit(void) {
    wup_screen();
    draw_brand(30, edit_mode ? "UDP port (1-65535)" : "IPv4 address for the host field");
    int valid = edit_valid();
    gfx_rrect(80, 88, 694, 46, 9, RGB(18, 28, 48), 230);
    gfx_rrect_outline(80, 88, 694, 46, 9, 2,
                      valid ? RGB(100, 210, 145) : RGB(105, 125, 165));
    gfx_text_c(&font_big, GFX_W / 2, 122, edit_ip[0] ? edit_ip : "_", RGB(255, 255, 255));

    for (int key = 0; key < edit_nkeys; key++) {
        int x, y, w, h, down = key == edit_touch && key == edit_hover;
        int disabled = key == edit_ndig + EKEY_SAVE && !valid;
        edit_key_rect(key, &x, &y, &w, &h);
        px_t fill = disabled ? RGB(36, 42, 54) : down ? RGB(74, 138, 210) :
                    key == edit_sel ? RGB(46, 96, 168) : RGB(24, 38, 62);
        px_t text = disabled ? RGB(95, 105, 125) : RGB(240, 246, 255);
        gfx_rrect(x, y, w, h, 9, fill, 230);
        if (!disabled && (down || key == edit_sel))
            gfx_rrect_outline(x, y, w, h, 9, 2, RGB(130, 205, 255));
        gfx_text_c(key < edit_ndig + EKEY_BACK ? &font_body :
                   key == edit_ndig + EKEY_BACK ? &font_small : &font_body,
                   x + w / 2, y + h / 2 + 7, edit_key_label(key), text);
    }
    gfx_text_c(&font_small, GFX_W / 2, 462,
               valid ? "Touch Save, or use D-pad + A   B: Cancel" :
                       (edit_mode ? "Digits only, 1-65535   B: Cancel"
                                  : "Enter four octets 0-255   B: Cancel"),
               RGB(145, 165, 200));
    gfx_present_all();
}

static void edit_open(int mode, const char *initial, int ret_state) {
    edit_mode = mode;
    edit_ret = ret_state;
    snprintf(edit_ip, sizeof edit_ip, "%s", initial);
    edit_ndig = mode ? 10 : 11;
    edit_nkeys = edit_ndig + 4;
    edit_sel = 0;
    edit_touch = edit_hover = -1;
    state = ST_EDIT;
    draw_edit();
}

static void edit_leave_pe(void) {
    state = ST_PE;
    draw_pe();
}

static void edit_activate(int key) {
    static const char digs_ip[] = "123456789.0";
    static const char digs_pt[] = "1234567890";
    const char *d = edit_mode ? digs_pt : digs_ip;
    if (key >= 0 && key < edit_ndig) {
        char ch = d[key];
        if (edit_mode) {
            /* port: digits only */
            size_t l = strlen(edit_ip);
            if (l + 1 < sizeof edit_ip) { edit_ip[l] = ch; edit_ip[l + 1] = 0; }
        } else {
            ip_editor_append(edit_ip, sizeof edit_ip, ch);
        }
    } else if (key == edit_ndig + EKEY_BACK) ip_editor_backspace(edit_ip);
    else if (key == edit_ndig + EKEY_CLEAR) edit_ip[0] = 0;
    else if (key == edit_ndig + EKEY_SAVE) {
        if (!edit_valid()) return;
        if (edit_ret == ST_KBD) {
            snprintf(kbd_buf, sizeof kbd_buf, "%s", edit_ip);   /* IPv4 into host field */
            kbd_len = (int)strlen(kbd_buf);
            state = ST_KBD;
            draw_kbd();
        } else {
            snprintf(pe_port, sizeof pe_port, "%s", edit_ip);
            edit_leave_pe();
        }
        return;
    } else if (key == edit_ndig + EKEY_CANCEL) {
        edit_leave_pe();
        return;
    }
    draw_edit();
}

/* ---- profile editor (ST_PE) ------------------------------------------------ */
static void draw_pe(void) {
    wup_screen();
    draw_brand(30, pe_idx >= 0 ? "Edit connection" : "New connection");
    static const char *const flabels[] = { "Name", "Host", "Port", "Auto-connect" };
    static const char *const fvals[] = { NULL, NULL, NULL, NULL };
    (void)fvals;
    char val[PROF_HOST_MAX + 8];
    for (int i = 0; i < 4; i++) {
        int y = 86 + i * 42;
        int sel = pe_sel == i;
        if (sel) {
            gfx_rrect(60, y, GFX_W - 120, 36, 8, RGB(46, 96, 168), 220);
            gfx_rrect_outline(60, y, GFX_W - 120, 36, 8, 1, RGB(120, 190, 255));
        }
        gfx_text(&font_body, 84, y + 24, flabels[i],
                 sel ? RGB(255, 255, 255) : RGB(170, 190, 220));
        const char *v;
        char abuf[8];
        switch (i) {
        case 0: v = pe_prof.name; break;
        case 1: v = pe_prof.host; break;
        case 2: v = pe_port; break;
        default:
            snprintf(abuf, sizeof abuf, "%s", pe_prof.auto_connect ? "on" : "off");
            v = abuf;
        }
        /* keep the tail visible when the value overflows the field */
        size_t vl = strlen(v);
        const char *show = v;
        if (vl > 52) { show = v + vl - 52; val[0] = '<'; }
        snprintf(val + (show != v), sizeof val - (show != v), "%s", show);
        gfx_text(&font_body, 320, y + 24, val,
                 v[0] ? (sel ? RGB(255, 255, 255) : RGB(215, 225, 245))
                      : (sel ? RGB(140, 170, 210) : RGB(110, 125, 155)));
    }
    /* endpoint preview: host:port, tail kept visible */
    {
        char ep[PROF_HOST_MAX + 8], ebuf[80];
        snprintf(ep, sizeof ep, "%s:%s",
                 pe_prof.host[0] ? pe_prof.host : "-",
                 pe_port[0] ? pe_port : "-");
        size_t el = strlen(ep);
        const char *eshow = ep;
        ebuf[0] = 0;
        if (el > 58) { eshow = ep + el - 58; ebuf[0] = '<'; }
        strncat(ebuf, eshow, sizeof ebuf - strlen(ebuf) - 1);
        gfx_text_c(&font_small, GFX_W / 2, 266, ebuf, RGB(140, 190, 235));
    }
    if (pe_err)
        gfx_text_c(&font_body, GFX_W / 2, 288, pe_err_msg, RGB(255, 150, 130));
    /* Save / Cancel buttons (pe_sel 4 / 5) */
    static const char *const bl[] = { "Save", "Cancel" };
    for (int i = 0; i < 2; i++) {
        int x = i ? 500 : 190, y = 312, w = 170, h = 44;
        int sel = pe_sel == 4 + i;
        int enabled = i == 1 || pe_prof.name[0] || pe_prof.host[0];
        px_t fill = !enabled ? RGB(30, 36, 48) : sel ? RGB(46, 96, 168) : RGB(24, 38, 62);
        px_t text = !enabled ? RGB(110, 120, 140) : RGB(240, 246, 255);
        gfx_rrect(x, y, w, h, 8, fill, 230);
        if (sel && enabled) gfx_rrect_outline(x, y, w, h, 8, 2, RGB(130, 205, 255));
        gfx_text_c(&font_body, x + w / 2, y + h / 2 + 7, bl[i], text);
    }
    gfx_text_c(&font_small, GFX_W / 2, 452,
               "D-pad + A   touch   B: Cancel   fields: Name/Host alphanumeric, Port numeric",
               RGB(140, 155, 190));
    gfx_present_all();
}

static void pe_field_touch(int idx) {              /* 0..3 field, 4 save, 5 cancel */
    pe_sel = idx;
    if (idx == 4) { pe_save(); return; }
    if (idx == 5) { state = ST_MENU; draw_menu(); return; }
    if (idx == 0) {                                 /* name -> alphanumeric kbd */
        snprintf(kbd_buf, sizeof kbd_buf, "%s", pe_prof.name);
        kbd_len = (int)strlen(kbd_buf); kbd_target = 0;
        kbd_open();
    } else if (idx == 1) {                          /* host -> alphanumeric kbd */
        snprintf(kbd_buf, sizeof kbd_buf, "%s", pe_prof.host);
        kbd_len = (int)strlen(kbd_buf); kbd_target = 1;
        kbd_open();
    } else if (idx == 2) {                          /* port -> numeric pad */
        edit_open(1, pe_port, ST_PE);
    } else if (idx == 3) {                          /* auto-connect toggle */
        pe_prof.auto_connect ^= 1;
        draw_pe();
    }
}

static void pe_activate(int sel) {
    if (sel >= 0 && sel <= 5) { pe_field_touch(sel); return; }
}

static void pe_save(void) {
    pe_err = 0; pe_err_msg[0] = 0;
    if (!prof_name_valid(pe_prof.name)) {
        pe_err = 1; snprintf(pe_err_msg, sizeof pe_err_msg,
            "Name: 1-%d chars, letters/digits/space . - _",
            PROF_NAME_MAX - 1); draw_pe(); return;
    }
    if (!prof_host_valid(pe_prof.host)) {
        pe_err = 2; snprintf(pe_err_msg, sizeof pe_err_msg,
            "Host: DNS name or IPv4, no spaces/URL parts"); draw_pe(); return;
    }
    if (!prof_port_valid(pe_port)) {
        pe_err = 3; snprintf(pe_err_msg, sizeof pe_err_msg,
            "Port: digits only, 1-65535"); draw_pe(); return;
    }
    pe_prof.port = (uint16_t)prof_port_num(pe_port);
    pe_prof.type = PROF_TYPE_UDP;
    int idx;
    if (pe_idx >= 0) {
        profs.p[pe_idx] = pe_prof;
        idx = pe_idx;
    } else {
        idx = prof_add(&profs, &pe_prof);
        if (idx < 0) {
            pe_err = 1; snprintf(pe_err_msg, sizeof pe_err_msg,
                "Cannot save: list full or invalid data"); draw_pe(); return;
        }
    }
    profs.sel = idx;
    menu_prof = idx;
    cfg_touch(); cfg_save();
    state = ST_MENU; menu_sel = idx < profs.n ? idx : 0;
    snprintf(status_msg, sizeof status_msg, "Connection saved");
    draw_menu();
}

/* pe field/button hit test: fields are full-width rows, buttons fixed */
static int pe_hit(int x, int y) {
    for (int i = 0; i < 4; i++) {
        int fy = 86 + i * 42;
        if (y >= fy && y < fy + 36 && x >= 60 && x <= GFX_W - 60) return i;
    }
    if (y >= 312 && y < 356) {
        if (x >= 190 && x < 360) return 4;
        if (x >= 500 && x < 670) return 5;
    }
    return -1;
}

/* ---- alphanumeric keyboard (ST_KBD) ---------------------------------------- */
enum { KK_CHAR, KK_SHIFT, KK_BKSP, KK_CLR, KK_SPACE, KK_IP, KK_SAVE, KK_CANCEL };
typedef struct {
    char ch; const char *label; int kind;
    int row, col, x, y, w, h;
} kkey_t;
static kkey_t kkeys[64];
static int kbd_n;
static int kbd_shift;
static int kbd_sel = -1, kbd_touch = -1, kbd_hover = -1;

static int kbd_char_ok(int target, char ch) {
    if (target == 0) return prof_name_valid((char[]){ ch, 0 });
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '-';
}

static void kbd_layout(void) {
    static const char *const rows[] = {
        "1234567890", "qwertyuiop", "asdfghjkl.-", "zxcvbnm_"
    };
    kbd_n = 0;
    int y = 128;
    for (int r = 0; r < 4; r++) {
        int n = (int)strlen(rows[r]);
        int pitch = 76, w = 70, h = 42;
        int x0 = (GFX_W - n * pitch) / 2;
        for (int c = 0; c < n; c++) {
            kkey_t *k = &kkeys[kbd_n++];
            memset(k, 0, sizeof *k);
            k->kind = KK_CHAR;
            k->ch = rows[r][c];
            k->row = r; k->col = c;
            k->x = x0 + c * pitch; k->y = y; k->w = w; k->h = h;
        }
        y += 50;
    }
    /* action row */
    struct { const char *l; int kind; int w; } acts[] = {
        { "SPACE", KK_SPACE, 150 }, { "aA", KK_SHIFT, 84 },
        { "Bksp", KK_BKSP, 84 }, { "Clr", KK_CLR, 84 },
        { "IP", KK_IP, 76 }, { "Save", KK_SAVE, 120 }, { "Cancel", KK_CANCEL, 120 }
    };
    int x = 20, ay = 336;
    for (unsigned i = 0; i < sizeof acts / sizeof acts[0]; i++) {
        kkey_t *k = &kkeys[kbd_n++];
        memset(k, 0, sizeof *k);
        k->kind = acts[i].kind;
        k->label = acts[i].l;
        k->row = 4; k->col = (int)i;
        k->x = x; k->y = ay; k->w = acts[i].w; k->h = 46;
        x += acts[i].w + 8;
    }
}

static void kbd_open(void) {
    kbd_layout();
    kbd_shift = 0;
    kbd_sel = 0; kbd_touch = kbd_hover = -1;
    state = ST_KBD;
    draw_kbd();
}

static int kbd_key_enabled(const kkey_t *k) {
    if (k->kind == KK_CHAR) {
        char ch = k->ch;
        if (kbd_shift && ch >= 'a' && ch <= 'z') ch -= 32;
        return kbd_char_ok(kbd_target, ch);
    }
    if (k->kind == KK_SPACE) return kbd_target == 0;   /* space: names only */
    if (k->kind == KK_IP)   return kbd_target == 1;   /* IPv4 pad: host only */
    return 1;
}

static void kbd_key_label(char *out, size_t sz, const kkey_t *k) {
    if (k->kind == KK_CHAR) {
        char ch = k->ch;
        if (kbd_shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        snprintf(out, sz, "%c", ch);
    } else if (k->kind == KK_SHIFT) {
        snprintf(out, sz, "%s", kbd_shift ? "AA" : "aA");
    } else {
        snprintf(out, sz, "%s", k->label);
    }
}

static void draw_kbd(void) {
    wup_screen();
    draw_brand(26, kbd_target == 0 ? "Connection name" : "Hostname or IPv4");
    /* echo bar: show the tail so long hostnames stay readable */
    int valid = kbd_target == 0 ? prof_name_valid(kbd_buf)
                                : prof_host_valid(kbd_buf);
    gfx_rrect(60, 76, GFX_W - 120, 44, 9, RGB(18, 28, 48), 230);
    gfx_rrect_outline(60, 76, GFX_W - 120, 44, 9, 2,
                      valid ? RGB(100, 210, 145) : RGB(105, 125, 165));
    const char *show = kbd_buf;
    size_t kl = (size_t)kbd_len;
    char tail[72];
    if (kl > 44) {
        memcpy(tail, kbd_buf + kl - 44, 44);
        tail[44] = 0;
        show = tail;
        gfx_text(&font_small, 74, 106, "<", RGB(140, 155, 190));
    }
    gfx_text(&font_body, 80, 106, kbd_len ? show : "_",
             RGB(255, 255, 255));
    gfx_text(&font_small, GFX_W - 80, 106, "|", RGB(120, 140, 175));

    char lb[8];
    for (int i = 0; i < kbd_n; i++) {
        kkey_t *k = &kkeys[i];
        int en = kbd_key_enabled(k);
        int down = i == kbd_touch && i == kbd_hover;
        px_t fill = !en ? RGB(26, 30, 40) : down ? RGB(74, 138, 210) :
                    i == kbd_sel ? RGB(46, 96, 168) : RGB(24, 38, 62);
        px_t text = !en ? RGB(90, 100, 125) : RGB(240, 246, 255);
        gfx_rrect(k->x, k->y, k->w, k->h, 8, fill, 235);
        if (en && (down || i == kbd_sel))
            gfx_rrect_outline(k->x, k->y, k->w, k->h, 8, 2, RGB(130, 205, 255));
        kbd_key_label(lb, sizeof lb, k);
        gfx_text_c(k->kind == KK_CHAR ? &font_body : &font_small,
                   k->x + k->w / 2, k->y + k->h / 2 + 6, lb, text);
    }
    gfx_text_c(&font_small, GFX_W / 2, 408,
               kbd_target == 0
                 ? "Letters, digits, space, . - _   B: Cancel"
                 : "Letters, digits, . -   IP: numeric pad   B: Cancel",
               RGB(140, 155, 190));
    gfx_present_all();
}

static int kbd_hit(int x, int y) {
    for (int i = 0; i < kbd_n; i++)
        if (x >= kkeys[i].x && x < kkeys[i].x + kkeys[i].w &&
            y >= kkeys[i].y && y < kkeys[i].y + kkeys[i].h)
            return i;
    return -1;
}

/* geometric D-pad move: up/down pick the nearest key by x in the adjacent
   row; left/right step within the same row */
static void kbd_move(int dx, int dy) {
    if (kbd_sel < 0) { kbd_sel = 0; return; }
    kkey_t *cur = &kkeys[kbd_sel];
    if (dx) {
        int best = -1, bd = 1 << 28;
        for (int i = 0; i < kbd_n; i++) {
            if (kkeys[i].row != cur->row) continue;
            int d = (kkeys[i].col - cur->col) * (dx > 0 ? 1 : -1);
            if (d > 0 && d < bd) { bd = d; best = i; }
        }
        if (best >= 0) kbd_sel = best;
    } else if (dy) {
        int trow = cur->row + dy;
        int best = -1, bd = 1 << 28, ccx = cur->x + cur->w / 2;
        for (int i = 0; i < kbd_n; i++) {
            if (kkeys[i].row != trow) continue;
            int d = kkeys[i].x + kkeys[i].w / 2 - ccx;
            if (d < 0) d = -d;
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) kbd_sel = best;
    }
}

static void kbd_append_char(char ch) {
    if (kbd_len + 1 >= PROF_HOST_MAX) return;
    kbd_buf[kbd_len++] = ch;
    kbd_buf[kbd_len] = 0;
}

static void kbd_press(int idx) {
    if (idx < 0 || idx >= kbd_n) return;
    kkey_t *k = &kkeys[idx];
    if (!kbd_key_enabled(k)) return;
    switch (k->kind) {
    case KK_CHAR: {
        char ch = k->ch;
        if (kbd_shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        kbd_append_char(ch);
        break;
    }
    case KK_SHIFT: kbd_shift ^= 1; break;
    case KK_BKSP:
        if (kbd_len > 0) kbd_buf[--kbd_len] = 0;
        break;
    case KK_CLR: kbd_len = 0; kbd_buf[0] = 0; break;
    case KK_SPACE: kbd_append_char(' '); break;
    case KK_IP:
        edit_open(0, kbd_buf, ST_KBD);      /* numeric IPv4 into host field */
        return;
    case KK_SAVE: {
        if (kbd_target == 0) {
            if (!prof_name_valid(kbd_buf)) {
                pe_err = 1;
                snprintf(pe_err_msg, sizeof pe_err_msg, "Invalid name");
                state = ST_PE; draw_pe(); return;
            }
            snprintf(pe_prof.name, sizeof pe_prof.name, "%s", kbd_buf);
        } else {
            if (!prof_host_valid(kbd_buf)) {
                pe_err = 2;
                snprintf(pe_err_msg, sizeof pe_err_msg, "Invalid host");
                state = ST_PE; draw_pe(); return;
            }
            snprintf(pe_prof.host, sizeof pe_prof.host, "%s", kbd_buf);
        }
        pe_err = 0;
        state = ST_PE; draw_pe();
        return;
    }
    case KK_CANCEL:
        state = ST_PE; draw_pe();
        return;
    }
    draw_kbd();
}

/* ---- delete confirmation (ST_DELCONF) --------------------------------------- */
static void draw_delconf(void) {
    wup_screen();
    draw_brand(120, "Delete this connection?");
    char line[96];
    if (del_idx >= 0 && del_idx < profs.n)
        snprintf(line, sizeof line, "\"%s\" (%s:%u)", profs.p[del_idx].name,
                 profs.p[del_idx].host, (unsigned)profs.p[del_idx].port);
    else
        snprintf(line, sizeof line, "?");
    gfx_text_c(&font_body, GFX_W / 2, 190, line, RGB(230, 220, 200));
    gfx_text_c(&font_small, GFX_W / 2, 220, "This cannot be undone", RGB(140, 155, 190));
    static const char *const bl[] = { "Delete", "Keep" };
    for (int i = 0; i < 2; i++) {
        int x = i ? 480 : 140, y = 270, w = 230, h = 52;
        int sel = pe_sel == i;
        px_t fill = sel ? RGB(46, 96, 168) : RGB(24, 38, 62);
        gfx_rrect(x, y, w, h, 9, fill, 230);
        if (sel) gfx_rrect_outline(x, y, w, h, 9, 2, RGB(130, 205, 255));
        gfx_text_c(&font_body, x + w / 2, y + h / 2 + 7, bl[i], RGB(240, 246, 255));
    }
    gfx_text_c(&font_small, GFX_W / 2, 380, "B: keep", RGB(140, 155, 190));
    gfx_present_all();
}

static int delconf_hit(int x, int y) {
    if (y >= 270 && y < 322) {
        if (x >= 140 && x < 370) return 0;
        if (x >= 480 && x < 710) return 1;
    }
    return -1;
}

static void delconf_activate(int sel) {
    if (sel == 0 && del_idx >= 0 && del_idx < profs.n) {
        prof_del(&profs, del_idx);
        if (profs.sel >= profs.n) profs.sel = profs.n - 1;
        if (menu_prof >= profs.n) menu_prof = profs.n - 1;
        cfg_touch(); cfg_save();
        state = ST_MENU; menu_sel = 0;
        snprintf(status_msg, sizeof status_msg, "Connection deleted");
    } else {
        state = ST_MENU;
        snprintf(status_msg, sizeof status_msg, "Delete cancelled");
    }
    draw_menu();
}

static void end_session(void) {
    diag_event(40, frames, von);
    wup_video_close();                                   /* (the decoder and its screen layer off; safe when idle) */
    motor(0, 0); mic(0);
    light_now = 0;
    if (light_override) wup_backlight(wup_backlight_level());
    light_off = 0; light_level = 0; light_override = 0;
    peer_on = von = sync = 0; need_idr = 1;
    hk_reset(&hk);
    mode_req_pending = 0;
    diag_event(41, 0, 0);
    diag_active = 0;
    int was = shown;
    shown = 0;
    state = next_state;
    menu_sel = ingame_sub = 0;
    wup_log("thinclient: session ended");
    if (was) gfx_brightness(0);                         /* (dark, then the menu fades in) */
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
        hk_reset(&hk);
        last_peer_ip = from->sin_addr.s_addr;
        state = ST_STREAMING;
        diag_active = 1;
        diag_event(42, from->sin_addr.s_addr, 0);
        wup_log("thinclient: stream started");
    }
    if (!peer_on || from->sin_addr.s_addr != peer_sa.sin_addr.s_addr || from->sin_port != peer_sa.sin_port) return;
    last_ms = now;
    if (h[0] == 'H') {
        mic(h[2] & 1); light(h[3]);
        /* 5th hello byte (new servers): the effective input mode */
        if (len >= 5 && h[4] <= 1) remote_mode = h[4];
    }
    if (h[0] == 'T' && len >= 4 && mode_req_pending && h[1] == mode_reqid) {
        /* ack to our 'T' request: status 0 applied / 1 rejected / 2 invalid,
           byte 3 is the server's effective mode — authoritative */
        mode_req_pending = 0;
        int status = h[2], eff = h[3];
        if (eff <= 1) remote_mode = (u8)eff;
        if (status == 0)
            snprintf(mode_notice, sizeof mode_notice, "Input mode: %s",
                     eff == 0 ? "Gamepad" : "Desktop");
        else if (status == 2)
            snprintf(mode_notice, sizeof mode_notice, "Mode request rejected");
        else
            snprintf(mode_notice, sizeof mode_notice,
                     "Still %s - Gamepad unavailable", eff == 0 ? "Gamepad" : "Desktop");
        mode_notice_ms = now;
        wup_log("thinclient: mode switch acknowledged");
    }
    if (h[0] == 'V' && n == 16 && state != ST_INGAME) video(d + 16, len, h, now);
    else if (h[0] == 'A' && len > 8) audio(d + 8, len);
    else if (h[0] == 'R') motor(h[1] & 1, now);
    else if (h[0] == 'Q') {
        /* the server ended the session: reconnect when auto-connect is on */
        if (auto_connect) {
            next_state = ST_CONNECTING;
            pending_resolve = 1;
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
        pending_resolve = 1;
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
            /* direct endpoint: unicast 'B' to the resolved server; LAN
               discovery: broadcast 'B' to every listener on server_port */
            struct sockaddr_in to = { .sin_family = AF_INET,
                .sin_port = htons((u16)server_port),
                .sin_addr.s_addr = server_addr ? server_addr : 0xffffffffu };
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
        static const char *const name[] = { "frames ", " keyframes ", " lost ", " resent ", " stalls ", " full ", " gap-ms ", " queue " };
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
    const volatile u8 *report = wup_input_report();
    u16 seq = (u16)(report[0] | report[1] << 8);
    if (seq != in_seq || now - in_ms > 50) {
        u8 m[8 + 128] = { 'I', (u8)(need_idr | (von ? waiting() << 1 : 0)) };
        u32 since = wup_us_since_vblank();                   /* since the screen's last refresh (the server times frames to it) */
        m[2] = (u8)since; m[3] = (u8)(since >> 8); m[4] = (u8)(light_off ? 0 : level_now());
        const volatile u8 *rep = report;
        u8 neutral[128];
        if (state == ST_INGAME) {                           /* local menu: release everything remote */
            for (int i = 0; i < 128; i++) neutral[i] = report[i];
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
        /* HOME shortcuts (home_keys.c): standalone presses become a remote
           pulse on release; combo partners are masked while HOME is held.
           In the local menu nothing remote may leak, HOME included. */
        buttons = hk_filter(&hk, (u16)buttons, now);
        if (state == ST_INGAME) buttons &= (u16)~HK_HOME;
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
    int was = shown;
    wup_video_close();                                   /* decoder and video layer off (safe when idle) */
    von = 0; need_idr = 1; sync = 0;                     /* decoder idle; resync on resume */
    shown = 0;
    in_ms = 0;                                    /* neutral report out within a tick */
    if (was) gfx_brightness(0);                          /* (dark, then the menu fades in) */
    draw_ingame();
    gfx_fade(0, 256);
}

static void resume_stream(void) {
    state = ST_STREAMING;
    need_idr = 1;                                 /* next 'I' asks for a keyframe: the decoder restarts on it */
    /* the decoder reopens lazily on the next fed frame (frame_done) */
}

static void ingame_activate(int sel) {
    switch (sel) {
    case 0: resume_stream(); break;
    case 1: next_state = ST_MENU; snprintf(status_msg, sizeof status_msg, "Disconnected");
            end_session(); break;
    case 2: next_state = ST_CONNECTING; pending_resolve = 1;
            snprintf(status_msg, sizeof status_msg, "Reconnecting ...");
            end_session(); break;
    case 3: ingame_sub = 1; draw_ingame(); break;
    case 4: quit_app = 1; break;
    }
}

int main(void) {
    last_clk = CLOCK;
    for (int i = 0; i < NASM; i++) if (!(as[i].data = malloc(FRAME_MAX))) return 1;   /* (frames put together: ~700 KB) */
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
    /* start in the connection menu; auto-connect only fires for the selected
       startup profile flagged auto-connect, or for migrated LAN discovery */
    menu_sel = 0;
    menu_prof = profs.sel >= 0 && profs.sel < profs.n ? profs.sel
                     : (profs.n ? 0 : -1);
    int startup_connect = 0;
    if (net_ok) {
        if (menu_prof >= 0 && profs.p[menu_prof].auto_connect) {
            use_profile_endpoint(menu_prof);
            startup_connect = 1;
        } else if (profs.disc_auto) {
            server_host[0] = 0;
            server_port = profs.disc_port;
            auto_connect = profs.disc_auto;
            startup_connect = 1;
        }
    }
    if (startup_connect) {
        begin_connect();
        if (state != ST_CONNECTING) {              /* DNS failure: menu + error */
            menu_sel = 0;
        }
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
        int trelease = !tnow && touch_prev;
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

        /* HOME shortcut engine: polled every iteration while a session
           exists so edge tracking stays exact; events act only while the
           stream is live (the local menu keeps its own controls). */
        unsigned hk_ev = 0;
        if (peer_on) hk_poll(&hk, b, now, &hk_ev);
        mode_timeout(now);
        if (hk_ev && state == ST_STREAMING) {
            if (hk_ev & HK_EV_MENU)       enter_ingame();
            if (hk_ev & HK_EV_MODE)       mode_request(now);
            if (hk_ev & HK_EV_BRIGHT_UP)  local_light_step(1);
            if (hk_ev & HK_EV_BRIGHT_DN)  local_light_step(-1);
        }

        /* a reconnect queued by peer_lost/'Q'/ingame Reconnect: re-resolve
           the endpoint (the hostname may now answer with a new address) */
        if (state == ST_CONNECTING && pending_resolve) {
            pending_resolve = 0;
            begin_connect();
        }

        /* connect attempt timed out: back to the menu */
        if (state == ST_CONNECTING && !peer_on && (int)(now - conn_deadline) >= 0) {
            state = ST_MENU; menu_sel = 0;
            snprintf(status_msg, sizeof status_msg, "No answer from server");
            draw_menu();
        }

        /* ---- local UI ---------------------------------------------------- */
        if (state == ST_EDIT) {
            if (tpress) {
                edit_touch = edit_hover = edit_button_at(tx, ty);
                if (edit_touch >= 0) { edit_sel = edit_touch; draw_edit(); }
            } else if (tnow && edit_touch >= 0) {
                int hover = edit_button_at(tx, ty);
                if (hover != edit_hover) { edit_hover = hover; draw_edit(); }
            } else if (trelease && edit_touch >= 0) {
                int key = edit_touch, activate = edit_hover == edit_touch;
                edit_touch = edit_hover = -1;
                if (activate) edit_activate(key); else draw_edit();
            } else if (pressed & WUP_B) { state = ST_PE; draw_pe(); }
            else if (pressed & WUP_LEFT || pressed & WUP_RIGHT) {
                int d = (pressed & WUP_RIGHT) ? 1 : -1;
                if (edit_sel >= edit_ndig) {           /* action column: vertical */
                    if (pressed & WUP_LEFT) edit_sel = edit_ndig - 1;
                } else {
                    int col = edit_sel % 3;
                    if (col == 2 && d > 0 && edit_sel + 1 < edit_ndig)
                        edit_sel = edit_ndig;          /* into action column */
                    else {
                        int ns = edit_sel + d;
                        if (ns >= 0 && ns < edit_ndig) edit_sel = ns;
                    }
                }
                draw_edit();
            } else if (pressed & WUP_UP || pressed & WUP_DOWN) {
                int d = (pressed & WUP_DOWN) ? 1 : -1;
                if (edit_sel >= edit_ndig) {           /* action column */
                    int ns = edit_sel + d;
                    if (ns >= edit_ndig && ns < edit_nkeys) edit_sel = ns;
                } else {
                    int ns = edit_sel + 3 * d;
                    if (ns < 0) ns = edit_sel % 3;
                    if (ns >= edit_ndig) ns = edit_ndig - 1 - (edit_ndig - 1 - ns) % 3;
                    if (ns >= edit_ndig) ns = edit_ndig - 1;
                    edit_sel = ns;
                }
                draw_edit();
            } else if (pressed & WUP_A) edit_activate(edit_sel);
            held = b;
            continue;
        }
        if (state == ST_MENU) {
            int total = profs.n + ACT_COUNT;
            int hit = tpress ? prof_hit(tx, ty) : -1;
            if (hit >= 0) {
                if (hit < profs.n) {                 /* profile row: select, never connect */
                    menu_sel = menu_prof = hit;
                    if (profs.sel != hit) { profs.sel = hit; cfg_touch(); }
                    draw_menu();
                } else {
                    menu_sel = hit;
                    menu_activate(hit);
                }
            } else if (pressed & WUP_UP)    { menu_sel = (menu_sel + total - 1) % total; draw_menu(); }
            else if (pressed & WUP_DOWN)  { menu_sel = (menu_sel + 1) % total; draw_menu(); }
            else if (pressed & WUP_A)     { menu_activate(menu_sel); }
            else if (pressed & WUP_B)     { quit_app = 1; }
        } else if (state == ST_PE) {
            int hit = tpress ? pe_hit(tx, ty) : -1;
            if (hit >= 0) { pe_sel = hit; pe_field_touch(hit); }
            else if (pressed & WUP_UP)    { if (pe_sel > 0) pe_sel--; draw_pe(); }
            else if (pressed & WUP_DOWN)  { if (pe_sel < 5) pe_sel++; draw_pe(); }
            else if (pressed & WUP_LEFT)  { if (pe_sel == 5) { pe_sel = 4; draw_pe(); } }
            else if (pressed & WUP_RIGHT) { if (pe_sel == 4) { pe_sel = 5; draw_pe(); } }
            else if (pressed & WUP_A)     { pe_activate(pe_sel); }
            else if (pressed & WUP_B)     { state = ST_MENU; draw_menu(); }
        } else if (state == ST_KBD) {
            if (tpress) {
                kbd_touch = kbd_hover = kbd_hit(tx, ty);
                if (kbd_touch >= 0) { kbd_sel = kbd_touch; draw_kbd(); }
            } else if (tnow && kbd_touch >= 0) {
                int hover = kbd_hit(tx, ty);
                if (hover != kbd_hover) { kbd_hover = hover; draw_kbd(); }
            } else if (trelease && kbd_touch >= 0) {
                int key = kbd_touch, activate = kbd_hover == kbd_touch;
                kbd_touch = kbd_hover = -1;
                if (activate) kbd_press(key); else draw_kbd();
            } else if (pressed & WUP_B) { state = ST_PE; pe_err = 0; draw_pe(); }
            else if (pressed & WUP_LEFT)  { kbd_move(-1, 0); draw_kbd(); }
            else if (pressed & WUP_RIGHT) { kbd_move(1, 0); draw_kbd(); }
            else if (pressed & WUP_UP)    { kbd_move(0, -1); draw_kbd(); }
            else if (pressed & WUP_DOWN)  { kbd_move(0, 1); draw_kbd(); }
            else if (pressed & WUP_A)     { if (kbd_sel >= 0) kbd_press(kbd_sel); }
        } else if (state == ST_DELCONF) {
            int hit = tpress ? delconf_hit(tx, ty) : -1;
            if (hit >= 0) { pe_sel = hit; delconf_activate(hit); }
            else if (pressed & WUP_LEFT)  { pe_sel = 0; draw_delconf(); }
            else if (pressed & WUP_RIGHT) { pe_sel = 1; draw_delconf(); }
            else if (pressed & WUP_A)     { delconf_activate(pe_sel); }
            else if (pressed & WUP_B)     { delconf_activate(1); }
        } else if (state == ST_CONNECTING) {
            if (pressed & WUP_B) {
                state = ST_MENU; menu_sel = 0;
                snprintf(status_msg, sizeof status_msg, "Cancelled");
                draw_menu();
            }
        } else if (state == ST_STREAMING) {
            /* HOME is no longer a menu hotkey: short press forwards remotely,
               5 s hold opens this menu (hk_poll above), combos act locally */
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
    cfg_save();
    diag_run = 0;
    if (diag_thread >= 0 && wup_thread_join(diag_thread, 1000)) wup_thread_kill(diag_thread);
    return 0;
}
