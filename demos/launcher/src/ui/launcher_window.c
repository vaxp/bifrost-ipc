#include "launcher_window.h"
#include "search_bar.h"
#include "app_grid.h"
#include "client_ipc.h"
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <gdk/gdk.h>
#ifdef HAVE_GTK_LAYER_SHELL
#include <gtk-layer-shell.h>
#endif
#include <string.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------
 * Private struct
 * ------------------------------------------------------------------------- */

struct _VaxpLauncherWindow {
    GtkApplicationWindow  parent_instance;

    GtkWidget    *search_bar;
    GtkWidget    *app_grid;
    GtkWidget    *root_overlay;
    GtkWidget    *stats_label;

    char         *wallpaper_path;
    GdkPixbuf    *wallpaper_pixbuf;
    cairo_surface_t *wallpaper_surface;
    int           cached_width;
    int           cached_height;
    time_t        wallpaper_mtime;
};

G_DEFINE_TYPE (VaxpLauncherWindow, vaxp_launcher_window,
               GTK_TYPE_APPLICATION_WINDOW)

/* -------------------------------------------------------------------------
 * CSS loader
 * ------------------------------------------------------------------------- */
static void
load_css (void)
{
    GtkCssProvider *provider = gtk_css_provider_new ();

    /* Try installed path first, then development path */
    const char *paths[] = {
        PKGDATADIR "/style/launcher.css",
        "data/style/launcher.css",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        GError *err = NULL;
        if (gtk_css_provider_load_from_path (provider, paths[i], &err)) {
            gtk_style_context_add_provider_for_screen (
                gdk_screen_get_default (),
                GTK_STYLE_PROVIDER (provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            break;
        }
        g_error_free (err);
    }

    g_object_unref (provider);
}

/* -------------------------------------------------------------------------
 * Transparency / RGBA visual
 * ------------------------------------------------------------------------- */

static void
setup_transparency (GtkWidget *widget)
{
    GdkScreen  *screen = gtk_widget_get_screen (widget);
    GdkVisual  *visual = gdk_screen_get_rgba_visual (screen);

    if (visual && gdk_screen_is_composited (screen)) {
        gtk_widget_set_visual (widget, visual);
        gtk_widget_set_app_paintable (widget, TRUE);
    }
}

/* -------------------------------------------------------------------------
 * Window background
 * ------------------------------------------------------------------------- */

static void
load_wallpaper (VaxpLauncherWindow *self)
{
    const char *config_dir = g_get_user_config_dir ();
    char *wallpaper_file_path = g_build_filename (config_dir, "vaxp", "desktop", "background", NULL);

    struct stat st;
    gboolean file_changed = FALSE;

    if (stat (wallpaper_file_path, &st) == 0) {
        if (self->wallpaper_mtime != st.st_mtime) {
            file_changed = TRUE;
            self->wallpaper_mtime = st.st_mtime;
        }
    } else {
        if (self->wallpaper_path != NULL) {
            file_changed = TRUE;
            self->wallpaper_mtime = 0;
        }
    }

    if (file_changed) {
        if (self->wallpaper_surface) {
            cairo_surface_destroy (self->wallpaper_surface);
            self->wallpaper_surface = NULL;
        }
        if (self->wallpaper_pixbuf) {
            g_object_unref (self->wallpaper_pixbuf);
            self->wallpaper_pixbuf = NULL;
        }
        if (self->wallpaper_path) {
            g_free (self->wallpaper_path);
            self->wallpaper_path = NULL;
        }

        GError *pixbuf_error = NULL;
        self->wallpaper_pixbuf = gdk_pixbuf_new_from_file (wallpaper_file_path, &pixbuf_error);
        if (self->wallpaper_pixbuf) {
            self->wallpaper_path = g_strdup (wallpaper_file_path);
        } else {
            if (pixbuf_error) g_error_free (pixbuf_error);
        }
    }

    g_free (wallpaper_file_path);
}

static gboolean
on_window_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    (void) user_data;
    VaxpLauncherWindow *self = VAXP_LAUNCHER_WINDOW (widget);

    GtkAllocation alloc;
    const double radius = 24.0;

    gtk_widget_get_allocation (widget, &alloc);

    cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.300);
    cairo_paint (cr);
    cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

    cairo_new_sub_path (cr);
    cairo_arc (cr, alloc.width - radius, radius, radius, -G_PI / 2.0, 0);
    cairo_arc (cr, alloc.width - radius, alloc.height - radius, radius, 0, G_PI / 2.0);
    cairo_arc (cr, radius, alloc.height - radius, radius, G_PI / 2.0, G_PI);
    cairo_arc (cr, radius, radius, radius, G_PI, 3.0 * G_PI / 2.0);
    cairo_close_path (cr);

    if (self->wallpaper_pixbuf) {
        if (!self->wallpaper_surface || self->cached_width != alloc.width || self->cached_height != alloc.height) {
            if (self->wallpaper_surface) {
                cairo_surface_destroy (self->wallpaper_surface);
                self->wallpaper_surface = NULL;
            }

            self->cached_width = alloc.width;
            self->cached_height = alloc.height;

            int wp = gdk_pixbuf_get_width (self->wallpaper_pixbuf);
            int hp = gdk_pixbuf_get_height (self->wallpaper_pixbuf);

            double scale_x = (double)alloc.width / wp;
            double scale_y = (double)alloc.height / hp;
            double scale = scale_x > scale_y ? scale_x : scale_y;

            int scaled_w = (int)(wp * scale);
            int scaled_h = (int)(hp * scale);

            GdkPixbuf *scaled_pixbuf = gdk_pixbuf_scale_simple (self->wallpaper_pixbuf, scaled_w, scaled_h, GDK_INTERP_BILINEAR);
            if (scaled_pixbuf) {
                self->wallpaper_surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, alloc.width, alloc.height);
                cairo_t *temp_cr = cairo_create (self->wallpaper_surface);

                double offset_x = (alloc.width - scaled_w) / 2.0;
                double offset_y = (alloc.height - scaled_h) / 2.0;

                gdk_cairo_set_source_pixbuf (temp_cr, scaled_pixbuf, offset_x, offset_y);
                cairo_paint (temp_cr);

                cairo_destroy (temp_cr);
                g_object_unref (scaled_pixbuf);
            }
        }

        if (self->wallpaper_surface) {
            cairo_save (cr);
            cairo_clip_preserve (cr);
            cairo_set_source_surface (cr, self->wallpaper_surface, 0, 0);
            cairo_paint (cr);
            cairo_restore (cr);
        }
    } else {
        /* Keep the window itself translucent so the compositor can blur behind it. */
        cairo_set_source_rgba (cr, 0.08, 0.09, 0.12, 0.38);
        cairo_fill_preserve (cr);
    }

    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.12);
    cairo_set_line_width (cr, 1.2);
    cairo_stroke (cr);

    return FALSE;
}


