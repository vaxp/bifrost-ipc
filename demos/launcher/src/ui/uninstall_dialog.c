#include "uninstall_dialog.h"
#include "launcher_window.h"
#include "client_ipc.h"

#include <string.h>

/* -------------------------------------------------------------------------
 * Signals
 * ------------------------------------------------------------------------- */

enum {
    SIGNAL_UNINSTALL_DONE,
    SIGNAL_DISMISS,
    N_SIGNALS
};
static guint signals[N_SIGNALS];

/* -------------------------------------------------------------------------
 * Widget struct
 *
 * The widget is a full-screen GtkBox (backdrop) that contains a centered
 * "card" GtkBox.  It is added to the launcher's GtkOverlay so it lives
 * inside the same Wayland surface.
 * ------------------------------------------------------------------------- */

struct _VaxpUninstallDialog {
    GtkBox      parent_instance;    /* full-screen backdrop */

    AppEntry   *entry;              /* Not owned */

    /* Inner card stack */
    GtkWidget  *card;               /* styled card container */
    GtkWidget  *stack;
    GtkWidget  *page_confirm;
    GtkWidget  *page_progress;
    GtkWidget  *page_result;

    /* Widgets updated at runtime */
    GtkWidget  *spinner;
    GtkWidget  *status_label;
    GtkWidget  *result_icon;
    GtkWidget  *result_label;
    GtkWidget  *result_detail;
};

G_DEFINE_TYPE (VaxpUninstallDialog, vaxp_uninstall_dialog, GTK_TYPE_BOX)

/* -------------------------------------------------------------------------
 * CSS
 * ------------------------------------------------------------------------- */

