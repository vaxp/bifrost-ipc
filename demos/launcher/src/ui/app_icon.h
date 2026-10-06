#pragma once

#include <gtk/gtk.h>
#include "../core/app_entry.h"

G_BEGIN_DECLS

#define VAXP_TYPE_APP_ICON (vaxp_app_icon_get_type ())
G_DECLARE_FINAL_TYPE (VaxpAppIcon, vaxp_app_icon, VAXP, APP_ICON, GtkButton)

/**
 * VaxpAppIcon - A single tappable app icon widget.
 * Shows a 96×96 icon image + app name label below.
 */
GtkWidget *vaxp_app_icon_new       (AppEntry *entry);
void       vaxp_app_icon_set_entry (VaxpAppIcon *icon, AppEntry *entry);
AppEntry  *vaxp_app_icon_get_entry (VaxpAppIcon *icon);

G_END_DECLS
