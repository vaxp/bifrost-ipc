#ifndef APP_MANAGER_H
#define APP_MANAGER_H

#include <glib-object.h>
#include "app_entry.h"

G_BEGIN_DECLS

#define VAXP_TYPE_APP_MANAGER (vaxp_app_manager_get_type ())
G_DECLARE_FINAL_TYPE (VaxpAppManager, vaxp_app_manager, VAXP, APP_MANAGER, GObject)

VaxpAppManager *vaxp_app_manager_get_default         (void);

GPtrArray      *vaxp_app_manager_get_apps            (VaxpAppManager *self);

void            vaxp_app_manager_launch_app          (VaxpAppManager *self,
                                                      AppEntry       *entry);

void            vaxp_app_manager_create_shortcut     (VaxpAppManager *self,
                                                      AppEntry       *entry);

void            vaxp_app_manager_uninstall_app_async (VaxpAppManager *self,
                                                      AppEntry       *entry);

G_END_DECLS

#endif /* APP_MANAGER_H */
