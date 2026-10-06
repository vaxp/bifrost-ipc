#include "icon_loader.h"
#include <string.h>
#include <pthread.h>

/* -------------------------------------------------------------------------
 * LRU Cache node
 * ------------------------------------------------------------------------- */

typedef struct _CacheNode {
    char             *key;
    GdkPixbuf        *pixbuf;
    struct _CacheNode *prev;
    struct _CacheNode *next;
} CacheNode;

/* -------------------------------------------------------------------------
 * IconLoader struct
 * ------------------------------------------------------------------------- */

struct _IconLoader {
    GHashTable      *map;    /* key -> CacheNode* (main thread only) */
    CacheNode       *head;   /* MRU end */
    CacheNode       *tail;   /* LRU end */
    int              count;
    GtkIconTheme    *theme;  /* GTK objects — main thread only */
    GThreadPool     *pool;
    pthread_mutex_t  path_lock;  /* protects async task path-resolve queue */
};

/* Async task: pass file path (not GtkIconTheme) to the thread */
typedef struct {
    IconLoader        *loader;
    char              *resolved_path;  /* absolute file path — safe for threads */
    char              *icon_name;
    IconReadyCallback  callback;
    gpointer           user_data;
} AsyncTask;

typedef struct {
    IconLoader        *loader;
    char              *icon_name;
    IconReadyCallback  callback;
    GdkPixbuf         *pixbuf;
    gpointer           user_data;
} IdleData;

/* -------------------------------------------------------------------------
 * Global singleton + g_once for thread-safe lazy init
 * ------------------------------------------------------------------------- */

static IconLoader *g_loader      = NULL;
static GOnce      g_loader_once  = G_ONCE_INIT;

/* -------------------------------------------------------------------------
 * LRU internals  (all called under main-thread or with appropriate locking)
 * ------------------------------------------------------------------------- */

static void
lru_detach (IconLoader *l, CacheNode *node)
{
    if (node->prev) node->prev->next = node->next;
    else            l->head          = node->next;
    if (node->next) node->next->prev = node->prev;
    else            l->tail          = node->prev;
    node->prev = node->next = NULL;
}

static void
lru_push_front (IconLoader *l, CacheNode *node)
{
    node->next = l->head;
    node->prev = NULL;
    if (l->head) l->head->prev = node;
    l->head = node;
    if (!l->tail) l->tail = node;
}

static void
lru_evict_lru (IconLoader *l)
{
    if (!l->tail) return;
    CacheNode *old = l->tail;
    lru_detach (l, old);
    g_hash_table_remove (l->map, old->key);
    l->count--;
    g_free (old->key);
    if (old->pixbuf) g_object_unref (old->pixbuf);
    g_free (old);
}

/* Returns new ref or NULL — must be called on main thread */
static GdkPixbuf *
lru_get (IconLoader *l, const char *key)
{
    CacheNode *node = g_hash_table_lookup (l->map, key);
    if (!node) return NULL;
    lru_detach (l, node);
    lru_push_front (l, node);
    return node->pixbuf ? g_object_ref (node->pixbuf) : NULL;
}

/* Stores a ref — must be called on main thread */
static void
lru_put (IconLoader *l, const char *key, GdkPixbuf *pixbuf)
{
    CacheNode *existing = g_hash_table_lookup (l->map, key);
    if (existing) {
        if (existing->pixbuf) g_object_unref (existing->pixbuf);
        existing->pixbuf = pixbuf ? g_object_ref (pixbuf) : NULL;
        lru_detach (l, existing);
        lru_push_front (l, existing);
        return;
    }
    if (l->count >= ICON_CACHE_MAX)
        lru_evict_lru (l);

    CacheNode *node  = g_new0 (CacheNode, 1);
    node->key        = g_strdup (key);
    node->pixbuf     = pixbuf ? g_object_ref (pixbuf) : NULL;
    g_hash_table_insert (l->map, node->key, node);
    lru_push_front (l, node);
    l->count++;
}

/* -------------------------------------------------------------------------
 * Resolve icon name -> absolute file path (main-thread only, uses GtkIconTheme)
 * ------------------------------------------------------------------------- */

static char *
resolve_icon_path (IconLoader *l, const char *icon_name)
{
    if (!icon_name || *icon_name == '\0') return NULL;

    if (g_path_is_absolute (icon_name) && g_file_test (icon_name, G_FILE_TEST_EXISTS))
        return g_strdup (icon_name);

    GtkIconInfo *info = gtk_icon_theme_lookup_icon (
        l->theme, icon_name, ICON_LOAD_SIZE, GTK_ICON_LOOKUP_FORCE_SIZE);

    if (!info) {
        /* Fallback */
        info = gtk_icon_theme_lookup_icon (
            l->theme, "application-x-executable", ICON_LOAD_SIZE,
            GTK_ICON_LOOKUP_FORCE_SIZE);
    }

    if (!info) return NULL;

    const char *path = gtk_icon_info_get_filename (info);
    char *result = path ? g_strdup (path) : NULL;
    g_object_unref (info);
    return result;
}

