/* identity-config - Identity configuration service
 *
 * Copyright 2012 Red Hat Inc
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 *
 * Author: Stef Walter <stefw@gnome.org>
 */

#include "config.h"

#ifndef __IC_AD_ENROLL_H__
#define __IC_AD_ENROLL_H__

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean            ic_service_lock_for_action            (GDBusMethodInvocation *invocation);

void                ic_service_unlock_for_action          (GDBusMethodInvocation *invocation);

const gchar *       ic_service_resolve_file               (const gchar *name);

G_END_DECLS

#endif /* __IC_AD_JOIN_H__ */
