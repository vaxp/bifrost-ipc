#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "../core/app_entry.h"
#include "../core/desktop_reader.h"
#include "../core/icon_loader.h"
#include "../core/app_manager.h"
#include "../core/launcher_ipc.h"
#include <bifrost.h>
#include <bifrost_command.h>

static bifrost_pool_t *g_pool = NULL;
static bifrost_writer_t *g_writer = NULL;
static VaxpAppManager *g_app_manager = NULL;
static uint64_t g_update_counter = 0;

static void
sync_apps_to_shared_memory (void)
{
    if (!g_writer) return;

    GPtrArray *apps = vaxp_app_manager_get_apps (g_app_manager);
    if (!apps) return;

    SharedLauncherState *st = bifrost_writer_alloc(g_writer, sizeof(SharedLauncherState));
    if (!st) return;

    st->magic = LAUNCHER_MAGIC;
    st->app_count = 0;

    for (guint i = 0; i < apps->len && i < MAX_APPS; i++) {
        AppEntry *app = g_ptr_array_index (apps, i);
        SharedAppEntry *sa = &st->apps[st->app_count];
        
        sa->index = i;
        strncpy(sa->name, app->name ? app->name : "", 63);
        strncpy(sa->exec, app->exec ? app->exec : "", 255);
        strncpy(sa->desktop_path, app->desktop_path ? app->desktop_path : "", 255);
        strncpy(sa->package_name, app->package_name ? app->package_name : "", 63);

        sa->has_icon = 0;

        // Load icon directly into Shared Memory
        if (app->icon_name) {
            IconLoader *loader = icon_loader_get();
            GdkPixbuf *pixbuf = icon_loader_load (loader, app->icon_name);
            if (pixbuf) {
                int width = gdk_pixbuf_get_width (pixbuf);
                int height = gdk_pixbuf_get_height (pixbuf);
                int rowstride = gdk_pixbuf_get_rowstride (pixbuf);
                int channels = gdk_pixbuf_get_n_channels (pixbuf);
                const guchar *pixels = gdk_pixbuf_read_pixels (pixbuf);
                
                if (width <= MAX_ICON_WIDTH && height <= MAX_ICON_HEIGHT) {
                    sa->icon_width = width;
                    sa->icon_height = height;
                    sa->icon_rowstride = width * 4;
                    sa->icon_channels = 4;
                    
                    int bytes_to_copy = height * (width * 4);
                    if (bytes_to_copy <= MAX_ICON_BYTES) {
                        uint32_t *dst = (uint32_t *)sa->icon_data;
                        for (int y = 0; y < height; y++) {
                            const guchar *src_row = pixels + y * rowstride;
                            uint32_t *dst_row = dst + y * width;
                            for (int x = 0; x < width; x++) {
                                const guchar *p = src_row + x * channels;
                                uint32_t r = p[0];
                                uint32_t g = p[1];
                                uint32_t b = p[2];
                                uint32_t a = (channels == 4) ? p[3] : 255;
                                
                                /* Pre-multiply alpha */
                                if (a != 255 && a != 0) {
                                    r = (r * a + 127) / 255;
                                    g = (g * a + 127) / 255;
                                    b = (b * a + 127) / 255;
                                } else if (a == 0) {
                                    r = g = b = 0;
                                }
                                
                                /* Cairo ARGB32 native endian */
                                dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
                            }
                        }
                        sa->has_icon = 1;
                    }
                }
                g_object_unref (pixbuf);
            }
        }

        st->app_count++;
    }

    st->update_counter = ++g_update_counter;
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    st->timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    bifrost_writer_publish(g_writer);
    printf("[Daemon] Synced %u apps to shared memory (Update %lu)\n", st->app_count, g_update_counter);
}

static void
on_apps_changed (VaxpAppManager *manager, gpointer user_data)
{
    (void) manager;
    (void) user_data;
    sync_apps_to_shared_memory ();
}

