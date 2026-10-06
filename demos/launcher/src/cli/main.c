#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <bifrost.h>
#include <bifrost_command.h>
#include "../core/launcher_ipc.h"

int main(void) {
    printf("Connecting to AetherShell Daemon...\n");
    bifrost_reader_t *reader = bifrost_join_as_reader("aethershell", "cli_launcher");
    if (!reader) {
        fprintf(stderr, "Failed to connect to daemon. Is it running?\n");
        return 1;
    }

    SharedLauncherState *st = NULL;
    while (1) {
        size_t len;
        st = bifrost_reader_get(reader, "daemon", &len);
        if (st && len >= sizeof(SharedLauncherState) && st->magic == LAUNCHER_MAGIC) {
            break;
        }
        usleep(100000); // 100ms
    }

    printf("\n=== Available Applications ===\n");
    for (uint32_t i = 0; i < st->app_count; i++) {
        SharedAppEntry *app = &st->apps[i];
        printf("[%3u] %s\n", app->index, app->name);
    }
    printf("==============================\n");

    while (1) {
        printf("\nEnter App ID to launch (or 'q' to quit): ");
        char buf[64];
        if (!fgets(buf, sizeof(buf), stdin)) {
            break;
        }

        if (buf[0] == 'q' || buf[0] == 'Q') {
            break;
        }

        // Simple validation
        if (buf[0] < '0' || buf[0] > '9') {
            continue;
        }

        int id = atoi(buf);
        if (id < 0 || id >= (int)st->app_count) {
            printf("Invalid ID.\n");
            continue;
        }

        SharedAppEntry *app = &st->apps[id];
        printf("Launching: %s\n", app->name);

        bifrost_writer_ref_t dref = bifrost_reader_resolve_writer(reader, "daemon");
        if (dref.id != 0 && bifrost_reader_writer_alive(reader, dref)) {
            CmdLaunchPayload cmd_payload = {0};
            strncpy(cmd_payload.exec, app->exec, sizeof(cmd_payload.exec) - 1);
            strncpy(cmd_payload.desktop_path, app->desktop_path, sizeof(cmd_payload.desktop_path) - 1);
            bifrost_reader_send_command(reader, dref, CMD_LAUNCH_APP, &cmd_payload, sizeof(cmd_payload), BIFROST_SEND_F_NO_REPLY, NULL);
            usleep(50000);
            break;
        }

        pid_t pid = fork();
        if (pid == 0) {
            setsid(); // Detach
            close(0); // Close stdin
            // Child process
            // Tokenize exec command simply
            char exec_copy[256];
            strncpy(exec_copy, app->exec, 255);
            exec_copy[255] = '\0';
            
            char *args[64];
            int argc = 0;
            char *token = strtok(exec_copy, " ");
            while (token && argc < 63) {
                // Ignore standard desktop file placeholders
                if (token[0] == '%' && (token[1] == 'U' || token[1] == 'F' || token[1] == 'u' || token[1] == 'f')) {
                    token = strtok(NULL, " ");
                    continue;
                }
                args[argc++] = token;
                token = strtok(NULL, " ");
            }
            args[argc] = NULL;
            
            if (argc > 0) {
                if (!freopen("/dev/null", "r", stdin)) {}
                if (!freopen("/dev/null", "w", stdout)) {}
                if (!freopen("/dev/null", "w", stderr)) {}
                execvp(args[0], args);
            }
            exit(1); // Exit child if exec fails
        } else if (pid > 0) {
            usleep(50000); 
            break; 
        }
    }

    bifrost_reader_leave(reader);
    return 0;
}
