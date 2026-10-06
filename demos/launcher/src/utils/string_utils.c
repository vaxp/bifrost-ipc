#include "string_utils.h"
#include <glib.h>
#include <string.h>

bool
str_contains_icase (const char *haystack, const char *needle)
{
    if (!haystack || !needle) return false;
    if (*needle == '\0') return true;

    char *h = g_utf8_casefold (haystack, -1);
    char *n = g_utf8_casefold (needle,   -1);

    bool found = (strstr (h, n) != NULL);

    g_free (h);
    g_free (n);

    return found;
}

char *
str_normalize (const char *input)
{
    if (!input) return g_strdup ("");

    char *lower     = g_utf8_casefold (input, -1);
    char *normalized = g_strstrip (lower);

    return normalized;
}
