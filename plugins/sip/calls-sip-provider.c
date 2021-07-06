/*
 * Copyright (C) 2021 Purism SPC
 *
 * This file is part of Calls.
 *
 * Calls is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Calls is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Calls.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Evangelos Ribeiro Tzaras <evangelos.tzaras@puri.sm>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#define G_LOG_DOMAIN "CallsSipProvider"

#define SIP_ACCOUNT_FILE "sip-account.cfg"

#include "calls-account-provider.h"
#include "calls-message-source.h"
#include "calls-provider.h"
#include "calls-sip-enums.h"
#include "calls-sip-origin.h"
#include "calls-sip-provider.h"
#include "calls-sip-util.h"
#include "config.h"

#include <libpeas/peas.h>
#include <sofia-sip/nua.h>
#include <sofia-sip/su_glib.h>

static const char * const supported_protocols[] = {
  "tel",
  "sip",
  "sips",
  NULL
};

/**
 * SECTION:sip-provider
 * @short_description: A #CallsProvider for the SIP protocol
 * @Title: CallsSipProvider
 *
 * #CallsSipProvider is derived from #CallsProvider and is responsible
 * for setting up the sofia-sip stack.
 */

enum {
  PROP_0,
  PROP_SIP_STATE,
  PROP_LAST_PROP,
};
static GParamSpec *props[PROP_LAST_PROP];

struct _CallsSipProvider
{
  CallsProvider parent_instance;

  GListStore *origins;
  /* SIP */
  CallsSipContext *ctx;
  SipEngineState sip_state;

  gboolean use_memory_backend;
  gchar *filename;
};

static void calls_sip_provider_message_source_interface_init (CallsMessageSourceInterface *iface);
static void calls_sip_provider_account_provider_interface_init (CallsAccountProviderInterface *iface);


G_DEFINE_DYNAMIC_TYPE_EXTENDED
(CallsSipProvider, calls_sip_provider, CALLS_TYPE_PROVIDER, 0,
 G_IMPLEMENT_INTERFACE_DYNAMIC (CALLS_TYPE_MESSAGE_SOURCE,
                                calls_sip_provider_message_source_interface_init)
 G_IMPLEMENT_INTERFACE_DYNAMIC (CALLS_TYPE_ACCOUNT_PROVIDER,
                                calls_sip_provider_account_provider_interface_init))

static void
new_origin_from_keyfile (CallsSipProvider *self,
                         GKeyFile         *key_file,
                         const char       *name)
{
  g_autofree char *host = NULL;
  g_autofree char *user = NULL;
  g_autofree char *password = NULL;
  g_autofree char *display_name = NULL;
  g_autofree char *protocol = NULL;
  gint port = 0;
  gint local_port = 0;
  gboolean auto_connect = TRUE;
  gboolean direct_mode = FALSE;

  g_return_if_fail (name);
  g_return_if_fail (key_file);
  g_return_if_fail (g_key_file_has_group (key_file, name));

  host = g_key_file_get_string (key_file, name, "Host", NULL);
  user = g_key_file_get_string (key_file, name, "User", NULL);
  /* TODO password will get removed very soon, but is currently useful for testing */
  password = g_key_file_get_string (key_file, name, "Password", NULL);
  display_name = g_key_file_get_string (key_file, name, "DisplayName", NULL);
  protocol = g_key_file_get_string (key_file, name, "Protocol", NULL);
  port = g_key_file_get_integer (key_file, name, "Port", NULL);
  display_name = g_key_file_get_string (key_file, name, "DisplayName", NULL);
  local_port = g_key_file_get_integer (key_file, name, "LocalPort", NULL);

  if (g_key_file_has_key (key_file, name, "AutoConnect", NULL))
    auto_connect = g_key_file_get_boolean (key_file, name, "AutoConnect", NULL);

  if (protocol == NULL)
    protocol = g_strdup ("UDP");

  if (g_key_file_has_key (key_file, name, "DirectMode", NULL))
    direct_mode = g_key_file_get_boolean (key_file, name, "DirectMode", NULL);

#define IS_NULL_OR_EMPTY(x)  ((x) == NULL || (x)[0] == '\0')
  if (!direct_mode &&
      (IS_NULL_OR_EMPTY (host) ||
       IS_NULL_OR_EMPTY (user) ||
       IS_NULL_OR_EMPTY (password))) {
    g_warning ("Host, user and password must not be empty");

    return;
  }
#undef IS_NULL_OR_EMPTY

  calls_sip_provider_add_origin_full (self,
                                      host,
                                      user,
                                      password,
                                      display_name,
                                      protocol,
                                      port,
                                      auto_connect,
                                      direct_mode,
                                      local_port);
}


static const char *
calls_sip_provider_get_name (CallsProvider *provider)
{
  return "SIP provider";
}