static void
inject_dialog_css (void)
{
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;

    static const char *css =
        /* Full-screen backdrop (dimmer) */
        ".uninstall-backdrop {"
        "  background: rgba(0,0,0,0.55);"
        "}"
        /* Floating card */
        ".uninstall-card {"
        "  background: rgba(18,20,28,0.96);"
        "  border-radius: 18px;"
        "  border: 1px solid rgba(255,255,255,0.10);"
        "  box-shadow: 0 24px 64px rgba(0,0,0,0.7);"
        "}"
        /* App name */
        ".uninstall-app-name {"
        "  font-size: 17px;"
        "  font-weight: bold;"
        "  color: rgba(255,255,255,0.92);"
        "}"
        /* Warning text */
        ".uninstall-warn {"
        "  font-size: 13px;"
        "  color: rgba(255,255,255,0.55);"
        "}"
        /* Confirm (red) button */
        ".uninstall-confirm-btn {"
        "  background: rgba(210,45,45,0.88);"
        "  color: white;"
        "  border-radius: 9px;"
        "  padding: 7px 20px;"
        "  border: none;"
        "  font-weight: 600;"
        "  font-size: 13px;"
        "}"
        ".uninstall-confirm-btn:hover {"
        "  background: rgba(190,28,28,0.98);"
        "}"
        /* Cancel button */
        ".uninstall-cancel-btn {"
        "  background: rgba(255,255,255,0.08);"
        "  color: rgba(255,255,255,0.72);"
        "  border-radius: 9px;"
        "  padding: 7px 18px;"
        "  border: none;"
        "  font-size: 13px;"
        "}"
        ".uninstall-cancel-btn:hover {"
        "  background: rgba(255,255,255,0.15);"
        "}"
        /* Spinner label */
        ".uninstall-status {"
        "  color: rgba(255,255,255,0.65);"
        "  font-size: 13px;"
        "}"
        /* Result title */
        ".uninstall-result-title {"
        "  font-size: 16px;"
        "  font-weight: bold;"
        "  color: rgba(255,255,255,0.88);"
        "}"
        /* Result detail */
        ".uninstall-result-detail {"
        "  font-size: 12px;"
        "  color: rgba(255,255,255,0.50);"
        "}"
        ".result-success { color: #4cda8a; }"
        ".result-error   { color: #e05555; }";

    GtkCssProvider *prov = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (prov, css, -1, NULL);
    gtk_style_context_add_provider_for_screen (
        gdk_screen_get_default (),
        GTK_STYLE_PROVIDER (prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 5);
    g_object_unref (prov);
}

/* -------------------------------------------------------------------------
 * Dismiss helper
 * ------------------------------------------------------------------------- */

static void
dismiss_self (VaxpUninstallDialog *self)
{
    /* Find the launcher window and ask it to remove this overlay */
    GtkWidget *overlay_parent = gtk_widget_get_parent (GTK_WIDGET (self));
    if (overlay_parent && GTK_IS_OVERLAY (overlay_parent)) {
        GtkWidget *launcher = gtk_widget_get_parent (overlay_parent);
        if (launcher && VAXP_IS_LAUNCHER_WINDOW (launcher)) {
            vaxp_launcher_window_pop_overlay (
                VAXP_LAUNCHER_WINDOW (launcher), GTK_WIDGET (self));
            return;
        }
    }
    /* Fallback */
    gtk_widget_destroy (GTK_WIDGET (self));
}

/* -------------------------------------------------------------------------
 * Page builders
 * ------------------------------------------------------------------------- */

static void
on_cancel_clicked (GtkButton *btn, gpointer data)
{
    (void) btn;
    VaxpUninstallDialog *self = VAXP_UNINSTALL_DIALOG (data);
    g_signal_emit (self, signals[SIGNAL_DISMISS], 0);
    dismiss_self (self);
}

static GtkWidget *
build_confirm_page (VaxpUninstallDialog *self)
{
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start  (vbox, 32);
    gtk_widget_set_margin_end    (vbox, 32);
    gtk_widget_set_margin_top    (vbox, 28);
    gtk_widget_set_margin_bottom (vbox, 24);

    /* App icon */
    GtkWidget *icon_img = gtk_image_new_from_icon_name (
        self->entry && self->entry->icon_name
            ? self->entry->icon_name : "application-x-executable",
        GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size (GTK_IMAGE (icon_img), 64);
    gtk_widget_set_halign (icon_img, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (vbox), icon_img, FALSE, FALSE, 0);

    /* App name */
    GtkWidget *name_lbl = gtk_label_new (
        self->entry && self->entry->name ? self->entry->name : "");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (name_lbl), "uninstall-app-name");
    gtk_label_set_xalign (GTK_LABEL (name_lbl), 0.5f);
    gtk_box_pack_start (GTK_BOX (vbox), name_lbl, FALSE, FALSE, 0);

    /* Warning */
    const char *pkg = (self->entry && self->entry->package_name)
                      ? self->entry->package_name : NULL;
    char *warn_text;
    if (pkg) {
        warn_text = g_strdup_printf (
            "سيتم محاولة إزالة الحزمة \"%s\"\n"
            "وحذف ملف الاختصار.\n"
            "هذا الإجراء لا يمكن التراجع عنه.", pkg);
    } else {
        warn_text = g_strdup (
            "سيتم حذف اختصار التطبيق.\n"
            "هذا الإجراء لا يمكن التراجع عنه.");
    }
    GtkWidget *warn = gtk_label_new (warn_text);
    g_free (warn_text);
    gtk_style_context_add_class (gtk_widget_get_style_context (warn), "uninstall-warn");
    gtk_label_set_justify (GTK_LABEL (warn), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap (GTK_LABEL (warn), TRUE);
    gtk_box_pack_start (GTK_BOX (vbox), warn, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (vbox),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                        FALSE, FALSE, 4);

    /* Buttons */
    GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
    gtk_box_pack_start (GTK_BOX (vbox), btn_box, FALSE, FALSE, 0);

    GtkWidget *cancel_btn = gtk_button_new_with_label ("إلغاء");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (cancel_btn), "uninstall-cancel-btn");
    g_signal_connect (cancel_btn, "clicked", G_CALLBACK (on_cancel_clicked), self);
    gtk_box_pack_end (GTK_BOX (btn_box), cancel_btn, FALSE, FALSE, 0);

    GtkWidget *confirm_btn = gtk_button_new_with_label ("حذف التطبيق");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (confirm_btn), "uninstall-confirm-btn");
    g_signal_connect_swapped (confirm_btn, "clicked",
                              G_CALLBACK (vaxp_uninstall_dialog_run_async), self);
    gtk_box_pack_end (GTK_BOX (btn_box), confirm_btn, FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *
build_progress_page (VaxpUninstallDialog *self)
{
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_margin_start  (vbox, 40);
    gtk_widget_set_margin_end    (vbox, 40);
    gtk_widget_set_margin_top    (vbox, 36);
    gtk_widget_set_margin_bottom (vbox, 36);
    gtk_widget_set_valign (vbox, GTK_ALIGN_CENTER);

    self->spinner = gtk_spinner_new ();
    gtk_widget_set_halign (self->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (self->spinner, 48, 48);
    gtk_box_pack_start (GTK_BOX (vbox), self->spinner, FALSE, FALSE, 0);

    self->status_label = gtk_label_new ("جارٍ إزالة التطبيق\342\200\246");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (self->status_label), "uninstall-status");
    gtk_box_pack_start (GTK_BOX (vbox), self->status_label, FALSE, FALSE, 0);

    return vbox;
}

static GtkWidget *
build_result_page (VaxpUninstallDialog *self)
{
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_start  (vbox, 32);
    gtk_widget_set_margin_end    (vbox, 32);
    gtk_widget_set_margin_top    (vbox, 28);
    gtk_widget_set_margin_bottom (vbox, 24);
    gtk_widget_set_valign (vbox, GTK_ALIGN_CENTER);

    self->result_icon = gtk_image_new_from_icon_name (
        "dialog-information", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size (GTK_IMAGE (self->result_icon), 52);
    gtk_widget_set_halign (self->result_icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (vbox), self->result_icon, FALSE, FALSE, 0);

    self->result_label = gtk_label_new ("");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (self->result_label), "uninstall-result-title");
    gtk_label_set_xalign (GTK_LABEL (self->result_label), 0.5f);
    gtk_box_pack_start (GTK_BOX (vbox), self->result_label, FALSE, FALSE, 0);

    self->result_detail = gtk_label_new ("");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (self->result_detail), "uninstall-result-detail");
    gtk_label_set_line_wrap (GTK_LABEL (self->result_detail), TRUE);
    gtk_label_set_justify (GTK_LABEL (self->result_detail), GTK_JUSTIFY_CENTER);
    gtk_label_set_max_width_chars (GTK_LABEL (self->result_detail), 38);
    gtk_box_pack_start (GTK_BOX (vbox), self->result_detail, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (vbox),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                        FALSE, FALSE, 4);

    GtkWidget *close_btn = gtk_button_new_with_label ("\330\245\330\272\331\204\330\247\331\202");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (close_btn), "uninstall-cancel-btn");
    gtk_widget_set_halign (close_btn, GTK_ALIGN_END);
    g_signal_connect (close_btn, "clicked", G_CALLBACK (on_cancel_clicked), self);
    gtk_box_pack_start (GTK_BOX (vbox), close_btn, FALSE, FALSE, 0);

    return vbox;
}

