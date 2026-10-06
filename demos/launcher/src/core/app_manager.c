#include "app_manager.h"
#include "desktop_reader.h"
#include <glib/gstdio.h>
#include <string.h>

struct _VaxpAppManager {
    GObject    parent_instance;
    GPtrArray *apps;
    GList     *dir_monitors;
    guint      reload_source_id;
};

G_DEFINE_TYPE (VaxpAppManager, vaxp_app_manager, G_TYPE_OBJECT)

enum {
    SIGNAL_APPS_CHANGED,
    SIGNAL_UNINSTALL_DONE,
    N_SIGNALS
};
static guint manager_signals[N_SIGNALS] = { 0 };

static VaxpAppManager *default_manager = NULL;

/* -------------------------------------------------------------------------
 * Internal App Reloading
 * ------------------------------------------------------------------------- */
static gboolean
reload_apps_idle_cb (gpointer data)
{
    VaxpAppManager *self = VAXP_APP_MANAGER (data);
    self->reload_source_id = 0;

    GPtrArray *new_apps = desktop_reader_load_apps ();
    if (self->apps) {
        g_ptr_array_unref (self->apps);
    }
    self->apps = new_apps;

    g_signal_emit (self, manager_signals[SIGNAL_APPS_CHANGED], 0);

    return G_SOURCE_REMOVE;
}

static void
on_desktop_dir_changed (GFileMonitor *monitor, GFile *file, GFile *other_file,
                        GFileMonitorEvent event_type, gpointer user_data)
{
    (void) monitor; (void) file; (void) other_file; (void) event_type;
    VaxpAppManager *self = VAXP_APP_MANAGER (user_data);

    if (self->reload_source_id != 0) {
        g_source_remove (self->reload_source_id);
    }
    self->reload_source_id = g_timeout_add (500, reload_apps_idle_cb, self);
}

static void
setup_directory_monitors (VaxpAppManager *self)
{
    const char *dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        NULL
    };
    
    for (int i = 0; dirs[i] != NULL; i++) {
        GFile *file = g_file_new_for_path (dirs[i]);
        GFileMonitor *mon = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, NULL);
        if (mon) {
            g_signal_connect (mon, "changed", G_CALLBACK (on_desktop_dir_changed), self);
            self->dir_monitors = g_list_prepend (self->dir_monitors, mon);
        }
        g_object_unref (file);
    }
    
    const char *home = g_get_home_dir ();
    if (home) {
        char *user_dir = g_build_filename (home, ".local", "share", "applications", NULL);
        GFile *file = g_file_new_for_path (user_dir);
        GFileMonitor *mon = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, NULL);
        if (mon) {
            g_signal_connect (mon, "changed", G_CALLBACK (on_desktop_dir_changed), self);
            self->dir_monitors = g_list_prepend (self->dir_monitors, mon);
        }
        g_object_unref (file);
        g_free (user_dir);
    }
}

/* -------------------------------------------------------------------------
 * GObject Boilerplate
 * ------------------------------------------------------------------------- */
static void
vaxp_app_manager_finalize (GObject *object)
{
    VaxpAppManager *self = VAXP_APP_MANAGER (object);

    if (self->reload_source_id != 0) {
        g_source_remove (self->reload_source_id);
        self->reload_source_id = 0;
    }

    if (self->dir_monitors) {
        g_list_free_full (self->dir_monitors, g_object_unref);
        self->dir_monitors = NULL;
    }

    if (self->apps) {
        g_ptr_array_unref (self->apps);
        self->apps = NULL;
    }

    if (default_manager == self) {
        default_manager = NULL;
    }

    G_OBJECT_CLASS (vaxp_app_manager_parent_class)->finalize (object);
}

