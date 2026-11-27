#include "../pad/profiles.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %d: %s\n", __LINE__, #cond); fails++; } \
} while (0)

static void test_validators(void) {
    /* names */
    CHECK(prof_name_valid("Home Desktop"));
    CHECK(prof_name_valid("Raspberry_Pi-2"));
    CHECK(!prof_name_valid(""));
    CHECK(!prof_name_valid("bad|name"));
    CHECK(!prof_name_valid("a\tb"));
    char longname[PROF_NAME_MAX + 1];
    memset(longname, 'a', PROF_NAME_MAX);
    longname[PROF_NAME_MAX] = 0;
    CHECK(!prof_name_valid(longname));

    /* hosts: IPv4 and DNS names */
    CHECK(prof_host_valid("192.168.1.100"));
    CHECK(prof_host_valid("desktop.example.com"));
    CHECK(prof_host_valid("a-b.example-domain.org"));
    CHECK(!prof_host_valid(""));
    CHECK(!prof_host_valid("-bad.example.com"));
    CHECK(!prof_host_valid("bad-.example.com"));
    CHECK(!prof_host_valid("bad..example.com"));
    CHECK(!prof_host_valid("bad_host.example.com"));
    CHECK(!prof_host_valid("http://x.example.com"));
    CHECK(!prof_host_valid("host:50001"));
    CHECK(!prof_host_valid("user@host"));
    char longhost[260];
    memset(longhost, 'a', 254);
    longhost[254] = 0;
    CHECK(!prof_host_valid(longhost));
    /* 253-char single-label is too long per label rule anyway; build valid:
       four 63-char labels + 3 dots = 255 -> too long; use 63.63.63.61 = 253 */
    {
        char h[254];
        memset(h, 'a', 63); h[63] = '.';
        memset(h + 64, 'b', 63); h[127] = '.';
        memset(h + 128, 'c', 63); h[191] = '.';
        memset(h + 192, 'd', 61); h[253] = 0;
        CHECK(prof_host_valid(h));
        h[253] = 'e'; h[254] = 0;               /* 254 chars */
        CHECK(!prof_host_valid(h));
    }

    /* ports */
    CHECK(prof_port_valid("50001"));
    CHECK(prof_port_valid("1"));
    CHECK(prof_port_valid("65535"));
    CHECK(!prof_port_valid("0"));
    CHECK(!prof_port_valid("65536"));
    CHECK(!prof_port_valid("5080abc"));
    CHECK(!prof_port_valid(""));
    CHECK(!prof_port_valid("-1"));
    CHECK(prof_port_num("5080") == 5080);
    CHECK(prof_port_num("abc") == 0);
}

static void test_add_del_sel(void) {
    profiles_t ps;
    prof_init(&ps);
    CHECK(ps.n == 0 && ps.sel == -1 && ps.next_id == 1);

    prof_t a = { .name = "Home", .host = "192.168.1.100", .port = 50001,
                 .type = PROF_TYPE_UDP, .auto_connect = 0 };
    int i0 = prof_add(&ps, &a);
    CHECK(i0 == 0 && ps.p[0].id == 1);
    prof_t b = a;
    strcpy(b.name, "Cloud");
    strcpy(b.host, "desktop.example.com");
    b.port = 5080;
    b.auto_connect = 1;
    int i1 = prof_add(&ps, &b);
    CHECK(i1 == 1 && ps.p[1].id == 2);
    CHECK(ps.next_id == 3);

    /* invalid drafts rejected, list unchanged */
    prof_t bad = a;
    strcpy(bad.host, "not a host!");
    CHECK(prof_add(&ps, &bad) == -1 && ps.n == 2);
    strcpy(bad.host, "ok.example.com");
    bad.port = 0;
    CHECK(prof_add(&ps, &bad) == -1);
    strcpy(bad.name, "");
    CHECK(prof_add(&ps, &bad) == -1);

    /* select second, delete first: selection follows the same profile */
    ps.sel = 1;
    CHECK(prof_del(&ps, 0) == 0 && ps.n == 1);
    CHECK(ps.sel == 0 && ps.p[0].id == 2);
    CHECK(prof_del(&ps, 5) == -1);

    /* bound */
    profiles_t full;
    prof_init(&full);
    for (int i = 0; i < PROF_MAX; i++) {
        prof_t t = a;
        char nm[PROF_NAME_MAX];
        snprintf(nm, sizeof nm, "P%d", i);
        strcpy(t.name, nm);
        CHECK(prof_add(&full, &t) == i);
    }
    CHECK(prof_add(&full, &a) == -1);
}