/* We no longer wait for a signal from the daemon because IPC is asynchronous.
   Instead, we just show a success message that the command was sent. */
static gboolean
auto_close_dialog (gpointer data)
{
    VaxpUninstallDialog *self = VAXP_UNINSTALL_DIALOG (data);
    g_signal_emit (self, signals[SIGNAL_DISMISS], 0);
    return G_SOURCE_REMOVE;
}

static void
show_success_and_close (VaxpUninstallDialog *self)
{
    gtk_spinner_stop (GTK_SPINNER (self->spinner));
    gtk_image_set_from_icon_name (GTK_IMAGE (self->result_icon),
                                  "emblem-ok-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_label_set_text (GTK_LABEL (self->result_label),
                        "تم إرسال طلب الحذف للخدمة الخلفية");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (self->result_label), "result-success");

    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "result");
    gtk_widget_show_all (GTK_WIDGET (self));

    g_timeout_add (1500, auto_close_dialog, self);
}

/* -------------------------------------------------------------------------
 * GObject boilerplate
 * ------------------------------------------------------------------------- */

static void
vaxp_uninstall_dialog_finalize (GObject *obj)
{
    G_OBJECT_CLASS (vaxp_uninstall_dialog_parent_class)->finalize (obj);
}

static void
vaxp_uninstall_dialog_class_init (VaxpUninstallDialogClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS (klass);
    obj_class->finalize = vaxp_uninstall_dialog_finalize;

    signals[SIGNAL_UNINSTALL_DONE] =
        g_signal_new ("uninstall-done",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    signals[SIGNAL_DISMISS] =
        g_signal_new ("dismiss",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}

static void
vaxp_uninstall_dialog_init (VaxpUninstallDialog *self)
{
    inject_dialog_css ();

    /* The outer widget IS the backdrop: full-screen, semi-transparent */
    gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

    gtk_style_context_add_class (
        gtk_widget_get_style_context (GTK_WIDGET (self)), "uninstall-backdrop");

    /*
     * Center the card both horizontally and vertically.
     * We use a GtkBox with CENTER alignment — GtkOverlay's
     * pass-through is set to FALSE so clicks on the backdrop
     * don't fall through to the launcher icons.
     */
    self->card = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign (self->card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (self->card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (self->card, 400, -1);
    gtk_style_context_add_class (
        gtk_widget_get_style_context (self->card), "uninstall-card");

    gtk_box_pack_start (GTK_BOX (self), self->card, TRUE, TRUE, 0);

    /* Stack for the three pages */
    self->stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (self->stack),
                                   GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_stack_set_transition_duration (GTK_STACK (self->stack), 180);
    gtk_container_add (GTK_CONTAINER (self->card), self->stack);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

GtkWidget *
vaxp_uninstall_dialog_new (AppEntry *entry)
{
    VaxpUninstallDialog *self =
        g_object_new (VAXP_TYPE_UNINSTALL_DIALOG, NULL);

    self->entry = entry;

    /* Build pages now that entry is set */
    self->page_confirm  = build_confirm_page  (self);
    self->page_progress = build_progress_page (self);
    self->page_result   = build_result_page   (self);

    gtk_stack_add_named (GTK_STACK (self->stack), self->page_confirm,  "confirm");
    gtk_stack_add_named (GTK_STACK (self->stack), self->page_progress, "progress");
    gtk_stack_add_named (GTK_STACK (self->stack), self->page_result,   "result");

    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "confirm");

    return GTK_WIDGET (self);
}

void
vaxp_uninstall_dialog_run_async (VaxpUninstallDialog *self)
{
    g_return_if_fail (VAXP_IS_UNINSTALL_DIALOG (self));
    g_return_if_fail (self->entry != NULL);

    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "progress");
    gtk_spinner_start (GTK_SPINNER (self->spinner));
    gtk_widget_show_all (GTK_WIDGET (self));

    client_ipc_send_uninstall (self->entry->desktop_path, self->entry->package_name, self->entry->exec);
    show_success_and_close (self);
}
