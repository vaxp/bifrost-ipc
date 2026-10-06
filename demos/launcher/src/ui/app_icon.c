#include "app_icon.h"
#include "client_ipc.h"
#include <string.h>

#define ICON_LOAD_SIZE 96

/* -------------------------------------------------------------------------
 * Widget struct
 * ------------------------------------------------------------------------- */

struct _VaxpAppIcon {
    GtkButton  parent_instance;
    AppEntry  *entry;
    GtkWidget *image;
    GtkWidget *label;
};

G_DEFINE_TYPE (VaxpAppIcon, vaxp_app_icon, GTK_TYPE_BUTTON)



/* -------------------------------------------------------------------------
 * Launch the application
 * ------------------------------------------------------------------------- */

static void
launch_app (VaxpAppIcon *self)
{
    if (!self->entry) return;

    client_ipc_send_launch (self->entry->exec);

    /* Close the launcher window */
    GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
    if (GTK_IS_WINDOW (toplevel))
        gtk_widget_hide (toplevel);
}

static void
on_clicked (GtkButton *btn, gpointer user_data)
{
    (void) user_data;
    launch_app (VAXP_APP_ICON (btn));
}

/* -------------------------------------------------------------------------
 * Context Menu Actions
 * ------------------------------------------------------------------------- */

static void
on_menu_run (GtkMenuItem *item, gpointer data)
{
    (void) item;
    launch_app (VAXP_APP_ICON (data));
}

static void
on_menu_shortcut (GtkMenuItem *item, gpointer data)
{
    (void) item;
    VaxpAppIcon *self  = VAXP_APP_ICON (data);
    
    client_ipc_send_shortcut (self->entry->desktop_path);

    /* Hide window so user sees the change */
    GtkWidget *toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
    if (GTK_IS_WINDOW (toplevel)) gtk_widget_hide (toplevel);
}

/* -------------------------------------------------------------------------
 * Uninstall
 * ------------------------------------------------------------------------- */

#include "uninstall_dialog.h"
#include "launcher_window.h"

static void
on_menu_uninstall (GtkMenuItem *item, gpointer data)
{
    (void) item;
    VaxpAppIcon *self  = VAXP_APP_ICON (data);
    AppEntry     *entry = self->entry;

    if (!entry || !entry->desktop_path) return;

    /* Find the VaxpLauncherWindow */
    GtkWidget *win = gtk_widget_get_toplevel (GTK_WIDGET (self));
    if (!VAXP_IS_LAUNCHER_WINDOW (win)) return;

    /* Build overlay widget and inject it into the launcher surface */
    GtkWidget *dialog = vaxp_uninstall_dialog_new (entry);

    vaxp_launcher_window_push_overlay (VAXP_LAUNCHER_WINDOW (win), dialog);
}

/* -------------------------------------------------------------------------
 * Right Click Menu Setup
 * ------------------------------------------------------------------------- */

static gboolean
on_button_press (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void) data;
    VaxpAppIcon *self = VAXP_APP_ICON (widget);

    /* Right click (button 3) reveals context menu */
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        GtkWidget *menu = gtk_menu_new ();

        GtkWidget *item_run = gtk_menu_item_new_with_label ("Run");
        g_signal_connect (item_run, "activate", G_CALLBACK (on_menu_run), self);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_run);

        GtkWidget *item_shortcut = gtk_menu_item_new_with_label ("Create Shortcut");
        g_signal_connect (item_shortcut, "activate", G_CALLBACK (on_menu_shortcut), self);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_shortcut);

        GtkWidget *item_uninstall = gtk_menu_item_new_with_label ("Uninstall");
        g_signal_connect (item_uninstall, "activate", G_CALLBACK (on_menu_uninstall), self);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_uninstall);

        gtk_widget_show_all (menu);
        gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *) event);

        return TRUE; /* Handled */
    }

    return FALSE;
}

/* -------------------------------------------------------------------------
 * GObject class init
 * ------------------------------------------------------------------------- */

static void
vaxp_app_icon_class_init (VaxpAppIconClass *klass)
{
    (void) klass;
}

static void
vaxp_app_icon_init (VaxpAppIcon *self)
{
    gtk_style_context_add_class (
        gtk_widget_get_style_context (GTK_WIDGET (self)),
        "app-icon-button");

    gtk_button_set_relief (GTK_BUTTON (self), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus (GTK_WIDGET (self), FALSE);

    /* Vertical box inside button */
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    gtk_container_add (GTK_CONTAINER (self), box);

    /* Image placeholder */
    self->image = gtk_image_new_from_icon_name ("application-x-executable",
                                                GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size (GTK_IMAGE (self->image), ICON_LOAD_SIZE);
    gtk_widget_set_halign (self->image, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (box), self->image, FALSE, FALSE, 0);

    /* Label */
    self->label = gtk_label_new ("");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (self->label), "app-name");
    gtk_label_set_max_width_chars (GTK_LABEL (self->label), 12);
    gtk_label_set_ellipsize (GTK_LABEL (self->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines (GTK_LABEL (self->label), 2);
    gtk_label_set_line_wrap (GTK_LABEL (self->label), TRUE);
    gtk_label_set_line_wrap_mode (GTK_LABEL (self->label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_justify (GTK_LABEL (self->label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign (self->label, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (box), self->label, FALSE, FALSE, 0);

    /* Standard left click */
    g_signal_connect (self, "clicked", G_CALLBACK (on_clicked), NULL);

    /* Manual button-press for right click */
    g_signal_connect (self, "button-press-event", G_CALLBACK (on_button_press), NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

GtkWidget *
vaxp_app_icon_new (AppEntry *entry)
{
    VaxpAppIcon *self = g_object_new (VAXP_TYPE_APP_ICON, NULL);
    vaxp_app_icon_set_entry (self, entry);
    return GTK_WIDGET (self);
}

void
vaxp_app_icon_set_entry (VaxpAppIcon *self, AppEntry *entry)
{
    g_return_if_fail (VAXP_IS_APP_ICON (self));

    if (self->entry) {
        /* Already holding an entry? The grid only sets it once, but safe */
        self->entry = NULL;
    }

    self->entry = entry;

    /* 1) Set labels */
    gtk_label_set_text (GTK_LABEL (self->label), entry->name);
    
    if (entry->comment) {
        gtk_widget_set_tooltip_text (GTK_WIDGET (self), entry->comment);
    }

    /* 2) Set icon */
    gtk_image_clear (GTK_IMAGE (self->image));
    gtk_image_set_pixel_size (GTK_IMAGE (self->image), ICON_LOAD_SIZE);

    if (entry->surface) {
        /* We already have the surface from the daemon's zero-copy memory */
        gtk_image_set_from_surface (GTK_IMAGE (self->image), entry->surface);
    } else {
        /* Fallback icon */
        gtk_image_set_from_icon_name (GTK_IMAGE (self->image),
                                      "application-x-executable",
                                      GTK_ICON_SIZE_DIALOG);
    }
}

AppEntry *
vaxp_app_icon_get_entry (VaxpAppIcon *icon)
{
    g_return_val_if_fail (VAXP_IS_APP_ICON (icon), NULL);
    return icon->entry;
}
