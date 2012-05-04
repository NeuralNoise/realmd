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
#define DEBUG_FLAG REALM_DEBUG_SERVICE
#include "realm-debug.h"
#include "realm-dbus-constants.h"
#include "realm-dbus-generated.h"
#include "realm-diagnostics.h"
#include "realm-errors.h"
#include "realm-kerberos-realm.h"

#include <krb5/krb5.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

struct _RealmKerberosRealmPrivate {
	GHashTable *discovery;
};

enum {
	PROP_0,
	PROP_DISCOVERY
};

G_DEFINE_TYPE (RealmKerberosRealm, realm_kerberos_realm,
               REALM_DBUS_TYPE_KERBEROS_REALM_SKELETON);

static void
handle_krb5_error (GDBusMethodInvocation *invocation,
                   krb5_error_code code,
                   krb5_context context,
                   const gchar *message,
                   ...)
{
	gchar *string;
	va_list va;

	va_start (va, message);
	string = g_strdup_vprintf (message, va);
	va_end (va);

	realm_diagnostics_error (invocation, NULL, "%s: %s", string,
	                         krb5_get_error_message (context, code));
	g_free (string);
}

static GBytes *
kinit_to_kerberos_cache (GDBusMethodInvocation *invocation,
                         const gchar *principal_name,
                         const gchar *password)
{
	krb5_get_init_creds_opt *options = NULL;
	krb5_context context = NULL;
	krb5_principal principal = NULL;
	krb5_error_code code;
	gchar *filename = NULL;
	krb5_ccache ccache = NULL;
	krb5_creds my_creds;
	GBytes *result = NULL;
	gchar *contents;
	gsize length;
	GError *error = NULL;
	int temp_fd;

	code = krb5_init_context (&context);
	if (code != 0) {
		handle_krb5_error (invocation, code, NULL,
		                   "Couldn't initialize kerberos");
		goto cleanup;
	}

	code = krb5_parse_name (context, principal_name, &principal);
	if (code != 0) {
		handle_krb5_error (invocation, code, context,
		                   "Couldn't parse principal: %s", principal_name);
		goto cleanup;
	}

	code = krb5_get_init_creds_opt_alloc (context, &options);
	if (code != 0) {
		g_warning ("Couldn't setup kerberos options: %s",
		           krb5_get_error_message (context, code));
		goto cleanup;
	}

	filename = g_build_filename (g_get_tmp_dir (), "realmd-krb5-cache.XXXXXX", NULL);
	temp_fd = g_mkstemp_full (filename, O_RDWR, S_IRUSR | S_IWUSR);
	if (temp_fd == -1) {
		realm_diagnostics_error (invocation, NULL,
		                         "Couldn't create credential cache file: %s",
		                         g_strerror (errno));
		goto cleanup;
	}
	close (temp_fd);

	code = krb5_cc_resolve (context, filename, &ccache);
	if (code != 0) {
		handle_krb5_error (invocation, code, context,
		                   "Couldn't resolve credential cache: %s", filename);
		goto cleanup;
	}

	code = krb5_get_init_creds_opt_set_out_ccache (context, options, ccache);
	if (code != 0) {
		g_warning ("Couldn't setup credential cache: %s",
		           krb5_get_error_message (context, code));
		goto cleanup;
	}

	code = krb5_get_init_creds_password (context, &my_creds, principal,
	                                     (char *)password, NULL, 0, 0, NULL, options);
	if (code != 0) {
		handle_krb5_error (invocation, code, context,
		                   "Couldn't authenticate as: %s", principal_name);
		goto cleanup;
	}

	krb5_cc_close (context, ccache);
	ccache = NULL;

	g_file_get_contents (filename, &contents, &length, &error);
	if (error != NULL) {
		realm_diagnostics_error (invocation, error, "Couldn't read credential cache");
		g_error_free (error);
	}
	result = g_bytes_new_take (contents, length);

cleanup:
	if (filename) {
		g_unlink (filename);
		g_free (filename);
	}

	if (options)
		krb5_get_init_creds_opt_free (context, options);
	if (principal)
		krb5_free_principal (context, principal);
	if (ccache)
		krb5_cc_close (context, ccache);
	if (context)
		krb5_free_context (context);
	return result;

}

typedef struct {
	RealmKerberosRealm *self;
	GDBusMethodInvocation *invocation;
} MethodClosure;

