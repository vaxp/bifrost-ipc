#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ncurses.h>
#include <ctype.h>
#include <bifrost.h>
#include <bifrost_command.h>
#include "../core/launcher_ipc.h"

void to_lower_str(char *dest, const char *src) {
    while (*src) {
        *dest = tolower((unsigned char)*src);
        dest++; src++;
    }
    *dest = '\0';
}

int main(void) {
    bifrost_reader_t *reader = bifrost_join_as_reader("aethershell", "tui_launcher");
    if (!reader) {
        fprintf(stderr, "Failed to connect to AetherShell Daemon.\n");
        return 1;
    }

    SharedLauncherState *st = NULL;
    while (1) {
        size_t len;
        st = bifrost_reader_get(reader, "daemon", &len);
        if (st && len >= sizeof(SharedLauncherState) && st->magic == LAUNCHER_MAGIC) {
            break;
        }
        usleep(100000);
    }

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();
    
    init_pair(1, COLOR_WHITE, COLOR_BLUE);
    init_pair(2, COLOR_BLACK, COLOR_CYAN);
    init_pair(3, COLOR_WHITE, -1);

    char search_query[64] = "";
    int search_len = 0;
    int selected = 0;
    
    SharedAppEntry *filtered_apps[MAX_APPS];
    int filtered_count = 0;

    int running = 1;
    while (running) {
        filtered_count = 0;
        char lower_query[64];
        to_lower_str(lower_query, search_query);

        for (uint32_t i = 0; i < st->app_count; i++) {
            char lower_name[128];
            to_lower_str(lower_name, st->apps[i].name);
            if (search_len == 0 || strstr(lower_name, lower_query) != NULL) {
                filtered_apps[filtered_count++] = &st->apps[i];
            }
        }

        if (selected >= filtered_count && filtered_count > 0) {
            selected = filtered_count - 1;
        } else if (filtered_count == 0) {
            selected = 0;
        }

        clear();
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);

        attron(COLOR_PAIR(1) | A_BOLD);
        mvprintw(0, 0, " Search: %s", search_query);
        for (int i = 9 + search_len; i < max_x; i++) mvaddch(0, i, ' ');
        attroff(COLOR_PAIR(1) | A_BOLD);

        int list_max = max_y - 2;
        int start_idx = 0;
        if (selected >= list_max) {
            start_idx = selected - list_max + 1;
        }

        for (int i = 0; i < list_max && (start_idx + i) < filtered_count; i++) {
            int app_idx = start_idx + i;
            if (app_idx == selected) {
                attron(COLOR_PAIR(2) | A_BOLD);
                mvprintw(i + 2, 2, " > %-*s ", max_x - 6, filtered_apps[app_idx]->name);
                attroff(COLOR_PAIR(2) | A_BOLD);
            } else {
                attron(COLOR_PAIR(3));
                mvprintw(i + 2, 4, "%-*s", max_x - 4, filtered_apps[app_idx]->name);
                attroff(COLOR_PAIR(3));
            }
        }

        refresh();

        int ch = getch();
        if (ch == KEY_UP) {
            if (selected > 0) selected--;
        } else if (ch == KEY_DOWN) {
            if (selected < filtered_count - 1) selected++;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (search_len > 0) {
                search_query[--search_len] = '\0';
                selected = 0;
            }
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (filtered_count > 0) {
                SharedAppEntry *app = filtered_apps[selected];
                
                endwin();
                
                bifrost_writer_ref_t dref = bifrost_reader_resolve_writer(reader, "daemon");
                if (dref.id != 0 && bifrost_reader_writer_alive(reader, dref)) {
                    CmdLaunchPayload cmd_payload = {0};
                    strncpy(cmd_payload.exec, app->exec, sizeof(cmd_payload.exec) - 1);
                    strncpy(cmd_payload.desktop_path, app->desktop_path, sizeof(cmd_payload.desktop_path) - 1);
                    bifrost_reader_send_command(reader, dref, CMD_LAUNCH_APP, &cmd_payload, sizeof(cmd_payload), BIFROST_SEND_F_NO_REPLY, NULL);
                    usleep(50000);
                    bifrost_reader_leave(reader);
                    return 0;
                }

                pid_t pid = fork();
                if (pid == 0) {
                    setsid(); // Detach from terminal
                    close(0); // Close stdin
                    
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
                    exit(1);
                } else if (pid > 0) {
                    usleep(50000); // Give the child 50ms to detach before we give the terminal back
                }

                bifrost_reader_leave(reader);
                return 0;
            }
        } else if (ch == 27) { // ESC key
            running = 0;
        } else if (isprint(ch)) {
            if (search_len < 63) {
                search_query[search_len++] = (char)ch;
                search_query[search_len] = '\0';
                selected = 0;
            }
        }
    }

    endwin();
    bifrost_reader_leave(reader);
    return 0;
}