static void
vaxp_app_manager_class_init (VaxpAppManagerClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS (klass);
    obj_class->finalize = vaxp_app_manager_finalize;

    manager_signals[SIGNAL_APPS_CHANGED] =
        g_signal_new ("apps-changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);

    manager_signals[SIGNAL_UNINSTALL_DONE] =
        g_signal_new ("uninstall-done",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 3,
                      G_TYPE_POINTER, /* AppEntry */
                      G_TYPE_BOOLEAN, /* success */
                      G_TYPE_STRING); /* detail msg */
}

static void
vaxp_app_manager_init (VaxpAppManager *self)
{
    self->apps = desktop_reader_load_apps ();
    setup_directory_monitors (self);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
VaxpAppManager *
vaxp_app_manager_get_default (void)
{
    if (default_manager == NULL) {
        default_manager = g_object_new (VAXP_TYPE_APP_MANAGER, NULL);
    }
    return default_manager;
}

GPtrArray *
vaxp_app_manager_get_apps (VaxpAppManager *self)
{
    g_return_val_if_fail (VAXP_IS_APP_MANAGER (self), NULL);
    return self->apps;
}

void
vaxp_app_manager_launch_app (VaxpAppManager *self, AppEntry *entry)
{
    g_return_if_fail (VAXP_IS_APP_MANAGER (self));
    if (!entry || !entry->exec) return;

    GError *err = NULL;
    if (!g_spawn_command_line_async (entry->exec, &err)) {
        g_warning ("Failed to launch '%s': %s", entry->name, err->message);
        g_error_free (err);
    }
}

void
vaxp_app_manager_create_shortcut (VaxpAppManager *self, AppEntry *entry)
{
    g_return_if_fail (VAXP_IS_APP_MANAGER (self));
    if (!entry || !entry->desktop_path) return;

    const char *home    = g_get_home_dir ();
    char *desktop_dir   = g_build_filename (home, "Desktop", NULL);
    char *filename      = g_path_get_basename (entry->desktop_path);
    char *dest_path     = g_build_filename (desktop_dir, filename, NULL);

    char *cmd = g_strdup_printf ("sh -c \"cp '%s' '%s' && chmod +x '%s'\"",
                                 entry->desktop_path, dest_path, dest_path);
    g_spawn_command_line_async (cmd, NULL);

    g_free (cmd);
    g_free (dest_path);
    g_free (filename);
    g_free (desktop_dir);
}

/* -------------------------------------------------------------------------
 * Uninstall Thread Worker
 * ------------------------------------------------------------------------- */
typedef enum { APP_TYPE_DEB, APP_TYPE_FLATPAK, APP_TYPE_SNAP } AppType;

typedef struct {
    VaxpAppManager *manager;
    AppEntry       *entry;
    char           *desktop_path;
    char           *exec_binary;
    char           *package_hint;
    char           *flatpak_id;
    char           *snap_name;
    AppType         app_type;
} UninstallTask;

typedef struct {
    VaxpAppManager *manager;
    AppEntry       *entry;
    gboolean        success;
    char           *detail_msg;
} UninstallResult;

static char *
run_and_capture (char **argv)
{
    gchar  *sout        = NULL;
    gint    exit_status = 0;
    GError *err         = NULL;

    gboolean ok = g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                NULL, NULL, &sout, NULL, &exit_status, &err);
    g_clear_error (&err);
    if (!ok || exit_status != 0) {
        g_free (sout);
        return NULL;
    }
    if (sout) g_strchomp (sout);
    return sout;
}

static char *
dpkg_search (const char *path)
{
    if (!path || !*path) return NULL;
    char *argv[] = { "dpkg", "-S", (char *) path, NULL };
    char *out    = run_and_capture (argv);
    if (!out) return NULL;

    char *newline = strchr (out, '\n');
    if (newline) *newline = '\0';
    char *colon = strchr (out, ':');
    if (!colon) {
        g_free (out);
        return NULL;
    }
    *colon = '\0';
    g_strchomp (out);
    char *pkg = g_strdup (out);
    g_free (out);
    return pkg;
}

static char *
which_binary (const char *name)
{
    if (!name || !*name || name[0] == '/') return g_strdup (name);
    char *argv[] = { "which", (char *) name, NULL };
    return run_and_capture (argv);
}

static char *
resolve_deb_package (const char *desktop_path, const char *exec_binary, const char *hint)
{
    char *pkg = NULL;
    if (desktop_path) {
        pkg = dpkg_search (desktop_path);
        if (pkg) return pkg;
    }
    if (exec_binary) {
        char *bin_path = which_binary (exec_binary);
        if (bin_path) {
            pkg = dpkg_search (bin_path);
            g_free (bin_path);
            if (pkg) return pkg;
        }
    }
    if (hint && *hint) return g_strdup (hint);
    return NULL;
}

static gboolean
on_uninstall_finished_idle (gpointer user_data)
{
    UninstallResult *res = user_data;
    if (G_IS_OBJECT (res->manager)) {
        g_signal_emit (res->manager, manager_signals[SIGNAL_UNINSTALL_DONE], 0,
                       res->entry, res->success, res->detail_msg);
    }
    g_free (res->detail_msg);
    g_free (res);
    return G_SOURCE_REMOVE;
}

static void
post_result (UninstallTask *task, gboolean success, char *detail_msg)
{
    UninstallResult *res = g_new0 (UninstallResult, 1);
    res->manager    = task->manager;
    res->entry      = task->entry;
    res->success    = success;
    res->detail_msg = detail_msg;
    g_idle_add (on_uninstall_finished_idle, res);
}

static gpointer
uninstall_thread (gpointer user_data)
{
    UninstallTask *task = user_data;
    gboolean  success    = FALSE;
    char     *detail_msg = NULL;

    if (task->app_type == APP_TYPE_FLATPAK && task->flatpak_id) {
        char *cmd[] = { "flatpak", "uninstall", "--noninteractive", "-y", task->flatpak_id, NULL };
        gint st = 0; GError *err = NULL;
        gboolean ok = g_spawn_sync (NULL, cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &st, &err);
        if (ok && st == 0) {
            success = TRUE;
            detail_msg = g_strdup_printf ("تم إزالة برنامج Flatpak \"%s\" بنجاح.", task->flatpak_id);
        } else {
            detail_msg = g_strdup_printf ("فشل flatpak uninstall (كود %d).\nتأكد من أن تطبيق flatpak سليم.", st);
        }
        g_clear_error (&err);
        goto done;
    }

    if (task->app_type == APP_TYPE_SNAP && task->snap_name) {
        char *cmd[] = { "pkexec", "snap", "remove", task->snap_name, NULL };
        gint st = 0; GError *err = NULL;
        gboolean ok = g_spawn_sync (NULL, cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &st, &err);
        if (ok && st == 0) {
            success = TRUE;
            detail_msg = g_strdup_printf ("تم إزالة برنامج Snap \"%s\" بنجاح.", task->snap_name);
        } else {
            detail_msg = g_strdup_printf ("فشل snap remove (كود %d).", st);
        }
        g_clear_error (&err);
        goto done;
    }

    {
        char *pkg = resolve_deb_package (task->desktop_path, task->exec_binary, task->package_hint);
        if (!pkg) {
            detail_msg = g_strdup ("لم يمكن تحديد الحزمة باستخدام dpkg.\nتحقق من أن التطبيق مثبت بقاعدة .deb.");
            goto done;
        }

        char *apt_cmd[] = { "pkexec", "apt-get", "remove", "-y", "--auto-remove", pkg, NULL };
        gint st = 0; GError *err = NULL; gchar *sout = NULL, *serr = NULL;
        gboolean ok = g_spawn_sync (NULL, apt_cmd, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &sout, &serr, &st, &err);
        if (ok && st == 0) {
            success = TRUE;
            detail_msg = g_strdup_printf ("تم إزالة الحزمة \"%s\" بنجاح.", pkg);
        } else if (!ok) {
            detail_msg = g_strdup_printf ("تعذر تشغيل pkexec: %s", err ? err->message : "?");
        } else {
            detail_msg = g_strdup_printf ("فشل apt-get remove \"%s\" (كود %d).\nتحقق مخرجات dpkg: \"%s\"", pkg, st, (serr && *serr) ? serr : "خطأ غير معروف");
        }
        g_free (sout); g_free (serr); g_clear_error (&err); g_free (pkg);
    }

done:
    post_result (task, success, detail_msg);
    g_free (task->desktop_path);
    g_free (task->exec_binary);
    g_free (task->package_hint);
    g_free (task->flatpak_id);
    g_free (task->snap_name);
    g_free (task);
    return NULL;
}

void
vaxp_app_manager_uninstall_app_async (VaxpAppManager *self, AppEntry *entry)
{
    g_return_if_fail (VAXP_IS_APP_MANAGER (self));
    if (!entry) return;

    UninstallTask *task = g_new0 (UninstallTask, 1);
    task->manager       = self;
    task->entry         = entry;
    task->desktop_path  = g_strdup (entry->desktop_path);
    task->package_hint  = g_strdup (entry->package_name);

    const char *dpath = entry->desktop_path ? entry->desktop_path : "";

    if (strstr (dpath, "/flatpak/") || strstr (dpath, "flatpak")) {
        task->app_type = APP_TYPE_FLATPAK;
        char *base = g_path_get_basename (dpath);
        if (g_str_has_suffix (base, ".desktop")) base[strlen (base) - 8] = '\0';
        task->flatpak_id = base;
    } else if (strstr (dpath, "/snap/") || strstr (dpath, "snap")) {
        task->app_type = APP_TYPE_SNAP;
        char *base = g_path_get_basename (dpath);
        if (g_str_has_suffix (base, ".desktop")) base[strlen (base) - 8] = '\0';
        task->snap_name = base;
    } else {
        task->app_type = APP_TYPE_DEB;
        if (entry->exec && *entry->exec) {
            const char *end = entry->exec;
            while (*end && *end != ' ' && *end != '\t') end++;
            task->exec_binary = g_strndup (entry->exec, (gsize)(end - entry->exec));
        }
    }

    GThread *thr = g_thread_new ("uninstall-worker", uninstall_thread, task);
    g_thread_unref (thr);
}