static const char *
calls_sip_provider_get_status (CallsProvider *provider)
{
  CallsSipProvider *self = CALLS_SIP_PROVIDER (provider);

  switch (self->sip_state) {
  case SIP_ENGINE_ERROR:
    return "Error";

  case SIP_ENGINE_READY:
    return "Normal";

  default:
    break;
  }
  return "Unknown";
}


static GListModel *
calls_sip_provider_get_origins (CallsProvider *provider)
{
  CallsSipProvider *self = CALLS_SIP_PROVIDER (provider);

  return G_LIST_MODEL (self->origins);
}

static const char * const *
calls_sip_provider_get_protocols (CallsProvider *provider)
{
  return supported_protocols;
}


static void
calls_sip_provider_deinit_sip (CallsSipProvider *self)
{
  GSource *gsource;

  if (self->sip_state == SIP_ENGINE_NULL)
    return;

  /* clean up sofia */
  if (!self->ctx)
    goto bail;


  if (self->ctx->root) {
    gsource = su_glib_root_gsource (self->ctx->root);
    g_source_destroy (gsource);
    su_root_destroy (self->ctx->root);

    if (su_home_unref (self->ctx->home) != 1)
      g_error ("Error in su_home_unref ()");
  }
  g_clear_pointer (&self->ctx, g_free);

 bail:
  self->sip_state = SIP_ENGINE_NULL;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SIP_STATE]);
}


static gboolean
calls_sip_provider_init_sofia (CallsSipProvider *self,
                               GError          **error)
{
  GSource *gsource;

  g_assert (CALLS_SIP_PROVIDER (self));

  self->ctx = g_new0 (CallsSipContext, 1);
  if (self->ctx == NULL) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Could not allocate memory for the SIP context");
    goto err;
  }

  if (su_init () != su_success) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "su_init () failed");
    goto err;
  }

  if (su_home_init (self->ctx->home) != su_success) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "su_home_init () failed");
    goto err;
  }

  self->ctx->root = su_glib_root_create (self);
  if (self->ctx->root == NULL) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "su_glib_root_create () failed");
    goto err;
  }
  gsource = su_glib_root_gsource (self->ctx->root);
  if (gsource == NULL) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "su_glib_root_gsource () failed");
    goto err;
  }

  g_source_attach (gsource, NULL);
  self->sip_state = SIP_ENGINE_READY;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SIP_STATE]);
  return TRUE;

 err:
  self->sip_state = SIP_ENGINE_ERROR;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SIP_STATE]);
  return FALSE;
}


static void
calls_sip_provider_get_property (GObject      *object,
                                 guint         property_id,
                                 GValue       *value,
                                 GParamSpec   *pspec)
{
  CallsSipProvider *self = CALLS_SIP_PROVIDER (object);

  switch (property_id) {
  case PROP_SIP_STATE:
    g_value_set_enum (value, self->sip_state);
    break;

  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    break;
  }
}


static void
calls_sip_provider_constructed (GObject *object)
{
  CallsSipProvider *self = CALLS_SIP_PROVIDER (object);
  g_autoptr (GError) error = NULL;
  const gchar *env_sip_test;

  env_sip_test = g_getenv ("CALLS_SIP_TEST");
  if (env_sip_test && env_sip_test[0] != '\0')
    self->use_memory_backend = TRUE;

  if (calls_sip_provider_init_sofia (self, &error)) {
    if (!self->use_memory_backend) {
      g_autoptr (GKeyFile) key_file = g_key_file_new ();

      if (!g_key_file_load_from_file (key_file, self->filename, G_KEY_FILE_NONE, &error)) {
        g_warning ("Error loading key file: %s", error->message);
        goto out;
      }
      calls_sip_provider_load_accounts (self, key_file);
    }
  } else {
    g_warning ("Could not initialize sofia stack: %s", error->message);
  }

 out:

  G_OBJECT_CLASS (calls_sip_provider_parent_class)->constructed (object);
}


static void
calls_sip_provider_dispose (GObject *object)
{
  CallsSipProvider *self = CALLS_SIP_PROVIDER (object);

  g_list_store_remove_all (self->origins);
  g_clear_object (&self->origins);

  g_clear_pointer (&self->filename, g_free);

  calls_sip_provider_deinit_sip (self);

  G_OBJECT_CLASS (calls_sip_provider_parent_class)->dispose (object);
}


static void
calls_sip_provider_class_finalize (CallsSipProviderClass *klass)
{
}


static void
calls_sip_provider_class_init (CallsSipProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  CallsProviderClass *provider_class = CALLS_PROVIDER_CLASS (klass);

  object_class->constructed = calls_sip_provider_constructed;
  object_class->dispose = calls_sip_provider_dispose;
  object_class->get_property = calls_sip_provider_get_property;

  provider_class->get_name = calls_sip_provider_get_name;
  provider_class->get_status = calls_sip_provider_get_status;
  provider_class->get_origins = calls_sip_provider_get_origins;
  provider_class->get_protocols = calls_sip_provider_get_protocols;

  props[PROP_SIP_STATE] =
    g_param_spec_enum ("sip-state",
                       "SIP state",
                       "The state of the SIP engine",
                       SIP_TYPE_ENGINE_STATE,
                       SIP_ENGINE_NULL,
                       G_PARAM_READABLE);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);
}


