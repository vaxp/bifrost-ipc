#include "ui/launcher_window.h"
#include <gtk/gtk.h>

/* -------------------------------------------------------------------------
 * GtkApplication activate callback
 * ------------------------------------------------------------------------- */

static void
on_activate (GtkApplication *app, gpointer user_data)
{
    (void) user_data;

    GList *windows = gtk_application_get_windows (app);
    if (windows) {
        GtkWidget *win = windows->data;
        /* Toggle visibility */
        if (gtk_widget_get_visible (win)) {
            gtk_widget_hide (win);
        } else {
            vaxp_launcher_window_show_launcher (VAXP_LAUNCHER_WINDOW (win));
        }
    } else {
        GtkWidget *win = vaxp_launcher_window_new (app);
        vaxp_launcher_window_show_launcher (VAXP_LAUNCHER_WINDOW (win));
    }
}

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

int
main (int argc, char *argv[])
{
    gdk_set_allowed_backends ("wayland,x11");

    GtkApplication *app = gtk_application_new (
        "org.vaxp.Launcher",
        G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

    int status = g_application_run (G_APPLICATION (app), argc, argv);

    g_object_unref (app);
    return status;
}
