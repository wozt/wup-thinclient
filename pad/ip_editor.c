#include "ip_editor.h"

#include <string.h>

int ip_editor_valid(const char *text) {
    int octets = 0;
    const char *p = text;

    if (!p || !*p) return 0;
    while (*p) {
        int digits = 0, value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p++ - '0');
            if (++digits > 3 || value > 255) return 0;
        }
        if (!digits || ++octets > 4) return 0;
        if (!*p) break;
        if (*p++ != '.') return 0;
        if (!*p) return 0;
    }
    return octets == 4;
}

int ip_editor_append(char *text, size_t size, char ch) {
    size_t len;

    if (!text || size < 2 || (ch != '.' && (ch < '0' || ch > '9'))) return 0;
    len = strlen(text);
    if (len + 1 >= size) return 0;
    text[len] = ch;
    text[len + 1] = 0;
    return 1;
}

void ip_editor_backspace(char *text) {
    size_t len;

    if (!text) return;
    len = strlen(text);
    if (len) text[len - 1] = 0;
}
