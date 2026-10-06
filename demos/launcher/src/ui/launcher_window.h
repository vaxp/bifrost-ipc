#pragma once

#include <gtk/gtk.h>
#include <glib.h>

G_BEGIN_DECLS

#define VAXP_TYPE_LAUNCHER_WINDOW (vaxp_launcher_window_get_type ())
G_DECLARE_FINAL_TYPE (VaxpLauncherWindow, vaxp_launcher_window,
                      VAXP, LAUNCHER_WINDOW, GtkApplicationWindow)

/**
 * VaxpLauncherWindow - Fullscreen, translucent launcher window.
 * Owns the app list, search bar, and icon grid.
 */
GtkWidget *vaxp_launcher_window_new          (GtkApplication *app);
void       vaxp_launcher_window_show_launcher (VaxpLauncherWindow *win);

/**
 * Overlay helpers — add / remove a widget as a centered overlay
 * on top of the launcher contents (inside the same layer-shell surface).
 */
void       vaxp_launcher_window_push_overlay (VaxpLauncherWindow *win,
                                               GtkWidget           *widget);
void       vaxp_launcher_window_pop_overlay  (VaxpLauncherWindow *win,
                                               GtkWidget           *widget);

G_END_DECLS
