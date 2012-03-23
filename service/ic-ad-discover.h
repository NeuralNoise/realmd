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

#ifndef __IC_AD_DISCOVER_H__
#define __IC_AD_DISCOVER_H__

#include <gio/gio.h>

#include "ic-kerberos-provider.h"

G_BEGIN_DECLS

void           ic_ad_discover_async         (IcKerberosProvider *provider,
                                             const gchar *string,
                                             GDBusMethodInvocation *invocation,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);

gchar *        ic_ad_discover_finish        (IcKerberosProvider *provider,
                                             GAsyncResult *result,
                                             GHashTable *discovery,
                                             GError **error);

G_END_DECLS

#endif /* __IC_AD_DISCOVER_H__ */
