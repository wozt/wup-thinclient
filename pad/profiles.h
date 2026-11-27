#pragma once

#include <stddef.h>
#include <stdint.h>

/* Saved connection profiles for WUP ThinClient.
 *
 * Pure logic only — no libwup, no X11 — so the same code runs in the
 * GamePad app and in host-side unit tests. Storage is the versioned text
 * format inside /save/thinclient.cfg (see profiles.c). */

#define PROF_MAX 8
#define PROF_NAME_MAX 32    /* display name, incl. NUL */
#define PROF_HOST_MAX 256   /* hostname or IPv4, incl. NUL (DNS limit 253) */
#define PROF_TYPE_UDP 0     /* only transport today; secure types reserved */

typedef struct {
    uint16_t id;                    /* stable id: survives rename/delete of others */
    char name[PROF_NAME_MAX];
    char host[PROF_HOST_MAX];
    uint16_t port;                  /* UDP destination port, 1..65535 */
    uint8_t type;                   /* PROF_TYPE_* */
    uint8_t auto_connect;           /* connect automatically on startup */
} prof_t;

typedef struct {
    prof_t p[PROF_MAX];
    int n;
    uint16_t next_id;               /* never reuse an id */
    int sel;                        /* selected profile index, -1 = none */
    int disc_auto;                  /* startup: LAN discovery connect */
    int disc_port;                  /* UDP port LAN discovery broadcasts to */
} profiles_t;

void prof_init(profiles_t *ps);

/* Field validators. Host accepts a DNS hostname or a dotted IPv4 quad;
   IPv6 is not supported. */
int prof_name_valid(const char *s);
int prof_host_valid(const char *s);
int prof_port_valid(const char *s);   /* strict decimal, 1..65535 */
int prof_port_num(const char *s);     /* value when valid, else 0 */
int prof_endpoint_valid(const char *host, int port);

/* "host:port" into buf (>= PROF_HOST_MAX + 8). Returns buf or NULL. */
char *prof_endpoint(const prof_t *p, char *buf, size_t size);

int prof_find_id(const profiles_t *ps, uint16_t id);   /* index or -1 */
int prof_add(profiles_t *ps, const prof_t *src);       /* index or -1 */
int prof_del(profiles_t *ps, int idx);                 /* 0 ok, -1 bad index */
/* Editor commit: draft->id == 0 adds (fresh unique id), nonzero id
   replaces that profile by stable id. Returns index or -1. */
int prof_commit(profiles_t *ps, const prof_t *draft);

/* Versioned text format:
     v=2
     sel=<id|-1>
     disc=<0|1>
     P<id>|<name>|<host>|<port>|<type>|<auto>
   Legacy single-line files (bare host, port=N, auto=N, "any") are
   migrated into one profile preserving the auto-connect flag. */
int prof_serialize(const profiles_t *ps, char *buf, size_t size);
/* Parses v=2 and migrates legacy formats. Returns 0 ok, -1 on a file that
   is neither (caller keeps defaults). Never writes past buf/len. */
int prof_parse(profiles_t *ps, const char *buf, size_t len);