static void
calls_sip_provider_message_source_interface_init (CallsMessageSourceInterface *iface)
{
}

static void
calls_sip_provider_account_provider_interface_init (CallsAccountProviderInterface *iface)
{
}

static void
calls_sip_provider_init (CallsSipProvider *self)
{
  const char *filename_env = g_getenv ("CALLS_SIP_ACCOUNT_FILE");

  self->origins = g_list_store_new (CALLS_TYPE_ORIGIN);

  if (filename_env && filename_env[0] != '\0')
    self->filename = g_strdup (filename_env);
  else
    self->filename = g_build_filename (g_get_user_config_dir (),
                                       APP_DATA_NAME,
                                       SIP_ACCOUNT_FILE,
                                       NULL);
}

/**
 * calls_sip_provider_add_origin:
 * @self: A #CallsSipProvider
 * @host: The host to connect to
 * @user: The username to use
 * @password: The password to use
 * @display_name: The display name
 * @transport_protocol: The transport protocol to use, can be one of "UDP", "TCP" or "TLS"
 *
 * Adds a new origin (SIP account)
 *
 * Return: (transfer none): A #CallsSipOrigin
 */
CallsSipOrigin *
calls_sip_provider_add_origin (CallsSipProvider *self,
                               const char       *host,
                               const char       *user,
                               const char       *password,
                               const char       *display_name,
                               const char       *transport_protocol,
                               gint              port)
{
  return calls_sip_provider_add_origin_full (self,
                                             host,
                                             user,
                                             password,
                                             display_name,
                                             transport_protocol,
                                             port,
                                             TRUE,
                                             FALSE,
                                             0);
}

/**
 * calls_sip_provider_add_origin_full:
 * @self: A #CallsSipProvider
 * @host: The host to connect to
 * @user: The username to use
 * @password: The password to use
 * @display_name: The display name
 * @transport_protocol: The transport protocol to use, can be one of "UDP", "TCP" or "TLS"
 * @auto_connect: Whether to automatically try going online
 * @direct_mode: Whether to use direct connection mode. Useful when you don't want to
 * connect to a SIP server. Mostly useful for testing and debugging.
 *
 * Adds a new origin (SIP account). If @direct_mode is %TRUE then @host, @user and
 * @password do not have to be set.
 *
 * Return: (transfer none): A #CallsSipOrigin
 */
CallsSipOrigin *
calls_sip_provider_add_origin_full (CallsSipProvider *self,
                                    const char       *host,
                                    const char       *user,
                                    const char       *password,
                                    const char       *display_name,
                                    const char       *transport_protocol,
                                    gint              port,
                                    gboolean          auto_connect,
                                    gboolean          direct_mode,
                                    gint              local_port)
{
  g_autoptr (CallsSipOrigin) origin = NULL;
  g_autofree char *protocol = NULL;

  g_return_val_if_fail (CALLS_IS_SIP_PROVIDER (self), NULL);

  /* direct-mode is mostly useful for testing without a SIP server */
  if (!direct_mode) {
    g_return_val_if_fail (host, NULL);
    g_return_val_if_fail (user, NULL);
    g_return_val_if_fail (password, NULL);
  }

  if (transport_protocol) {
    g_return_val_if_fail (protocol_is_valid (transport_protocol), NULL);

    protocol = g_ascii_strup (transport_protocol, -1);
  }

  origin = g_object_new (CALLS_TYPE_SIP_ORIGIN,
                         "sip-context", self->ctx,
                         "host", host,
                         "user", user,
                         "password", password,
                         "display-name", display_name,
                         "transport-protocol", protocol ?: "UDP",
                         "port", port,
                         "auto-connect", auto_connect,
                         "direct-mode", direct_mode,
                         "local-port", local_port,
                         NULL);

  g_list_store_append (self->origins, origin);

  return origin;
}


CallsSipProvider *
calls_sip_provider_new (void)
{
  return g_object_new (CALLS_TYPE_SIP_PROVIDER, NULL);
}


void
calls_sip_provider_load_accounts (CallsSipProvider *self,
                                  GKeyFile         *key_file)
{
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) groups = NULL;

  g_return_if_fail (CALLS_IS_SIP_PROVIDER (self));
  g_return_if_fail (key_file);

  groups = g_key_file_get_groups (key_file, NULL);

  for (gsize i = 0; groups[i] != NULL; i++) {
    new_origin_from_keyfile (self, key_file, groups[i]);
  }
}


G_MODULE_EXPORT void
peas_register_types (PeasObjectModule *module)
{
  calls_sip_provider_register_type (G_TYPE_MODULE (module));

  peas_object_module_register_extension_type (module,
                                              CALLS_TYPE_PROVIDER,
                                              CALLS_TYPE_SIP_PROVIDER);
}