/* -------------------------------------------------------------------------
 * Keyboard handling
 * ------------------------------------------------------------------------- */

static gboolean
on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
    (void) user_data;
    VaxpLauncherWindow *self = VAXP_LAUNCHER_WINDOW (widget);

    if (event->keyval == GDK_KEY_Escape) {
        vaxp_search_bar_clear (VAXP_SEARCH_BAR (self->search_bar));
        gtk_widget_hide (widget);
        return TRUE;
    }

    /* Page Navigation */
    if (event->keyval == GDK_KEY_Page_Up) {
        vaxp_app_grid_go_prev_page (VAXP_APP_GRID (self->app_grid));
        return TRUE;
    } else if (event->keyval == GDK_KEY_Page_Down) {
        vaxp_app_grid_go_next_page (VAXP_APP_GRID (self->app_grid));
        return TRUE;
    }

    /* Page Navigation via Arrow Keys when search is empty */
    const char *search_text = vaxp_search_bar_get_text (VAXP_SEARCH_BAR (self->search_bar));
    if (!search_text || search_text[0] == '\0') {
        if (event->keyval == GDK_KEY_Left) {
            vaxp_app_grid_go_prev_page (VAXP_APP_GRID (self->app_grid));
            return TRUE;
        } else if (event->keyval == GDK_KEY_Right) {
            vaxp_app_grid_go_next_page (VAXP_APP_GRID (self->app_grid));
            return TRUE;
        }
    }

    /* Forward typing to search bar */
    if (!gtk_widget_has_focus (self->search_bar)) {
        gtk_widget_grab_focus (self->search_bar);
    }

    return FALSE;
}

