#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VAXP_TYPE_SEARCH_BAR (vaxp_search_bar_get_type ())
G_DECLARE_FINAL_TYPE (VaxpSearchBar, vaxp_search_bar,
                      VAXP, SEARCH_BAR, GtkSearchEntry)

/**
 * VaxpSearchBar - Styled search entry with debounce.
 * Emits "search-changed" signal after 150ms debounce.
 */
GtkWidget  *vaxp_search_bar_new         (void);
const char *vaxp_search_bar_get_text    (VaxpSearchBar *bar);
void        vaxp_search_bar_clear       (VaxpSearchBar *bar);
void        vaxp_search_bar_grab_focus  (VaxpSearchBar *bar);

G_END_DECLS
