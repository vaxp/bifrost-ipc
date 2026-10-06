#include "client_ipc.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bifrost_pool_t *g_pool = NULL;
static bifrost_reader_t *g_reader = NULL;
static bifrost_writer_ref_t g_daemon_ref = {0, 0};
static SharedLauncherState *g_current_state = NULL;

uint64_t g_last_update_counter = 0;

SharedLauncherState *client_ipc_get_state(void) {
    return g_current_state;
}

double g_ipc_poll_latency_ms = 0.0;
double g_ipc_read_mbps = 0.0;
int g_ipc_fps = 0;

static int frames_this_sec = 0;
static size_t bytes_this_sec = 0;
static uint64_t last_sec_time = 0;

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static bool ensure_daemon_ref(void) {
    if (!g_reader) return false;
    if (g_daemon_ref.id != 0 && bifrost_reader_writer_alive(g_reader, g_daemon_ref)) {
        return true;
    }
    g_daemon_ref = bifrost_reader_resolve_writer(g_reader, "daemon");
    return (g_daemon_ref.id != 0);
}

bool client_ipc_init(void) {
    g_pool = bifrost_open_pool("aethershell");
    if (!g_pool) {
        fprintf(stderr, "[Launcher] Failed to connect to aethershell daemon pool!\n");
        return false;
    }

    char reader_name[64];
    snprintf(reader_name, sizeof(reader_name), "launcher_ui_%d", (int)getpid());

    g_reader = bifrost_join_as_reader("aethershell", reader_name);
    if (!g_reader) {
        fprintf(stderr, "[Launcher] Failed to join as reader '%s'!\n", reader_name);
        bifrost_close_pool(g_pool);
        g_pool = NULL;
        return false;
    }

    g_daemon_ref = bifrost_reader_resolve_writer(g_reader, "daemon");
    printf("[Launcher] Connected to daemon via Bifrost zero-copy (Command Layer active)!\n");
    return true;
}

void client_ipc_cleanup(void) {
    if (g_reader) {
        bifrost_reader_leave(g_reader);
        g_reader = NULL;
    }
    if (g_pool) {
        bifrost_close_pool(g_pool);
        g_pool = NULL;
    }
    g_daemon_ref.id = 0;
    g_daemon_ref.gen = 0;
}

bool client_ipc_poll_state(void) {
    if (!g_reader) return false;
    
    uint64_t t_start = get_time_us();

    uint64_t ver = bifrost_reader_get_version(g_reader, "daemon");
    size_t len = 0;
    bool changed = false;
    
    if (ver > g_last_update_counter) {
        SharedLauncherState *st = bifrost_reader_get(g_reader, "daemon", &len);
        if (st && len >= sizeof(SharedLauncherState) && st->magic == LAUNCHER_MAGIC) {
            g_current_state = st;
            g_last_update_counter = ver;
            changed = true;
        }
    }
    
    uint64_t t_end = get_time_us();
    g_ipc_poll_latency_ms = (t_end - t_start) / 1000.0;
    
    frames_this_sec++;
    bytes_this_sec += (changed ? len : sizeof(SharedLauncherState));
    
    if (t_end - last_sec_time >= 1000000) {
        g_ipc_fps = frames_this_sec;
        g_ipc_read_mbps = (bytes_this_sec / 1024.0 / 1024.0);
        frames_this_sec = 0;
        bytes_this_sec = 0;
        last_sec_time = t_end;
    }

    return changed;
}

void client_ipc_send_launch(const char *exec) {
    if (!exec || !ensure_daemon_ref()) return;

    CmdLaunchPayload args;
    memset(&args, 0, sizeof(args));
    strncpy(args.exec, exec, sizeof(args.exec) - 1);

    bifrost_error_t err = bifrost_reader_send_command(
        g_reader,
        g_daemon_ref,
        CMD_LAUNCH_APP,
        &args,
        sizeof(args),
        BIFROST_SEND_F_NO_REPLY,
        NULL
    );

    if (err != BIFROST_OK) {
        fprintf(stderr, "[Launcher] Failed to send launch command: %s\n", bifrost_error_string(err));
    }
}

void client_ipc_send_uninstall(const char *desktop_path, const char *package_name, const char *exec) {
    if (!desktop_path || !ensure_daemon_ref()) return;

    CmdUninstallPayload args;
    memset(&args, 0, sizeof(args));
    strncpy(args.desktop_path, desktop_path, sizeof(args.desktop_path) - 1);
    if (package_name) strncpy(args.package_name, package_name, sizeof(args.package_name) - 1);
    if (exec) strncpy(args.exec, exec, sizeof(args.exec) - 1);

    bifrost_error_t err = bifrost_reader_send_command(
        g_reader,
        g_daemon_ref,
        CMD_UNINSTALL_APP,
        &args,
        sizeof(args),
        BIFROST_SEND_F_NO_REPLY,
        NULL
    );

    if (err != BIFROST_OK) {
        fprintf(stderr, "[Launcher] Failed to send uninstall command: %s\n", bifrost_error_string(err));
    }
}

void client_ipc_send_shortcut(const char *desktop_path) {
    if (!desktop_path || !ensure_daemon_ref()) return;

    CmdShortcutPayload args;
    memset(&args, 0, sizeof(args));
    strncpy(args.desktop_path, desktop_path, sizeof(args.desktop_path) - 1);

    bifrost_error_t err = bifrost_reader_send_command(
        g_reader,
        g_daemon_ref,
        CMD_CREATE_SHORTCUT,
        &args,
        sizeof(args),
        BIFROST_SEND_F_NO_REPLY,
        NULL
    );

    if (err != BIFROST_OK) {
        fprintf(stderr, "[Launcher] Failed to send shortcut command: %s\n", bifrost_error_string(err));
    }
}

void client_ipc_send_refresh(void) {
    if (!ensure_daemon_ref()) return;

    bifrost_error_t err = bifrost_reader_send_command(
        g_reader,
        g_daemon_ref,
        CMD_REFRESH_APPS,
        NULL,
        0,
        BIFROST_SEND_F_NO_REPLY,
        NULL
    );

    if (err != BIFROST_OK) {
        fprintf(stderr, "[Launcher] Failed to send refresh command: %s\n", bifrost_error_string(err));
    }
}
