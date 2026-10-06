#pragma once

#include <gtk/gtk.h>
#include <glib.h>

#define ICON_LOAD_SIZE   96   /* pixels — matches design spec */
#define ICON_CACHE_MAX  256   /* LRU cache entries            */

/**
 * IconLoader - Thread-safe icon resolver with LRU cache.
 * Singleton: call icon_loader_get() to obtain the global instance.
 */
typedef struct _IconLoader IconLoader;

IconLoader  *icon_loader_get          (void);
void         icon_loader_destroy      (void);

/**
 * Returns a referenced GdkPixbuf* (96×96) for the given icon name.
 * Looks up LRU cache first, then resolves via GtkIconTheme.
 * Returns NULL if not found.
 * Thread-safe.
 */
GdkPixbuf   *icon_loader_load         (IconLoader  *loader,
                                       const char  *icon_name);

/**
 * Async variant — calls @callback on the GLib main thread
 * with the resolved GdkPixbuf* (may be NULL).
 */
typedef void (*IconReadyCallback) (GdkPixbuf *pixbuf, gpointer user_data);

void         icon_loader_load_async   (IconLoader       *loader,
                                       const char       *icon_name,
                                       IconReadyCallback callback,
                                       gpointer          user_data);