/* -------------------------------------------------------------------------
 * Thread pool worker — only does file I/O, no GTK calls
 * ------------------------------------------------------------------------- */

static gboolean
idle_deliver (gpointer data)
{
    IdleData *id = data;
    if (id->pixbuf) {
        lru_put (id->loader, id->icon_name, id->pixbuf);
    }
    id->callback (id->pixbuf, id->user_data);
    if (id->pixbuf) g_object_unref (id->pixbuf);
    g_free (id->icon_name);
    g_free (id);
    return G_SOURCE_REMOVE;
}

static void
thread_pool_func (gpointer task_data, gpointer user_data)
{
    (void) user_data;
    AsyncTask *task = task_data;
    GdkPixbuf *pb   = NULL;

    if (task->resolved_path) {
        GError *err = NULL;
        pb = gdk_pixbuf_new_from_file_at_scale (
            task->resolved_path,
            ICON_LOAD_SIZE, ICON_LOAD_SIZE,
            TRUE, &err);
        if (err) { g_error_free (err); pb = NULL; }
    }

    IdleData *id  = g_new0 (IdleData, 1);
    id->loader    = task->loader;
    id->icon_name = task->icon_name;
    id->callback  = task->callback;
    id->pixbuf    = pb;  /* hand ref to idle */
    id->user_data = task->user_data;

    g_idle_add (idle_deliver, id);

    g_free (task->resolved_path);
    g_free (task);
}

/* -------------------------------------------------------------------------
 * Singleton initializer (called once via g_once)
 * ------------------------------------------------------------------------- */

static gpointer
icon_loader_init_once (gpointer arg)
{
    (void) arg;

    IconLoader *l = g_new0 (IconLoader, 1);
    l->map   = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, NULL);
    l->theme = gtk_icon_theme_get_default ();
    pthread_mutex_init (&l->path_lock, NULL);

    GError *err = NULL;
    l->pool = g_thread_pool_new (thread_pool_func, NULL, 4, FALSE, &err);
    if (err) {
        g_warning ("IconLoader: thread pool: %s", err->message);
        g_error_free (err);
    }

    g_loader = l;
    return l;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

IconLoader *
icon_loader_get (void)
{
    g_once (&g_loader_once, icon_loader_init_once, NULL);
    return g_loader;
}

void
icon_loader_destroy (void)
{
    if (!g_loader) return;

    g_thread_pool_free (g_loader->pool, TRUE, TRUE);

    CacheNode *node = g_loader->head;
    while (node) {
        CacheNode *next = node->next;
        g_free (node->key);
        if (node->pixbuf) g_object_unref (node->pixbuf);
        g_free (node);
        node = next;
    }

    g_hash_table_destroy (g_loader->map);
    pthread_mutex_destroy (&g_loader->path_lock);
    g_free (g_loader);
    g_loader = NULL;
}

GdkPixbuf *
icon_loader_load (IconLoader *loader, const char *icon_name)
{
    g_return_val_if_fail (loader != NULL, NULL);

    GdkPixbuf *cached = lru_get (loader, icon_name);
    if (cached) return cached;

    char *path = resolve_icon_path (loader, icon_name);
    if (!path) return NULL;

    GError    *err = NULL;
    GdkPixbuf *pb  = gdk_pixbuf_new_from_file_at_scale (
        path, ICON_LOAD_SIZE, ICON_LOAD_SIZE, TRUE, &err);
    g_free (path);
    if (err) { g_error_free (err); return NULL; }

    lru_put (loader, icon_name, pb);
    return pb;
}

void
icon_loader_load_async (IconLoader       *loader,
                        const char       *icon_name,
                        IconReadyCallback callback,
                        gpointer          user_data)
{
    g_return_if_fail (loader   != NULL);
    g_return_if_fail (callback != NULL);

    /* Cache check on main thread */
    GdkPixbuf *cached = lru_get (loader, icon_name);
    if (cached) {
        callback (cached, user_data);
        g_object_unref (cached);
        return;
    }

    /* Resolve icon path on main thread (GtkIconTheme is not thread-safe) */
    char *path = resolve_icon_path (loader, icon_name);

    /* Build task — thread only does file I/O */
    AsyncTask *task        = g_new0 (AsyncTask, 1);
    task->loader           = loader;
    task->resolved_path    = path;  /* may be NULL — thread handles gracefully */
    task->icon_name        = g_strdup (icon_name);
    task->callback         = callback;
    task->user_data        = user_data;

    GError *err = NULL;
    g_thread_pool_push (loader->pool, task, &err);
    if (err) {
        g_warning ("IconLoader: push failed: %s", err->message);
        g_error_free (err);
        callback (NULL, user_data);
        g_free (task->icon_name);
        g_free (task->resolved_path);
        g_free (task);
    }
}