static void test_serialize_roundtrip(void) {
    profiles_t ps;
    prof_init(&ps);
    prof_t a = { .id = 7, .name = "Home Desktop",
                 .host = "192.168.1.100", .port = 50003,
                 .type = PROF_TYPE_UDP, .auto_connect = 1 };
    prof_t b = { .name = "Cloud", .host = "desktop.example.com",
                 .port = 5080, .type = PROF_TYPE_UDP, .auto_connect = 0 };
    int ia = prof_add(&ps, &a);   /* keeps explicit id 7 */
    int ib = prof_add(&ps, &b);
    CHECK(ia == 0 && ib == 1);
    CHECK(ps.p[0].id == 7);
    ps.sel = 1;
    ps.disc_auto = 1;

    char buf[2048];
    int n = prof_serialize(&ps, buf, sizeof buf);
    CHECK(n > 0 && (size_t)n < sizeof buf);
    CHECK(strstr(buf, "v=2\n"));
    CHECK(strstr(buf, "P7|Home Desktop|192.168.1.100|50003|0|1"));
    CHECK(strstr(buf, "P8|Cloud|desktop.example.com|5080|0|0"));

    profiles_t ps2;
    CHECK(prof_parse(&ps2, buf, (size_t)n) == 0);
    CHECK(ps2.n == 2);
    CHECK(!strcmp(ps2.p[0].name, "Home Desktop"));
    CHECK(ps2.p[0].id == 7 && ps2.p[0].port == 50003);
    CHECK(ps2.p[0].auto_connect == 1);
    CHECK(ps2.p[1].id == 8);
    CHECK(ps2.sel == 1);          /* sel resolved by id after P lines */
    CHECK(ps2.p[ps2.sel].id == 8);
    CHECK(ps2.disc_auto == 1);
    CHECK(ps2.next_id >= 9);
}

static void test_legacy_migration(void) {
    /* bare IPv4 */
    profiles_t ps;
    const char *bare = "192.168.1.50";
    CHECK(prof_parse(&ps, bare, strlen(bare)) == 0);
    CHECK(ps.n == 1);
    CHECK(!strcmp(ps.p[0].host, "192.168.1.50"));
    CHECK(ps.p[0].port == 50002 && ps.p[0].auto_connect == 0);
    CHECK(ps.sel == 0);

    /* host port=N auto=1 */
    const char *full = "desktop.example.com port=5080 auto=1";
    CHECK(prof_parse(&ps, full, strlen(full)) == 0);
    CHECK(ps.n == 1);
    CHECK(!strcmp(ps.p[0].host, "desktop.example.com"));
    CHECK(ps.p[0].port == 5080 && ps.p[0].auto_connect == 1);

    /* any auto=1 -> discovery auto, no profiles */
    const char *anycfg = "any auto=1";
    CHECK(prof_parse(&ps, anycfg, strlen(anycfg)) == 0);
    CHECK(ps.n == 0 && ps.disc_auto == 1 && ps.sel == -1);

    /* corrupt file -> empty but valid state */
    CHECK(prof_parse(&ps, "\x01\x02garbage!!!", 14) == 0);
    CHECK(ps.n == 0);

    /* truncated v2 file: valid lines kept, bad lines skipped */
    const char *t = "v=2\nsel=1\ndisc=0\nP1|A|192.168.0.1|50001|0|0\n"
                    "P2|Bad|bad host!|50001|0|0\nP3|C|10.0.0.2|7000|0|1\n";
    CHECK(prof_parse(&ps, t, strlen(t)) == 0);
    CHECK(ps.n == 2);            /* the bad-host line is skipped */
    CHECK(ps.p[1].id == 3 && ps.p[1].port == 7000);
}

static void test_port_migration(void) {
    /* versioned profiles keep explicitly configured ports — including
       50001, which a user may set on purpose; only the pre-versioned
       legacy format migrates the old shared default to 50002 */
    profiles_t ps;
    const char *t = "v=2\nP1|Old|192.168.0.1|50001|0|0\n"
                    "P2|Custom|192.168.0.2|50001|0|0\n"
                    "P3|Keep|192.168.0.3|5080|0|0\n";
    CHECK(prof_parse(&ps, t, strlen(t)) == 0);
    CHECK(ps.n == 3);
    CHECK(ps.p[0].port == 50001 && ps.p[1].port == 50001 && ps.p[2].port == 5080);

    /* legacy format: bare host defaults to 50002, explicit 50001 migrates */
    const char *bare = "192.168.1.50";
    CHECK(prof_parse(&ps, bare, strlen(bare)) == 0);
    CHECK(ps.n == 1 && ps.p[0].port == 50002);
    const char *old = "192.168.1.51 port=50001";
    CHECK(prof_parse(&ps, old, strlen(old)) == 0);
    CHECK(ps.n == 1 && ps.p[0].port == 50002);
}

