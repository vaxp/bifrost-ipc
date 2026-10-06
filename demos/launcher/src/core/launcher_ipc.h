#ifndef LAUNCHER_IPC_H
#define LAUNCHER_IPC_H

#include <stdint.h>

#define LAUNCHER_MAGIC 0x4C4E4352 // "LNCR"
#define MAX_APPS 512
#define MAX_ICON_WIDTH 96
#define MAX_ICON_HEIGHT 96
#define MAX_ICON_BYTES (MAX_ICON_WIDTH * MAX_ICON_HEIGHT * 4) // RGBA

typedef struct {
    uint32_t index;
    char name[64];
    char exec[256];
    char desktop_path[256];
    char package_name[64];
    uint8_t has_icon;
    uint32_t icon_width;
    uint32_t icon_height;
    uint32_t icon_rowstride;
    uint32_t icon_channels;
    uint8_t icon_data[MAX_ICON_BYTES]; 
} SharedAppEntry;

typedef struct {
    uint32_t magic;
    uint32_t app_count;
    SharedAppEntry apps[MAX_APPS];
    uint64_t update_counter;
    uint64_t timestamp_ns;
} SharedLauncherState;

// Commands from UI to Daemon
typedef enum {
    CMD_LAUNCH_APP = 1,
    CMD_UNINSTALL_APP = 2,
    CMD_CREATE_SHORTCUT = 3,
    CMD_REFRESH_APPS = 4
} LauncherCmd;

/* Specific payload structures for Bifrost Command Layer */
typedef struct {
    char desktop_path[256];
    char exec[256];
} CmdLaunchPayload;

typedef struct {
    char desktop_path[256];
    char package_name[64];
    char exec[256];
} CmdUninstallPayload;

typedef struct {
    char desktop_path[256];
} CmdShortcutPayload;

/* Legacy union command structure for compatibility */
typedef struct {
    uint8_t cmd;
    uint8_t _pad[3];
    union {
        struct { char desktop_path[256]; char exec[256]; } launch;
        struct { char desktop_path[256]; char package_name[64]; char exec[256]; } uninstall;
        struct { char desktop_path[256]; } shortcut;
    } data;
} LauncherCommand;

#endif // LAUNCHER_IPC_H
