#include "desktop_reader.h"
#include "app_entry.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static void
parse_desktop_file (const char *path, GPtrArray *out)
{
    GKeyFile *kf = g_key_file_new ();
    GError   *err = NULL;

    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err)) {
        g_error_free (err);
        g_key_file_free (kf);
        return;
    }

    /* Must be Type=Application */
    char *type = g_key_file_get_string (kf, "Desktop Entry", "Type", NULL);
    if (!type || g_strcmp0 (type, "Application") != 0) {
        g_free (type);
        g_key_file_free (kf);
        return;
    }
    g_free (type);

    AppEntry *e = app_entry_new ();

    e->no_display = g_key_file_get_boolean (kf, "Desktop Entry",
                                            "NoDisplay", NULL);
    if (e->no_display) {
        app_entry_free (e);
        g_key_file_free (kf);
        return;
    }

    /* Name: prefer localized */
    e->name = g_key_file_get_locale_string (kf, "Desktop Entry",
                                            "Name", NULL, NULL);
    if (!e->name)
        e->name = g_key_file_get_string (kf, "Desktop Entry", "Name", NULL);

    /* Exec */
    char *exec_raw = g_key_file_get_string (kf, "Desktop Entry", "Exec", NULL);
    e->exec = app_entry_clean_exec (exec_raw);
    g_free (exec_raw);

    /* Icon */
    e->icon_name = g_key_file_get_string (kf, "Desktop Entry", "Icon", NULL);

    /* Comment */
    e->comment = g_key_file_get_locale_string (kf, "Desktop Entry",
                                               "Comment", NULL, NULL);

    /* Categories */
    e->categories = g_key_file_get_string (kf, "Desktop Entry",
                                           "Categories", NULL);

    /* Store the absolute path for shortcuts/uninstall */
    e->desktop_path = g_strdup (path);

    /* Package name: prefer X-AppStream-Package, fallback to filename stem */
    char *pkg = g_key_file_get_string (kf, "Desktop Entry", "X-AppStream-Package", NULL);
    if (pkg && *pkg) {
        e->package_name = pkg;
    } else {
        g_free (pkg);
        char *basename  = g_path_get_basename (path);
        /* strip .desktop suffix */
        if (g_str_has_suffix (basename, ".desktop"))
            basename[strlen (basename) - 8] = '\0';
        e->package_name = basename;
    }

    if (e->name && e->exec) {
        g_ptr_array_add (out, e);
    } else {
        app_entry_free (e);
    }

    g_key_file_free (kf);
}

static void
scan_directory (const char *dir_path, GPtrArray *out)
{
    GDir *dir = g_dir_open (dir_path, 0, NULL);
    if (!dir) return;

    const char *filename;
    while ((filename = g_dir_read_name (dir))) {
        if (!g_str_has_suffix (filename, ".desktop")) continue;

        char *full_path = g_build_filename (dir_path, filename, NULL);
        parse_desktop_file (full_path, out);
        g_free (full_path);
    }

    g_dir_close (dir);
}

static int
app_entry_compare (gconstpointer a, gconstpointer b)
{
    const AppEntry *ea = *(const AppEntry **) a;
    const AppEntry *eb = *(const AppEntry **) b;

    if (!ea->name) return 1;
    if (!eb->name) return -1;

    return g_utf8_collate (ea->name, eb->name);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

GPtrArray *
desktop_reader_load_apps (void)
{
    GPtrArray *apps = g_ptr_array_new_with_free_func (
        (GDestroyNotify) app_entry_free);

    /* System directories */
    scan_directory ("/usr/share/applications",       apps);
    scan_directory ("/usr/local/share/applications", apps);

    /* User directory */
    const char *home = g_get_home_dir ();
    char *user_dir = g_build_filename (home,
                                       ".local", "share", "applications",
                                       NULL);
    scan_directory (user_dir, apps);
    g_free (user_dir);

    /* Sort alphabetically */
    g_ptr_array_sort (apps, app_entry_compare);

    return apps;
}
