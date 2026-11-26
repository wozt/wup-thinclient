#include "ip_editor.h"

#include <assert.h>
#include <string.h>

int main(void) {
    static const char *const valid[] = {
        "0.0.0.0", "192.168.1.20", "255.255.255.255", "001.02.3.4"
    };
    static const char *const invalid[] = {
        "", "1.2.3", "1.2.3.4.", ".1.2.3.4", "1..2.3.4",
        "256.1.1.1", "999.1.1.1", "1.2.3.4.5", "a.b.c.d"
    };
    char text[16] = "192.168.1.2";
    char full[16] = "255.255.255.25";

    for (unsigned i = 0; i < sizeof valid / sizeof valid[0]; i++)
        assert(ip_editor_valid(valid[i]));
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        assert(!ip_editor_valid(invalid[i]));

    assert(ip_editor_append(text, sizeof text, '0'));
    assert(!strcmp(text, "192.168.1.20"));
    assert(ip_editor_append(full, sizeof full, '5'));
    assert(ip_editor_valid(full));
    assert(!ip_editor_append(full, sizeof full, '1'));
    assert(!ip_editor_append(text, sizeof text, 'x'));
    ip_editor_backspace(text);
    assert(!strcmp(text, "192.168.1.2"));
    return 0;
}