static MethodClosure *
method_closure_new (RealmKerberosRealm *self,
                    GDBusMethodInvocation *invocation)
{
	MethodClosure *closure = g_slice_new (MethodClosure);
	closure->self = g_object_ref (self);
	closure->invocation = g_object_ref (invocation);
	return closure;
}

static void
method_closure_free (MethodClosure *closure)
{
	g_object_unref (closure->self);
	g_object_unref (closure->invocation);
	g_slice_free (MethodClosure, closure);
}

static void
on_enroll_complete (GObject *source,
                    GAsyncResult *result,
                    gpointer user_data)
{
	MethodClosure *closure = user_data;
	RealmKerberosRealmClass *klass;
	GError *error = NULL;

	klass = REALM_KERBEROS_REALM_GET_CLASS (closure->self);
	g_return_if_fail (klass->enroll_finish != NULL);

	(klass->enroll_finish) (closure->self, result, &error);
	if (error == NULL) {
		realm_diagnostics_info (closure->invocation, "Successfully enrolled machine in realm");
		g_dbus_method_invocation_return_value (closure->invocation, g_variant_new ("()"));

	} else if (error && error->domain == REALM_ERROR) {
		realm_diagnostics_error (closure->invocation, error, NULL);
		g_dbus_method_invocation_return_gerror (closure->invocation, error);
		g_error_free (error);

	} else {
		realm_diagnostics_error (closure->invocation, error, "Failed to enroll machine in realm");
		g_dbus_method_invocation_return_error (closure->invocation, REALM_ERROR, REALM_ERROR_ENROLL_FAILED,
		                                       "Failed to enroll machine in realm. See diagnostics.");
		g_error_free (error);
	}

	realm_daemon_unlock_for_action (closure->invocation);
	method_closure_free (closure);
}

static gboolean
handle_enroll_with_credential_cache (RealmKerberosRealm *self,
                                     GDBusMethodInvocation *invocation,
                                     GVariant *admin_cache,
                                     gpointer unused)
{
	GBytes *admin_kerberos_cache;
	RealmKerberosRealmClass *klass;
	const guchar *data;
	gsize length;

	data = g_variant_get_fixed_array (admin_cache, &length, 1);
	if (length == 0) {
		g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
		                                       "Invalid zero length credential cache argument");
		return TRUE;
	}

	if (!realm_daemon_lock_for_action (invocation)) {
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_BUSY,
		                                       "Already running another action");
		return TRUE;
	}

	klass = REALM_KERBEROS_REALM_GET_CLASS (self);
	g_return_val_if_fail (klass->enroll_async != NULL, FALSE);
	g_return_val_if_fail (klass->enroll_finish != NULL, FALSE);

	admin_kerberos_cache = g_bytes_new_with_free_func (data, length,
	                                                   (GDestroyNotify)g_variant_unref,
	                                                   g_variant_ref (admin_cache));

	(klass->enroll_async) (self, admin_kerberos_cache, invocation,
	                       on_enroll_complete, method_closure_new (self, invocation));

	g_bytes_unref (admin_kerberos_cache);
	return TRUE;
}

static gboolean
handle_enroll_with_password (RealmKerberosRealm *self,
                             GDBusMethodInvocation *invocation,
                             const gchar *principal,
                             const gchar *password,
                             gpointer unused)
{
	GBytes *admin_kerberos_cache;
	RealmKerberosRealmClass *klass;

	admin_kerberos_cache = kinit_to_kerberos_cache (invocation, principal, password);
	if (admin_kerberos_cache == NULL) {
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_AUTH_FAILED,
		                                       "Failed to authenticate with password");
		return TRUE;
	}

	if (!realm_daemon_lock_for_action (invocation)) {
		g_bytes_unref (admin_kerberos_cache);
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_BUSY,
		                                       "Already running another action");
		return TRUE;
	}

	klass = REALM_KERBEROS_REALM_GET_CLASS (self);
	g_return_val_if_fail (klass->enroll_async != NULL, FALSE);
	g_return_val_if_fail (klass->enroll_finish != NULL, FALSE);

	(klass->enroll_async) (self, admin_kerberos_cache, invocation,
	                       on_enroll_complete, method_closure_new (self, invocation));

	g_bytes_unref (admin_kerberos_cache);
	return TRUE;
}

