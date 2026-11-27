#include "profiles.h"

#include <stdio.h>
#include <string.h>

/* next whitespace-delimited token; returns NULL at end */
static char *next_tok(char **pp) {
    char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!*p) { *pp = p; return NULL; }
    char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    if (*p) *p++ = 0;
    *pp = p;
    return start;
}

static int parse_dec(const char *s, int *out) {
    int v = 0, d = 0;
    if (!s || !*s) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9' || ++d > 5) return -1;
        v = v * 10 + (*p - '0');
        if (v > 65535) return -1;
    }
    *out = v;
    return 0;
}

void prof_init(profiles_t *ps) {
    memset(ps, 0, sizeof *ps);
    ps->sel = -1;
    ps->next_id = 1;
    ps->disc_port = 50002;   /* (50002: WUP ThinServer's own discovery port, away from WUP Stream's 50001) */
}

static int name_char(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '-' ||
           c == '_';
}

int prof_name_valid(const char *s) {
    size_t n;
    if (!s) return 0;
    n = strlen(s);
    if (!n || n >= PROF_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if (!name_char((unsigned char)s[i])) return 0;
    return 1;
}

static int host_char(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-';
}

/* RFC 1035-ish: 1..253 chars, dot-separated labels of 1..63 chars drawn
   from [A-Za-z0-9-], no label may start or end with a hyphen. A dotted
   IPv4 quad satisfies the same rules, so no separate branch is needed. */
int prof_host_valid(const char *s) {
    size_t n, label = 0;
    int labels = 0;
    if (!s) return 0;
    n = strlen(s);
    if (!n || n > 253) return 0;
    for (size_t i = 0; i <= n; i++) {
        char c = s[i];
        if (c == '.' || c == 0) {
            if (!label || label > 63) return 0;
            if (s[i - label] == '-') return 0;          /* leading hyphen */
            if (s[i - 1] == '-') return 0;              /* trailing hyphen */
            labels++;
            label = 0;
            if (!c) break;
            continue;
        }
        if (!host_char((unsigned char)c)) return 0;
        label++;
    }
    return labels >= 1;
}

int prof_port_valid(const char *s) {
    int v = 0, digits = 0;
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9' || ++digits > 5) return 0;
        v = v * 10 + (*p - '0');
    }
    return v >= 1 && v <= 65535;
}

int prof_port_num(const char *s) {
    int v = 0;
    if (!prof_port_valid(s)) return 0;
    for (const char *p = s; *p; p++) v = v * 10 + (*p - '0');
    return v;
}

int prof_endpoint_valid(const char *host, int port) {
    return prof_host_valid(host) && port >= 1 && port <= 65535;
}

char *prof_endpoint(const prof_t *p, char *buf, size_t size) {
    if (!p || !buf || size < PROF_HOST_MAX + 8) return NULL;
    snprintf(buf, size, "%s:%u", p->host, (unsigned)p->port);
    return buf;
}

int prof_find_id(const profiles_t *ps, uint16_t id) {
    for (int i = 0; i < ps->n; i++)
        if (ps->p[i].id == id) return i;
    return -1;
}

int prof_add(profiles_t *ps, const prof_t *src) {
    if (ps->n >= PROF_MAX || !src) return -1;
    if (!prof_name_valid(src->name) || !prof_endpoint_valid(src->host, src->port))
        return -1;
    prof_t *d = &ps->p[ps->n];
    *d = *src;
    if (!d->id) {
        d->id = ps->next_id;
        if (d->id == 0xffff) d->id = 1;   /* wrap: ids are u16 */
    }
    if (d->id >= ps->next_id) ps->next_id = d->id + 1;
    return ps->n++;
}

int prof_del(profiles_t *ps, int idx) {
    if (idx < 0 || idx >= ps->n) return -1;
    memmove(&ps->p[idx], &ps->p[idx + 1],
            (size_t)(ps->n - idx - 1) * sizeof ps->p[0]);
    ps->n--;
    if (ps->sel == idx) ps->sel = -1;
    else if (ps->sel > idx) ps->sel--;
    return 0;
}

