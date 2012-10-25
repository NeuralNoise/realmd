/* realmd -- Realm configuration service
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

#include "realm-daemon.h"
#include "realm-dbus-constants.h"
#include "realm-diagnostics.h"
#include "realm-discovery.h"
#include "realm-errors.h"
#include "realm-example.h"
#include "realm-example-provider.h"
#include "realm-ini-config.h"
#include "realm-kerberos.h"

#include <string.h>

#define EXAMPLE_DOMAIN "EXAMPLE.COM"

struct _RealmExampleProvider {
	RealmProvider parent;
	RealmIniConfig *config;
};

typedef struct {
	RealmProviderClass parent_class;
} RealmExampleProviderClass;

enum {
	PROP_0,
	PROP_EXAMPLE_CONFIG,
};

#define  REALM_EXAMPLE_CONFIG       STATE_DIR "/example.conf"

#define  REALM_DBUS_EXAMPLE_PATH    "/org/freedesktop/realmd/Example"

#define  ALLOWED_CHARS              "abcdefghijklmnopqrstuvwxyz012346789-."

G_DEFINE_TYPE (RealmExampleProvider, realm_example_provider, REALM_TYPE_PROVIDER);

static void
realm_example_provider_init (RealmExampleProvider *self)
{
	self->config = realm_ini_config_new (REALM_INI_NONE);
}

static void
realm_example_provider_constructed (GObject *obj)
{
	RealmExampleProvider *self;
	GError *error = NULL;
	gchar **sections;
	gint i;

	G_OBJECT_CLASS (realm_example_provider_parent_class)->constructed (obj);

	self = REALM_EXAMPLE_PROVIDER (obj);

	realm_ini_config_read_file (self->config, REALM_EXAMPLE_CONFIG, &error);
	if (error != NULL) {
		g_warning ("Couldn't load config file: %s: %s",
		           REALM_EXAMPLE_CONFIG, error->message);
		g_error_free (error);
	}

	realm_provider_set_name (REALM_PROVIDER (self), "Example");

	sections = realm_ini_config_get_sections (self->config);
	for (i = 0; sections != NULL && sections[i] != NULL; i++) {
		realm_provider_lookup_or_register_realm (REALM_PROVIDER (self),
		                                         REALM_TYPE_EXAMPLE,
		                                         sections[i], NULL);
	}

	g_strfreev (sections);
}

static gboolean
on_discover_timeout (gpointer user_data)
{
	GSimpleAsyncResult *async = G_SIMPLE_ASYNC_RESULT (user_data);
	g_simple_async_result_complete (async);
	return FALSE;
}

static gchar *
parse_example_name (const char *string)
{
	gchar *domain;
	gsize length;

	domain = g_ascii_strdown (string, -1);
	g_strstrip (domain);

	length = strlen (domain);

	if (!length ||
	    strspn (domain, ALLOWED_CHARS) != length ||
	    strstr (domain, "..") != NULL ||
	    domain[0] == '.') {
		g_free (domain);
		return NULL;
	}

	if (g_str_has_suffix (domain, ".")) {
		domain[length] = '\0';
		length--;
	}

	/* No, I don't care */
	if (!g_str_has_suffix (domain, "example.org") &&
	    !g_str_has_suffix (domain, "example.com") &&
	    !g_str_has_suffix (domain, "example.net")) {
		g_free (domain);
		return NULL;
	}

	if (length > 11) {
		if (domain[length - 11] != '.') {
			g_free (domain);
			return NULL;
		}
	}

	return domain;
}

static void
realm_example_provider_discover_async (RealmProvider *provider,
                                       const gchar *string,
                                       GVariant *options,
                                       GDBusMethodInvocation *invocation,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data)
{
	GSimpleAsyncResult *async;

	async = g_simple_async_result_new (G_OBJECT (provider), callback, user_data,
	                                   realm_example_provider_discover_async);


	if (!realm_provider_match_software (options,
	                                    REALM_DBUS_IDENTIFIER_EXAMPLE,
	                                    REALM_DBUS_IDENTIFIER_EXAMPLE,
	                                    REALM_DBUS_IDENTIFIER_EXAMPLE)) {
		g_simple_async_result_complete_in_idle (async);

	/* A valid example domain name */
	} else {
		g_simple_async_result_set_op_res_gpointer (async,
		                                           parse_example_name (string),
		                                           g_free);

		g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
		                            g_random_int_range (2, 5),
		                            on_discover_timeout,
		                            g_object_ref (async),
		                            g_object_unref);
	}

	g_object_unref (async);
}

static GList *
realm_example_provider_discover_finish (RealmProvider *provider,
                                        GAsyncResult *result,
                                        gint *relevance,
                                        GError **error)
{
	RealmKerberos *realm = NULL;
	GSimpleAsyncResult *async;
	gchar *domain;

	async = G_SIMPLE_ASYNC_RESULT (result);
	domain = g_simple_async_result_get_op_res_gpointer (async);
	if (domain == NULL)
		return NULL;

	realm = realm_provider_lookup_or_register_realm (provider,
	                                                 REALM_TYPE_EXAMPLE,
	                                                 domain, NULL);

	if (realm == NULL)
		return NULL;

	*relevance = 10;
	return g_list_append (NULL, g_object_ref (realm));
}

static void
realm_example_provider_get_property (GObject *obj,
                                     guint prop_id,
                                     GValue *value,
                                     GParamSpec *pspec)
{
	RealmExampleProvider *self = REALM_EXAMPLE_PROVIDER (obj);

	switch (prop_id) {
	case PROP_EXAMPLE_CONFIG:
		g_value_set_object (value, self->config);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
realm_example_provider_finalize (GObject *obj)
{
	RealmExampleProvider *self = REALM_EXAMPLE_PROVIDER (obj);

	g_object_unref (self->config);

	G_OBJECT_CLASS (realm_example_provider_parent_class)->finalize (obj);
}

void
realm_example_provider_class_init (RealmExampleProviderClass *klass)
{
	RealmProviderClass *provider_class = REALM_PROVIDER_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	provider_class->discover_async = realm_example_provider_discover_async;
	provider_class->discover_finish = realm_example_provider_discover_finish;

	object_class->constructed = realm_example_provider_constructed;
	object_class->get_property = realm_example_provider_get_property;
	object_class->finalize = realm_example_provider_finalize;

	g_object_class_install_property (object_class, PROP_EXAMPLE_CONFIG,
	            g_param_spec_object ("example-config", "Example Config", "Example Config",
	                                 REALM_TYPE_INI_CONFIG, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

RealmProvider *
realm_example_provider_new (void)
{
	return g_object_new (REALM_TYPE_EXAMPLE_PROVIDER,
	                     "g-object-path", REALM_DBUS_EXAMPLE_PATH,
	                     NULL);
}