static void
on_unenroll_complete (GObject *source,
                      GAsyncResult *result,
                      gpointer user_data)
{
	MethodClosure *closure = user_data;
	RealmKerberosRealmClass *klass;
	GError *error = NULL;

	klass = REALM_KERBEROS_REALM_GET_CLASS (closure->self);
	g_return_if_fail (klass->unenroll_finish != NULL);

	if ((klass->unenroll_finish) (closure->self, result, &error)) {
		realm_diagnostics_info (closure->invocation, "Successfully unenrolled machine from realm");
		g_dbus_method_invocation_return_value (closure->invocation, g_variant_new ("()"));

	} else if (error && error->domain == REALM_ERROR) {
		realm_diagnostics_error (closure->invocation, error, NULL);
		g_dbus_method_invocation_return_gerror (closure->invocation, error);
		g_error_free (error);

	} else {
		realm_diagnostics_error (closure->invocation, error, "Failed to unenroll machine from realm");
		g_dbus_method_invocation_return_error (closure->invocation, REALM_ERROR, REALM_ERROR_UNENROLL_FAILED,
		                                       "Failed to unenroll machine from domain. See diagnostics.");
		g_error_free (error);
	}

	realm_daemon_unlock_for_action (closure->invocation);
	method_closure_free (closure);
}

static gboolean
handle_unenroll_with_credential_cache (RealmKerberosRealm *self,
                                       GDBusMethodInvocation *invocation,
                                       GVariant *admin_cache,
                                       gpointer unused)
{
	RealmKerberosRealmClass *klass;
	GBytes *admin_kerberos_cache;
	const guchar *data;
	gsize length;

	data = g_variant_get_fixed_array (admin_cache, &length, 1);
	if (length == 0) {
		g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
		                                       "Invalid zero length credential cache argument");
		return TRUE;
	}

	if (!realm_daemon_lock_for_action (invocation)) {
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_BUSY,
		                                       "Already running another action");
		return TRUE;
	}

	klass = REALM_KERBEROS_REALM_GET_CLASS (self);
	g_return_val_if_fail (klass->unenroll_async != NULL, FALSE);
	g_return_val_if_fail (klass->unenroll_finish != NULL, FALSE);

	admin_kerberos_cache = g_bytes_new_with_free_func (data, length,
	                                                   (GDestroyNotify)g_variant_unref,
	                                                   g_variant_ref (admin_cache));

	(klass->unenroll_async) (self, admin_kerberos_cache, invocation,
	                         on_unenroll_complete, method_closure_new (self, invocation));

	g_bytes_unref (admin_kerberos_cache);
	return TRUE;
}

static gboolean
handle_unenroll_with_password (RealmKerberosRealm *self,
                               GDBusMethodInvocation *invocation,
                               const gchar *principal,
                               const gchar *password,
                               gpointer unused)
{
	RealmKerberosRealmClass *klass;
	GBytes *admin_kerberos_cache;

	admin_kerberos_cache = kinit_to_kerberos_cache (invocation, principal, password);
	if (admin_kerberos_cache == NULL) {
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_AUTH_FAILED,
		                                       "Failed to authenticate with password");
		return TRUE;
	}

	if (!realm_daemon_lock_for_action (invocation)) {
		g_bytes_unref (admin_kerberos_cache);
		g_dbus_method_invocation_return_error (invocation, REALM_ERROR, REALM_ERROR_BUSY,
		                                       "Already running another action");
		return TRUE;
	}

	klass = REALM_KERBEROS_REALM_GET_CLASS (self);
	g_return_val_if_fail (klass->unenroll_async != NULL, FALSE);
	g_return_val_if_fail (klass->unenroll_finish != NULL, FALSE);

	(klass->unenroll_async) (self, admin_kerberos_cache, invocation,
	                         on_unenroll_complete, method_closure_new (self, invocation));

	g_bytes_unref (admin_kerberos_cache);
	return TRUE;
}