/* -------------------------------------------------------------------------
 * Mouse scroll handling
 * ------------------------------------------------------------------------- */

static gboolean
on_scroll (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
    (void) user_data;
    VaxpLauncherWindow *self = VAXP_LAUNCHER_WINDOW (widget);
    
    /* Accumulate delta for smooth scrolling */
    static double accum_y = 0.0;
    static guint32 last_scroll_time = 0;
    static guint32 last_page_time = 0;

    /* Reset accumulator if a lot of time passed since last scroll */
    if (event->time - last_scroll_time > 200) {
        accum_y = 0.0;
    }
    last_scroll_time = event->time;

    /* Debounce: Ignore events if we just changed a page within 300ms */
    if (event->time - last_page_time < 300) {
        return TRUE;
    }

    if (event->direction == GDK_SCROLL_UP) {
        vaxp_app_grid_go_prev_page (VAXP_APP_GRID (self->app_grid));
        last_page_time = event->time;
        return TRUE;
    } else if (event->direction == GDK_SCROLL_DOWN) {
        vaxp_app_grid_go_next_page (VAXP_APP_GRID (self->app_grid));
        last_page_time = event->time;
        return TRUE;
    } else if (event->direction == GDK_SCROLL_SMOOTH) {
        accum_y += event->delta_y;
        
        if (event->delta_y == 0.0 && event->delta_x != 0.0) {
            accum_y += event->delta_x;
        }

        if (accum_y <= -1.0) {
            vaxp_app_grid_go_prev_page (VAXP_APP_GRID (self->app_grid));
            accum_y = 0.0;
            last_page_time = event->time;
            return TRUE;
        } else if (accum_y >= 1.0) {
            vaxp_app_grid_go_next_page (VAXP_APP_GRID (self->app_grid));
            accum_y = 0.0;
            last_page_time = event->time;
            return TRUE;
        }
    }

    return FALSE;
}

/* -------------------------------------------------------------------------
 * Search changed handler
 * ------------------------------------------------------------------------- */

static void
on_search_changed (GtkWidget *search, gpointer data)
{
    VaxpLauncherWindow *self = VAXP_LAUNCHER_WINDOW (data);
    const char *query = vaxp_search_bar_get_text (VAXP_SEARCH_BAR (search));
    vaxp_app_grid_set_filter (VAXP_APP_GRID (self->app_grid), query);
}



/* -------------------------------------------------------------------------
 * GObject class init
 * ------------------------------------------------------------------------- */

static gboolean
update_stats_ui (gpointer data)
{
    VaxpLauncherWindow *self = VAXP_LAUNCHER_WINDOW (data);

    /* Update stats label 10 times a second so GTK doesn't freeze */
    char stats_text[1024];
    
    double display_val = g_ipc_read_mbps;
    const char *unit = "MB/s";
    
    if (display_val >= 1048576.0) {
        display_val /= 1048576.0;
        unit = "TB/s";
    } else if (display_val >= 1024.0) {
        display_val /= 1024.0;
        unit = "GB/s";
    }

    snprintf(stats_text, sizeof(stats_text),
             "<span foreground='#00FF66' font_weight='bold'>⚡ Bifrost IPC Telemetry (Benchmark Demo):</span>\n"
             "• <b>Polling Rate:</b> <b>%d</b> Hz <span foreground='#FFA500' size='smaller'>[Max CPU Speed — for stress/demo only, not for real-world production]</span>\n"
             "• <b>Access Latency:</b> <b>%.4f</b> ms <span foreground='#AAAAAA' size='smaller'>[Atomic memory load directly from L1/L3 Cache]</span>\n"
             "• <b>Simulated Throughput:</b> <b>%.2f</b> %s <span foreground='#AAAAAA' size='smaller'>[~19 MB app state × polling frequency]</span>\n"
             "• <b>Zero-Copy Rendering:</b> <span foreground='#00FF66'>Active</span> <span foreground='#AAAAAA' size='smaller'>[ARGB32 icons rendered directly from shared RAM]</span>",
             g_ipc_fps, g_ipc_poll_latency_ms, display_val, unit);
    gtk_label_set_markup (GTK_LABEL (self->stats_label), stats_text);

    return G_SOURCE_CONTINUE;
}

