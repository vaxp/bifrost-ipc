#include "app_entry.h"
#include <stdlib.h>
#include <string.h>

AppEntry *
app_entry_new (void)
{
    AppEntry *e = g_new0 (AppEntry, 1);
    return e;
}

void
app_entry_free (AppEntry *entry)
{
    if (!entry) return;
    g_free (entry->name);
    g_free (entry->exec);
    g_free (entry->icon_name);
    g_free (entry->categories);
    g_free (entry->comment);
    g_free (entry->desktop_path);
    g_free (entry->package_name);
    if (entry->surface) cairo_surface_destroy (entry->surface);
    g_free (entry);
}

/**
 * Strip field codes like %f %u %F %U %d %D %n %N %i %c %k from exec string.
 * Returns newly allocated string — caller must g_free().
 */
char *
app_entry_clean_exec (const char *exec_raw)
{
    if (!exec_raw) return NULL;

    GString *result = g_string_new (NULL);
    const char *p = exec_raw;

    while (*p) {
        if (*p == '%' && *(p + 1) != '\0') {
            p += 2; /* skip %X */
            continue;
        }
        g_string_append_c (result, *p);
        p++;
    }

    /* trim trailing spaces */
    g_strchomp (result->str);

    return g_string_free (result, FALSE);
}
