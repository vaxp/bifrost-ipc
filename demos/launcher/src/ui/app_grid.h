#pragma once

#include <gtk/gtk.h>
#include <glib.h>
#include "../core/app_entry.h"

G_BEGIN_DECLS

#define VAXP_TYPE_APP_GRID (vaxp_app_grid_get_type ())
G_DECLARE_FINAL_TYPE (VaxpAppGrid, vaxp_app_grid, VAXP, APP_GRID, GtkBox)

/**
 * VaxpAppGrid - Paginated icon grid + pagination dots.
 * Lays out AppEntry list in pages of APPS_PER_PAGE items.
 */
#define APPS_PER_PAGE   25
#define GRID_COLUMNS     5

GtkWidget *vaxp_app_grid_new          (GPtrArray  *apps);
void       vaxp_app_grid_set_filter   (VaxpAppGrid *grid,
                                        const char   *query);
void       vaxp_app_grid_go_next_page (VaxpAppGrid *grid);
void       vaxp_app_grid_go_prev_page (VaxpAppGrid *grid);
void       vaxp_app_grid_set_apps     (VaxpAppGrid *grid,
                                        GPtrArray    *apps);

G_END_DECLS
