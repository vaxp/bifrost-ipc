#include "search_bar.h"

/* -------------------------------------------------------------------------
 * Widget struct
 * ------------------------------------------------------------------------- */

struct _VaxpSearchBar {
    GtkSearchEntry  parent_instance;
    guint           debounce_id;   /* g_timeout source id */
};

G_DEFINE_TYPE (VaxpSearchBar, vaxp_search_bar, GTK_TYPE_SEARCH_ENTRY)

/* Signals */
enum { SIGNAL_SEARCH_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* -------------------------------------------------------------------------
 * Debounce logic
 * ------------------------------------------------------------------------- */

static gboolean
debounce_fire (gpointer user_data)
{
    VaxpSearchBar *self = VAXP_SEARCH_BAR (user_data);
    self->debounce_id = 0;
    g_signal_emit (self, signals[SIGNAL_SEARCH_CHANGED], 0);
    return G_SOURCE_REMOVE;
}

static void
on_text_changed (GtkEditable *editable, gpointer user_data)
{
    (void) user_data;
    VaxpSearchBar *self = VAXP_SEARCH_BAR (editable);

    /* Cancel pending debounce */
    if (self->debounce_id) {
        g_source_remove (self->debounce_id);
        self->debounce_id = 0;
    }

    /* Schedule new debounce: 150 ms */
    self->debounce_id = g_timeout_add (150, debounce_fire, self);
}

/* -------------------------------------------------------------------------
 * GObject class init
 * ------------------------------------------------------------------------- */

static void
vaxp_search_bar_dispose (GObject *obj)
{
    VaxpSearchBar *self = VAXP_SEARCH_BAR (obj);
    if (self->debounce_id) {
        g_source_remove (self->debounce_id);
        self->debounce_id = 0;
    }
    G_OBJECT_CLASS (vaxp_search_bar_parent_class)->dispose (obj);
}

static void
vaxp_search_bar_class_init (VaxpSearchBarClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS (klass);
    obj_class->dispose = vaxp_search_bar_dispose;

    signals[SIGNAL_SEARCH_CHANGED] =
        g_signal_new ("search-changed-debounced",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
}

static void
vaxp_search_bar_init (VaxpSearchBar *self)
{
    self->debounce_id = 0;

    gtk_widget_set_name (GTK_WIDGET (self), "vaxp-search-entry");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (GTK_WIDGET (self)), "search-bar");

    gtk_entry_set_placeholder_text (GTK_ENTRY (self), "Search");
    gtk_entry_set_width_chars (GTK_ENTRY (self), 30);
    gtk_widget_set_halign (GTK_WIDGET (self), GTK_ALIGN_CENTER);

    g_signal_connect (self, "changed",
                      G_CALLBACK (on_text_changed), NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

GtkWidget *
vaxp_search_bar_new (void)
{
    return g_object_new (VAXP_TYPE_SEARCH_BAR, NULL);
}

const char *
vaxp_search_bar_get_text (VaxpSearchBar *bar)
{
    g_return_val_if_fail (VAXP_IS_SEARCH_BAR (bar), "");
    return gtk_entry_get_text (GTK_ENTRY (bar));
}

void
vaxp_search_bar_clear (VaxpSearchBar *bar)
{
    g_return_if_fail (VAXP_IS_SEARCH_BAR (bar));
    gtk_entry_set_text (GTK_ENTRY (bar), "");
}

void
vaxp_search_bar_grab_focus (VaxpSearchBar *bar)
{
    g_return_if_fail (VAXP_IS_SEARCH_BAR (bar));
    gtk_widget_grab_focus (GTK_WIDGET (bar));
}