static gboolean
poll_ipc_idle (gpointer data)
{
    VaxpLauncherWindow *self = VAXP_LAUNCHER_WINDOW (data);

    bool state_changed = client_ipc_poll_state();

    if (state_changed) {
        SharedLauncherState *state = client_ipc_get_state();
        if (!state) return G_SOURCE_CONTINUE;

        // State changed, rebuild GPtrArray of AppEntry
        GPtrArray *apps = g_ptr_array_new_with_free_func ((GDestroyNotify) app_entry_free);

        for (uint32_t i = 0; i < state->app_count; i++) {
            SharedAppEntry *sa = &state->apps[i];
            AppEntry *entry = g_new0 (AppEntry, 1);
            entry->name = g_strdup(sa->name);
            entry->exec = g_strdup(sa->exec);
            entry->desktop_path = g_strdup(sa->desktop_path);
            entry->package_name = g_strdup(sa->package_name);
            
            if (sa->has_icon) {
                // Zero-Copy: Wrap the shared memory pointer directly!
                entry->surface = cairo_image_surface_create_for_data(
                    sa->icon_data, CAIRO_FORMAT_ARGB32,
                    sa->icon_width, sa->icon_height,
                    sa->icon_rowstride
                );
            }
            g_ptr_array_add(apps, entry);
        }

        vaxp_app_grid_set_apps (VAXP_APP_GRID (self->app_grid), apps);
        
        const char *query = vaxp_search_bar_get_text (VAXP_SEARCH_BAR (self->search_bar));
        vaxp_app_grid_set_filter (VAXP_APP_GRID (self->app_grid), query);
        
        // Decrement our own ref so the grid owns it
        g_ptr_array_unref(apps);
    }

    return G_SOURCE_CONTINUE;
}

static void
vaxp_launcher_window_finalize (GObject *obj)
{
    VaxpLauncherWindow *self = VAXP_LAUNCHER_WINDOW (obj);

    client_ipc_cleanup();
    if (self->wallpaper_path)
        g_free (self->wallpaper_path);
    if (self->wallpaper_pixbuf)
        g_object_unref (self->wallpaper_pixbuf);
    if (self->wallpaper_surface)
        cairo_surface_destroy (self->wallpaper_surface);
    // icon_loader_destroy();
    G_OBJECT_CLASS (vaxp_launcher_window_parent_class)->finalize (obj);
}

static void
vaxp_launcher_window_class_init (VaxpLauncherWindowClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS (klass);
    obj_class->finalize = vaxp_launcher_window_finalize;
}

#ifdef HAVE_GTK_LAYER_SHELL
static void
set_layer_shell_monitor (GtkWindow *win)
{
    GdkDisplay *display = gdk_display_get_default ();
    if (!display) {
        g_warning ("Launcher: no GDK display available for layer-shell monitor");
        return;
    }

    GdkMonitor *monitor = gdk_display_get_primary_monitor (display);
    if (!monitor && gdk_display_get_n_monitors (display) > 0)
        monitor = gdk_display_get_monitor (display, 0);

    if (!monitor) {
        g_warning ("Launcher: no monitor available for layer-shell");
        return;
    }

    GdkRectangle geometry;
    gdk_monitor_get_geometry (monitor, &geometry);
    gtk_widget_set_size_request (GTK_WIDGET (win), geometry.width, geometry.height);
    gtk_layer_set_monitor (win, monitor);
}
#endif

