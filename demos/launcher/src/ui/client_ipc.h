#ifndef CLIENT_IPC_H
#define CLIENT_IPC_H

#include <bifrost.h>
#include <bifrost_command.h>
#include <stdbool.h>
#include "../core/app_entry.h"
#include "../core/launcher_ipc.h"

#include <time.h>

extern uint64_t g_last_update_counter;

SharedLauncherState *client_ipc_get_state(void);

extern double g_ipc_poll_latency_ms;
extern double g_ipc_read_mbps;
extern int g_ipc_fps;

bool client_ipc_init(void);
void client_ipc_cleanup(void);

// Read state from shared memory
bool client_ipc_poll_state(void);

// Commands
void client_ipc_send_launch(const char *exec);
void client_ipc_send_uninstall(const char *desktop_path, const char *package_name, const char *exec);
void client_ipc_send_shortcut(const char *desktop_path);
void client_ipc_send_refresh(void);

#endif // CLIENT_IPC_H