/* ---- text format ------------------------------------------------------- */

static void put_str(char *buf, size_t size, size_t *off, const char *s) {
    size_t n = strlen(s);
    if (*off + n >= size) n = size - *off - 1;
    memcpy(buf + *off, s, n);
    *off += n;
    buf[*off] = 0;
}

int prof_serialize(const profiles_t *ps, char *buf, size_t size) {
    size_t off = 0;
    if (!buf || !size) return -1;
    buf[0] = 0;
    put_str(buf, size, &off, "v=2\nsel=");
    char num[16];
    snprintf(num, sizeof num, "%d\n", ps->sel >= 0 && ps->sel < ps->n ? ps->p[ps->sel].id : -1);
    put_str(buf, size, &off, num);
    snprintf(num, sizeof num, "disc=%d\n", ps->disc_auto ? 1 : 0);
    put_str(buf, size, &off, num);
    for (int i = 0; i < ps->n; i++) {
        const prof_t *p = &ps->p[i];
        char line[PROF_HOST_MAX + PROF_NAME_MAX + 32];
        snprintf(line, sizeof line, "P%u|%s|%s|%u|%u|%u\n",
                 (unsigned)p->id, p->name, p->host, (unsigned)p->port,
                 (unsigned)p->type, (unsigned)p->auto_connect);
        put_str(buf, size, &off, line);
    }
    return (int)off;
}

/* copy one '|' or '\n' delimited field; returns field length or -1 */
static int field_next(const char *s, size_t left, size_t *used,
                      char *out, size_t outsz) {
    size_t i = 0;
    while (i < left && s[i] != '|' && s[i] != '\n' && s[i] != '\r') i++;
    if (i >= outsz) i = outsz - 1;
    memcpy(out, s, i);
    out[i] = 0;
    *used = i + (i < left && (s[i] == '|' || s[i] == '\n' || s[i] == '\r'));
    return (int)i;
}

static int parse_u16(const char *s, int *out) {
    int v = 0, d = 0;
    if (!s || !*s) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9' || ++d > 5) return -1;
        v = v * 10 + (*p - '0');
        if (v > 65535) return -1;
    }
    *out = v;
    return 0;
}

/* legacy single-line: "<host|any> [port=N] [auto=N]" (space separated) */
static void migrate_legacy(profiles_t *ps, const char *buf, size_t len) {
    char tmp[512];
    if (len >= sizeof tmp) len = sizeof tmp - 1;
    memcpy(tmp, buf, len);
    tmp[len] = 0;
    char *host = NULL;
    int port = 50002, auto_c = 0, is_any = 0;
    char *p = tmp;
    char *tok;
    while ((tok = next_tok(&p)) != NULL) {
        if (!strncmp(tok, "port=", 5)) {
            int v;
            if (parse_dec(tok + 5, &v) == 0 && v >= 1) port = v;
        } else if (!strncmp(tok, "auto=", 5)) {
            if (tok[5] == '1') auto_c = 1;
        } else if (!strchr(tok, '=')) {
            if (!strcmp(tok, "any")) is_any = 1;
            else host = tok;
        }
    }
    if (is_any) {
        ps->disc_auto = auto_c;               /* old "any auto=1" behavior */
        return;
    }
    if (!host || !prof_host_valid(host)) return;
    prof_t np;
    memset(&np, 0, sizeof np);
    np.port = (uint16_t)port;
    if (np.port == 50001) np.port = 50002;   /* old shared default -> ThinServer port */
    np.type = PROF_TYPE_UDP;
    np.auto_connect = (uint8_t)auto_c;
    size_t hl = strlen(host);
    if (hl >= PROF_HOST_MAX) return;          /* cannot happen: legacy was <=47 */
    memcpy(np.host, host, hl + 1);
    /* display name: the host itself when it fits, else a generic label */
    if (hl < PROF_NAME_MAX && prof_name_valid(host)) {
        memcpy(np.name, host, hl + 1);
    } else {
        memcpy(np.name, "Server", 7);
    }
    int idx = prof_add(ps, &np);
    if (idx >= 0) ps->sel = idx;
}

