#pragma once

#include <stddef.h>

/* Small, allocation-free helpers shared by the GamePad UI and host tests. */
int ip_editor_valid(const char *text);
int ip_editor_append(char *text, size_t size, char ch);
void ip_editor_backspace(char *text);