/* Exercises the same commit path the profile editor uses (prof_commit):
   add-by-empty-id, edit-by-stable-id, delete isolation, and a full
   serialize/parse persistence cycle with four distinct ports. */
static void test_editor_workflow(void) {
    profiles_t ps;
    prof_init(&ps);
    static const char *const hosts[] = {
        "192.168.2.100", "192.168.2.204", "192.168.2.50", "10.0.0.9"
    };
    static const int ports[] = { 50002, 50003, 50004, 50005 };
    prof_t d;

    /* four Adds: each must get a fresh unique id and keep its own data */
    for (int i = 0; i < 4; i++) {
        memset(&d, 0, sizeof d);
        d.type = PROF_TYPE_UDP;
        snprintf(d.name, sizeof d.name, "Srv%d", i);
        snprintf(d.host, sizeof d.host, "%s", hosts[i]);
        d.port = (uint16_t)ports[i];
        int idx = prof_commit(&ps, &d);
        CHECK(idx == i && ps.p[i].id == (uint16_t)(i + 1) &&
              ps.p[i].port == (uint16_t)ports[i]);
    }
    CHECK(ps.n == 4 && ps.next_id == 5);

    /* Edit profile id=3 (index 2): only that slot may change, by id —
       even if the array order shifted underneath */
    memset(&d, 0, sizeof d);
    d.id = ps.p[2].id;
    strcpy(d.name, "Renamed");
    strcpy(d.host, "192.168.2.99");
    d.port = 50009;
    d.type = PROF_TYPE_UDP;
    CHECK(prof_commit(&ps, &d) == 2);
    CHECK(!strcmp(ps.p[2].name, "Renamed") && ps.p[2].port == 50009 &&
          ps.p[2].id == 3);
    CHECK(ps.p[0].port == 50002 && ps.p[1].port == 50003 &&
          ps.p[3].port == 50005);           /* neighbours untouched */

    /* commit with an unknown id must fail, not append or corrupt */
    memset(&d, 0, sizeof d);
    d.id = 999; strcpy(d.name, "Ghost"); strcpy(d.host, "1.2.3.4"); d.port = 1;
    CHECK(prof_commit(&ps, &d) == -1 && ps.n == 4);

    /* delete index 0: survivors keep id, name, host, port */
    CHECK(prof_del(&ps, 0) == 0 && ps.n == 3);
    CHECK(ps.p[0].id == 2 && ps.p[0].port == 50003);
    CHECK(ps.p[1].id == 3 && ps.p[1].port == 50009);
    CHECK(ps.p[2].id == 4 && ps.p[2].port == 50005);

    /* persistence: serialize -> parse -> every profile intact */
    char buf[3400];
    int n = prof_serialize(&ps, buf, sizeof buf);
    CHECK(n > 0);
    profiles_t q;
    CHECK(prof_parse(&q, buf, (size_t)n) == 0 && q.n == 3);
    CHECK(q.p[0].port == 50003 && q.p[1].port == 50009 &&
          q.p[2].port == 50005);
    CHECK(q.next_id >= 5);

    /* same IP on different ports and same port on different IPs coexist */
    prof_init(&ps);
    memset(&d, 0, sizeof d); d.type = PROF_TYPE_UDP;
    strcpy(d.name, "A"); strcpy(d.host, "192.168.2.100"); d.port = 50002;
    CHECK(prof_commit(&ps, &d) == 0);
    memset(&d, 0, sizeof d); d.type = PROF_TYPE_UDP;
    strcpy(d.name, "B"); strcpy(d.host, "192.168.2.100"); d.port = 50003;
    CHECK(prof_commit(&ps, &d) == 1);
    memset(&d, 0, sizeof d); d.type = PROF_TYPE_UDP;
    strcpy(d.name, "C"); strcpy(d.host, "192.168.2.204"); d.port = 50002;
    CHECK(prof_commit(&ps, &d) == 2);
    CHECK(ps.n == 3);
}

int main(void) {
    test_validators();
    test_add_del_sel();
    test_serialize_roundtrip();
    test_legacy_migration();
    test_port_migration();
    test_editor_workflow();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("test_profiles: all passed\n");
    return 0;
}