int prof_parse(profiles_t *ps, const char *buf, size_t len) {
    prof_init(ps);
    if (!buf || !len) return 0;
    /* v=2 detection: first line must be exactly "v=2" */
    if (len >= 3 && !memcmp(buf, "v=2", 3) &&
        (len == 3 || buf[3] == '\n' || buf[3] == '\r')) {
        const char *p = buf + 3;
        size_t left = len - 3;
        int sel_id = -1;                    /* resolved after P lines load */
        while (left && (*p == '\n' || *p == '\r')) { p++; left--; }
        while (left) {
            /* one line at a time */
            size_t linelen = 0;
            while (linelen < left && p[linelen] != '\n') linelen++;
            char line[600];
            size_t ll = linelen < sizeof line - 1 ? linelen : sizeof line - 1;
            memcpy(line, p, ll);
            line[ll] = 0;
            /* strip trailing \r */
            while (ll && (line[ll - 1] == '\r' || line[ll - 1] == '\n'))
                line[--ll] = 0;
            if (!strncmp(line, "sel=", 4)) {
                parse_dec(line + 4, &sel_id);
            } else if (!strncmp(line, "disc=", 5)) {
                ps->disc_auto = line[5] == '1';
            } else if (line[0] == 'P' && line[1]) {
                char idb[8], name[PROF_NAME_MAX], host[PROF_HOST_MAX];
                char portb[8], typeb[8], autob[8];
                size_t qu;
                /* P<id>|name|host|port|type|auto */
                const char *q = line + 1;
                size_t ql = ll - 1;
                field_next(q, ql, &qu, idb, sizeof idb);   q += qu; ql -= qu;
                field_next(q, ql, &qu, name, sizeof name); q += qu; ql -= qu;
                field_next(q, ql, &qu, host, sizeof host); q += qu; ql -= qu;
                field_next(q, ql, &qu, portb, sizeof portb); q += qu; ql -= qu;
                field_next(q, ql, &qu, typeb, sizeof typeb); q += qu; ql -= qu;
                field_next(q, ql, &qu, autob, sizeof autob);
                int pid = 0, type = 0, au = 0;
                if (parse_u16(idb, &pid) == 0 && pid > 0 &&
                    prof_port_valid(portb) &&
                    parse_u16(typeb, &type) == 0 && type <= 250 &&
                    parse_u16(autob, &au) == 0 && au <= 1 &&
                    prof_name_valid(name) && prof_host_valid(host) &&
                    ps->n < PROF_MAX) {
                    prof_t np;
                    memset(&np, 0, sizeof np);
                    np.id = (uint16_t)pid;
                    memcpy(np.name, name, strlen(name) + 1);
                    memcpy(np.host, host, strlen(host) + 1);
                    np.port = (uint16_t)prof_port_num(portb);
                    if (np.port == 50001) np.port = 50002;   /* old shared default -> ThinServer port */
                    np.type = (uint8_t)type;
                    np.auto_connect = (uint8_t)au;
                    prof_add(ps, &np);
                }
            }
            /* advance past this line */
            size_t adv = linelen;
            if (adv < left && p[adv] == '\n') adv++;
            else if (adv < left && p[adv] == '\r') adv++;
            p += adv; left -= adv < left ? adv : left;
        }
        /* sel= may appear before the P lines: resolve the stored id now. */
        if (sel_id >= 0) {
            int idx = prof_find_id(ps, (uint16_t)sel_id);
            if (idx >= 0) ps->sel = idx;
        }
        return 0;
    }
    /* legacy format: migrate */
    migrate_legacy(ps, buf, len);
    return 0;
}