static void
on_uninstall_done (VaxpAppManager *manager, gpointer entry_ptr, gboolean success, const char *msg, gpointer user_data)
{
    (void) manager;
    (void) user_data;
    AppEntry *app = (AppEntry *) entry_ptr;
    printf("[Daemon] Uninstall done for '%s': success=%d, msg='%s'\n", app->name, success, msg);
    // Ideally we could send a notification to UI, but UI polling state is enough for now.
}

static gboolean
poll_commands (gpointer user_data)
{
    (void) user_data;
    if (!g_writer) return G_SOURCE_CONTINUE;

    while (1) {
        const bifrost_cmd_request_t *req = bifrost_writer_poll_command(g_writer);
        if (!req) break;

        int32_t status = BIFROST_OK;

        switch (req->cmd_id) {
            case CMD_LAUNCH_APP: {
                const char *exec_str = NULL;
                if (req->arg_len == sizeof(LauncherCommand)) {
                    const LauncherCommand *lcmd = (const LauncherCommand *)req->args;
                    exec_str = lcmd->data.launch.exec;
                } else if (req->arg_len >= sizeof(CmdLaunchPayload)) {
                    const CmdLaunchPayload *args = (const CmdLaunchPayload *)req->args;
                    exec_str = args->exec;
                }
                if (exec_str && exec_str[0] != '\0') {
                    printf("[Daemon] Client command: LAUNCH '%s'\n", exec_str);
                    AppEntry temp = {0};
                    temp.exec = (char *)exec_str;
                    vaxp_app_manager_launch_app(g_app_manager, &temp);
                } else {
                    status = BIFROST_ERR_INVALID;
                }
                break;
            }
            case CMD_UNINSTALL_APP: {
                const char *desktop_path = NULL;
                const char *package_name = NULL;
                const char *exec_str = NULL;
                if (req->arg_len == sizeof(LauncherCommand)) {
                    const LauncherCommand *lcmd = (const LauncherCommand *)req->args;
                    desktop_path = lcmd->data.uninstall.desktop_path;
                    package_name = lcmd->data.uninstall.package_name;
                    exec_str = lcmd->data.uninstall.exec;
                } else if (req->arg_len >= sizeof(CmdUninstallPayload)) {
                    const CmdUninstallPayload *args = (const CmdUninstallPayload *)req->args;
                    desktop_path = args->desktop_path;
                    package_name = args->package_name;
                    exec_str = args->exec;
                }
                if (desktop_path && desktop_path[0] != '\0') {
                    printf("[Daemon] Client command: UNINSTALL '%s'\n", desktop_path);
                    AppEntry temp = {0};
                    temp.desktop_path = (char *)desktop_path;
                    temp.package_name = (char *)package_name;
                    temp.exec = (char *)exec_str;
                    vaxp_app_manager_uninstall_app_async(g_app_manager, &temp);
                } else {
                    status = BIFROST_ERR_INVALID;
                }
                break;
            }
            case CMD_CREATE_SHORTCUT: {
                const char *desktop_path = NULL;
                if (req->arg_len == sizeof(LauncherCommand)) {
                    const LauncherCommand *lcmd = (const LauncherCommand *)req->args;
                    desktop_path = lcmd->data.shortcut.desktop_path;
                } else if (req->arg_len >= sizeof(CmdShortcutPayload)) {
                    const CmdShortcutPayload *args = (const CmdShortcutPayload *)req->args;
                    desktop_path = args->desktop_path;
                }
                if (desktop_path && desktop_path[0] != '\0') {
                    printf("[Daemon] Client command: SHORTCUT '%s'\n", desktop_path);
                    AppEntry temp = {0};
                    temp.desktop_path = (char *)desktop_path;
                    vaxp_app_manager_create_shortcut(g_app_manager, &temp);
                } else {
                    status = BIFROST_ERR_INVALID;
                }
                break;
            }
            case CMD_REFRESH_APPS: {
                printf("[Daemon] Client command: REFRESH\n");
                sync_apps_to_shared_memory();
                break;
            }
            default:
                printf("[Daemon] Unknown command ID: %u\n", req->cmd_id);
                status = BIFROST_ERR_NOTFOUND;
                break;
        }

        bifrost_writer_command_done(g_writer, req, status, NULL, 0);
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    printf("[Daemon] Starting AetherShell Daemon...\n");

    bifrost_pool_config_t pool_config;
    bifrost_config_init(&pool_config);
    pool_config.initial_size = 128 * 1024 * 1024; // 128 MB Arena
    pool_config.max_size = 512 * 1024 * 1024;
    pool_config.cmd_inbox_depth = 64;
    pool_config.cmd_reply_depth = 32;
    pool_config.cmd_max_args = 1024;
    pool_config.cmd_max_reply = 256;

    g_pool = bifrost_create_pool("aethershell", &pool_config);
    if (!g_pool) {
        // If a stale shared memory segment exists from an ungraceful shutdown, unlink and retry
        shm_unlink("/bifrost_aethershell");
        g_pool = bifrost_create_pool("aethershell", &pool_config);
    }
    if (!g_pool) {
        fprintf(stderr, "[Daemon] Failed to create Bifrost shared memory pool!\n");
        return 1;
    }
    printf("[Daemon] Created Bifrost memory pool 'aethershell'\n");

    g_writer = bifrost_join_as_writer("aethershell", "daemon");
    if (!g_writer) {
        fprintf(stderr, "[Daemon] Failed to join as writer 'daemon'!\n");
        bifrost_destroy_pool(g_pool);
        return 1;
    }

    // Register commands in Bifrost command catalog
    bifrost_cmd_desc_t desc_launch = {
        .id = CMD_LAUNCH_APP,
        .flags = 0,
        .arg_min = sizeof(CmdLaunchPayload),
        .arg_max = 1024,
        .reply_max = 0,
        .name = "launch",
        .desc = "Launch application by exec path"
    };
    bifrost_writer_register_command(g_writer, &desc_launch);

    bifrost_cmd_desc_t desc_uninstall = {
        .id = CMD_UNINSTALL_APP,
        .flags = 0,
        .arg_min = sizeof(CmdShortcutPayload),
        .arg_max = 1024,
        .reply_max = 0,
        .name = "uninstall",
        .desc = "Uninstall application"
    };
    bifrost_writer_register_command(g_writer, &desc_uninstall);

    bifrost_cmd_desc_t desc_shortcut = {
        .id = CMD_CREATE_SHORTCUT,
        .flags = 0,
        .arg_min = sizeof(CmdShortcutPayload),
        .arg_max = 1024,
        .reply_max = 0,
        .name = "shortcut",
        .desc = "Create desktop shortcut"
    };
    bifrost_writer_register_command(g_writer, &desc_shortcut);

    bifrost_cmd_desc_t desc_refresh = {
        .id = CMD_REFRESH_APPS,
        .flags = 0,
        .arg_min = 0,
        .arg_max = 0,
        .reply_max = 0,
        .name = "refresh",
        .desc = "Refresh installed application list"
    };
    bifrost_writer_register_command(g_writer, &desc_refresh);

    g_app_manager = vaxp_app_manager_get_default();
    g_signal_connect(g_app_manager, "apps-changed", G_CALLBACK(on_apps_changed), NULL);
    g_signal_connect(g_app_manager, "uninstall-done", G_CALLBACK(on_uninstall_done), NULL);

    // Initial sync
    sync_apps_to_shared_memory();

    // Poll for commands (every 20ms)
    g_timeout_add(20, poll_commands, NULL);

    gtk_main();

    if (g_writer) bifrost_writer_leave(g_writer);
    if (g_pool) bifrost_destroy_pool(g_pool);
    return 0;
}