static gboolean
on_authorize_method (GDBusInterfaceSkeleton *skeleton,
                     GDBusMethodInvocation  *invocation,
                     gpointer user_data)
{
	const gchar *interface = g_dbus_method_invocation_get_interface_name (invocation);
	const gchar *method = g_dbus_method_invocation_get_method_name (invocation);
	const gchar *action_id = NULL;
	gboolean ret;

	/* Reading properties is authorized */
	if (g_str_equal (interface, DBUS_PROPERTIES_INTERFACE)) {
		if (g_str_equal (method, "Get") ||
		    g_str_equal (method, "GetAll"))
			ret = TRUE;
		else
			ret = FALSE; /* we have no setters */

	/* Each method has its own polkit authorization */
	} else if (g_str_equal (interface, REALM_DBUS_KERBEROS_REALM_INTERFACE)) {
		if (g_str_equal (method, "EnrollWithCredentialCache") ||
		    g_str_equal (method, "EnrollWithPassword")) {
			action_id = "org.freedesktop.realmd.enroll-machine";
		} else if (g_str_equal (method, "UnenrollWithCredentialCache") ||
		           g_str_equal (method, "UnenrollWithPassword")) {
			action_id = "org.freedesktop.realmd.unenroll-machine";
		} else {
			g_warning ("encountered unknown method during auth checks: %s.%s",
			           interface, method);
			action_id = NULL;
		}

		if (action_id != NULL)
			ret = realm_daemon_check_dbus_action (g_dbus_method_invocation_get_sender (invocation),
			                                       action_id);
		else
			ret = FALSE;
	}

	if (ret == FALSE) {
		realm_debug ("rejecting access to: %s.%s method on %s",
		             interface, method, g_dbus_method_invocation_get_object_path (invocation));
		g_dbus_method_invocation_return_dbus_error (invocation, REALM_DBUS_ERROR_NOT_AUTHORIZED,
		                                            "Not authorized to perform this action");
	}

	return ret;
}

static void
realm_kerberos_realm_init (RealmKerberosRealm *self)
{
	self->pv = G_TYPE_INSTANCE_GET_PRIVATE (self, REALM_TYPE_KERBEROS_REALM,
	                                        RealmKerberosRealmPrivate);

	g_signal_connect (self, "g-authorize-method",
	                  G_CALLBACK (on_authorize_method), NULL);
	g_signal_connect (self, "handle-enroll-with-password",
	                  G_CALLBACK (handle_enroll_with_password), NULL);
	g_signal_connect (self, "handle-unenroll-with-password",
	                  G_CALLBACK (handle_unenroll_with_password), NULL);
	g_signal_connect (self, "handle-enroll-with-credential-cache",
	                  G_CALLBACK (handle_enroll_with_credential_cache), NULL);
	g_signal_connect (self, "handle-unenroll-with-credential-cache",
	                  G_CALLBACK (handle_unenroll_with_credential_cache), NULL);
}

static void
realm_kerberos_realm_get_property (GObject *obj,
                                   guint prop_id,
                                   GValue *value,
                                   GParamSpec *pspec)
{
	RealmKerberosRealm *self = REALM_KERBEROS_REALM (obj);

	switch (prop_id) {
	case PROP_DISCOVERY:
		g_value_set_boxed (value, self->pv->discovery);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
realm_kerberos_realm_set_property (GObject *obj,
                                   guint prop_id,
                                   const GValue *value,
                                   GParamSpec *pspec)
{
	switch (prop_id) {
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
		break;
	}
}

static void
realm_kerberos_realm_finalize (GObject *obj)
{
	RealmKerberosRealm *self = REALM_KERBEROS_REALM (obj);

	if (self->pv->discovery)
		g_hash_table_unref (self->pv->discovery);

	G_OBJECT_CLASS (realm_kerberos_realm_parent_class)->finalize (obj);
}

static void
realm_kerberos_realm_class_init (RealmKerberosRealmClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = realm_kerberos_realm_get_property;
	object_class->set_property = realm_kerberos_realm_set_property;
	object_class->finalize = realm_kerberos_realm_finalize;

	g_type_class_add_private (klass, sizeof (RealmKerberosRealmPrivate));

	g_object_class_install_property (object_class, PROP_DISCOVERY,
	             g_param_spec_boxed ("discovery", "Discovery", "Discovery Data",
	                                 G_TYPE_HASH_TABLE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

void
realm_kerberos_realm_set_discovery (RealmKerberosRealm *self,
                                    GHashTable *discovery)
{
	g_return_if_fail (REALM_IS_KERBEROS_REALM (self));
	g_return_if_fail (discovery != NULL);

	if (discovery)
		g_hash_table_ref (discovery);
	if (self->pv->discovery)
		g_hash_table_unref (self->pv->discovery);
	self->pv->discovery = discovery;
	g_object_notify (G_OBJECT (self), "discovery");
}

GHashTable *
realm_kerberos_realm_get_discovery (RealmKerberosRealm *self)
{
	g_return_val_if_fail (REALM_IS_KERBEROS_REALM (self), NULL);
	return self->pv->discovery;
}