#pragma once

#include <glib.h>
#include "app_entry.h"

/**
 * DesktopReader - Scans .desktop files and builds AppEntry list.
 * Searches:
 *   1. /usr/share/applications
 *   2. /usr/local/share/applications
 *   3. ~/.local/share/applications
 *
 * Returns a GPtrArray* of AppEntry* (caller owns — free with
 * g_ptr_array_unref after setting free_func to app_entry_free).
 */
GPtrArray *desktop_reader_load_apps (void);