static void
vaxp_launcher_window_init (VaxpLauncherWindow *self)
{
    self->wallpaper_path = NULL;
    self->wallpaper_pixbuf = NULL;
    self->wallpaper_surface = NULL;
    self->cached_width = 0;
    self->cached_height = 0;

    load_wallpaper (self);

    GtkWindow *win = GTK_WINDOW (self);
#ifdef HAVE_GTK_LAYER_SHELL
    gboolean layer_shell_supported = gtk_layer_is_supported ();

    if (layer_shell_supported) {
        gtk_layer_init_for_window (win);
        gtk_layer_set_namespace (win, "launcher");
        set_layer_shell_monitor (win);
        gtk_layer_set_layer (win, GTK_LAYER_SHELL_LAYER_OVERLAY);
        gtk_layer_set_anchor (win, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
        gtk_layer_set_anchor (win, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
        gtk_layer_set_anchor (win, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
        gtk_layer_set_anchor (win, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
        gtk_layer_set_margin (win, GTK_LAYER_SHELL_EDGE_LEFT, 0);
        gtk_layer_set_margin (win, GTK_LAYER_SHELL_EDGE_RIGHT, 0);
        gtk_layer_set_margin (win, GTK_LAYER_SHELL_EDGE_TOP, 0);
        gtk_layer_set_margin (win, GTK_LAYER_SHELL_EDGE_BOTTOM, 0);
        gtk_layer_set_exclusive_zone (win, -1);
        gtk_layer_set_keyboard_mode (win, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    } else {
        /* X11 fallback */
        gtk_window_set_keep_above (win, TRUE);
        gtk_window_set_position (win, GTK_WIN_POS_CENTER_ALWAYS);
        gtk_window_fullscreen (win);
    }
#else
    /* X11 fallback */
    gtk_window_set_keep_above (win, TRUE);
    gtk_window_set_position (win, GTK_WIN_POS_CENTER_ALWAYS);
    gtk_window_fullscreen (win);
#endif

    gtk_window_set_decorated         (win, FALSE);
    gtk_window_set_skip_taskbar_hint (win, TRUE);
    gtk_window_set_skip_pager_hint   (win, TRUE);
    gtk_window_set_type_hint         (win, GDK_WINDOW_TYPE_HINT_SPLASHSCREEN);

    gtk_widget_set_name (GTK_WIDGET (self), "launcher");
    gtk_style_context_add_class (
        gtk_widget_get_style_context (GTK_WIDGET (self)), "launcher-window");

    /* Setup RGBA transparency */
    setup_transparency (GTK_WIDGET (self));

    /* Add scroll events to the window */
    gtk_widget_add_events (GTK_WIDGET (self), GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

    /* Load CSS */
    load_css ();

    /* ── Root overlay ───────────────────────────────────────────────── */
    self->root_overlay = gtk_overlay_new ();
    gtk_container_add (GTK_CONTAINER (self), self->root_overlay);

    /* ── Stats Label ───────────────────────────────────────────────── */
    self->stats_label = gtk_label_new ("");
    gtk_label_set_use_markup (GTK_LABEL (self->stats_label), TRUE);
    gtk_label_set_xalign (GTK_LABEL (self->stats_label), 0.0);
    gtk_widget_set_halign (self->stats_label, GTK_ALIGN_START);
    gtk_widget_set_valign (self->stats_label, GTK_ALIGN_START);
    gtk_widget_set_margin_top (self->stats_label, 12);
    gtk_widget_set_margin_start (self->stats_label, 12);
    
    // Add styled dark backdrop to text
    GtkCssProvider *prov = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (prov,
        "label { background-color: rgba(15, 17, 23, 0.85); padding: 12px 16px; border-radius: 8px; color: #E0E0E0; font-family: monospace; font-size: 13px; border: 1px solid rgba(255,255,255,0.12); }",
        -1, NULL);
    gtk_style_context_add_provider (
        gtk_widget_get_style_context (self->stats_label),
        GTK_STYLE_PROVIDER (prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (prov);
    
    gtk_overlay_add_overlay (GTK_OVERLAY (self->root_overlay), self->stats_label);

    /* ── Main content box ──────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_top    (vbox, 70);
    gtk_widget_set_margin_bottom (vbox, 32);
    gtk_widget_set_margin_start  (vbox, 150);
    gtk_widget_set_margin_end    (vbox, 150);
    gtk_container_add (GTK_CONTAINER (self->root_overlay), vbox);

    /* ── Search Bar ────────────────────────────────────────────────── */
    self->search_bar = vaxp_search_bar_new ();
    gtk_widget_set_halign (self->search_bar, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (vbox), self->search_bar, FALSE, FALSE, 0);

    /* ── Load apps via IPC ───────────────────────────────────────── */
    client_ipc_init();
    
    // Uncapped polling in idle loop!
    g_idle_add (poll_ipc_idle, self);
    // Update visual stats label at 10 fps to prevent GTK freezing
    g_timeout_add (100, update_stats_ui, self);

    /* ── App Grid ──────────────────────────────────────────────────── */
    // Initial empty grid until first poll succeeds
    GPtrArray *empty_apps = g_ptr_array_new_with_free_func ((GDestroyNotify) app_entry_free);
    self->app_grid = vaxp_app_grid_new (empty_apps);
    gtk_box_pack_start (GTK_BOX (vbox), self->app_grid, TRUE, TRUE, 0);
    g_ptr_array_unref(empty_apps);
    g_signal_connect (self->search_bar, "search-changed-debounced",
                      G_CALLBACK (on_search_changed), self);

    g_signal_connect (GTK_WIDGET (self), "key-press-event",
                      G_CALLBACK (on_key_press), NULL);

    g_signal_connect (GTK_WIDGET (self), "scroll-event",
                      G_CALLBACK (on_scroll), NULL);

    g_signal_connect (GTK_WIDGET (self), "draw",
                      G_CALLBACK (on_window_draw), NULL);

    /* ── Emergency Close Button ────────────────────────────────────── */
    GtkWidget *close_btn = gtk_button_new_with_label ("Close Launcher");
    
    GtkCssProvider *close_prov = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (close_prov,
        "button { background-color: rgba(255, 0, 0, 0.8); color: white; border-radius: 10px; padding: 5px 15px; font-weight: bold; } "
        "button:hover { background-color: rgba(255, 50, 50, 1.0); }",
        -1, NULL);
    gtk_style_context_add_provider (
        gtk_widget_get_style_context (close_btn),
        GTK_STYLE_PROVIDER (close_prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (close_prov);

    gtk_widget_set_halign (close_btn, GTK_ALIGN_END);
    gtk_widget_set_valign (close_btn, GTK_ALIGN_START);
    gtk_widget_set_margin_top (close_btn, 15);
    gtk_widget_set_margin_end (close_btn, 15);
    
    g_signal_connect_swapped (close_btn, "clicked", G_CALLBACK (gtk_widget_hide), self);
    gtk_overlay_add_overlay (GTK_OVERLAY (self->root_overlay), close_btn);

}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

GtkWidget *
vaxp_launcher_window_new (GtkApplication *app)
{
    VaxpLauncherWindow *win = g_object_new (
        VAXP_TYPE_LAUNCHER_WINDOW,
        "application", app,
        NULL);

    return GTK_WIDGET (win);
}

void
vaxp_launcher_window_show_launcher (VaxpLauncherWindow *win)
{
    g_return_if_fail (VAXP_IS_LAUNCHER_WINDOW (win));

    vaxp_search_bar_clear (VAXP_SEARCH_BAR (win->search_bar));
    vaxp_app_grid_set_filter (VAXP_APP_GRID (win->app_grid), NULL);

    load_wallpaper (win);
    gtk_widget_queue_draw (GTK_WIDGET (win));

    gtk_widget_show_all (GTK_WIDGET (win));
    gtk_window_present  (GTK_WINDOW (win));

    vaxp_search_bar_grab_focus (VAXP_SEARCH_BAR (win->search_bar));
}

void
vaxp_launcher_window_push_overlay (VaxpLauncherWindow *win,
                                    GtkWidget           *widget)
{
    g_return_if_fail (VAXP_IS_LAUNCHER_WINDOW (win));
    g_return_if_fail (GTK_IS_WIDGET (widget));

    gtk_widget_set_halign (widget, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (widget, GTK_ALIGN_CENTER);

    gtk_overlay_add_overlay (GTK_OVERLAY (win->root_overlay), widget);
    gtk_widget_show_all (widget);
}

void
vaxp_launcher_window_pop_overlay (VaxpLauncherWindow *win,
                                   GtkWidget           *widget)
{
    g_return_if_fail (VAXP_IS_LAUNCHER_WINDOW (win));
    g_return_if_fail (GTK_IS_WIDGET (widget));

    gtk_widget_destroy (widget);
}
