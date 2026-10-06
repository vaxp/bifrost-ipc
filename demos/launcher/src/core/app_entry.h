#pragma once

#include <gtk/gtk.h>
#include <stdbool.h>

/**
 * AppEntry - Data model for a single application.
 * Pure data struct — no GTK rendering logic.
 */
typedef struct {
    char       *name;        /* Display name */
    char       *exec;        /* Exec command (cleaned) */
    char       *icon_name;   /* Icon name or absolute path */
    char       *categories;  /* Desktop categories string */
    char       *comment;     /* Short description */
    char       *desktop_path;/* Absolute path to the original .desktop file */
    char       *package_name;/* System package name (for apt/dpkg uninstall) */
    bool        no_display;  /* Hidden from launcher */
    cairo_surface_t *surface; /* Loaded icon surface (NULL until loaded) */
} AppEntry;

AppEntry *app_entry_new  (void);
void      app_entry_free (AppEntry *entry);

/* Utility: strip %f, %u, %F, %U, etc. from Exec field */
char     *app_entry_clean_exec (const char *exec_raw);
