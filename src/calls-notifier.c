/*
 * Copyright (C) 2020 Purism SPC
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
 * Author: Guido Günther <agx@sigxcpu.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "calls-notifier.h"
#include "calls-manager.h"
#include "config.h"

#include <glib/gi18n.h>
#include <glib-object.h>

enum {
  PROP_0,
  PROP_CONTACTS,
  PROP_LAST_PROP,
};
static GParamSpec *props[PROP_LAST_PROP];

struct _CallsNotifier
{
  GObject parent_instance;

  GListStore    *unanswered;
  GList         *notifications;

  CallsContacts *contacts;
};

G_DEFINE_TYPE (CallsNotifier, calls_notifier, G_TYPE_OBJECT);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (EPhoneNumber, e_phone_number_free)

static void
notify (CallsNotifier *self, CallsCall *call)
{
  GApplication *app = g_application_get_default ();
  g_autoptr(GNotification) notification;
  g_autofree gchar *msg = NULL;
  g_autofree gchar *ref = NULL;
  g_autoptr(EPhoneNumber) phone_number = NULL;
  g_autoptr(GError) err = NULL;
  const char *number, *name;
  CallsBestMatch *match;

  notification = g_notification_new (_("Missed call"));

  number = calls_call_get_number (call);
  if (!number)
    goto done;

  phone_number = e_phone_number_from_string (number, NULL, &err);
  if (!phone_number)
    {
      g_warning ("Failed to convert %s to a phone number: %s", number, err->message);
      goto done;
    }

  match = calls_contacts_lookup_phone_number (self->contacts, phone_number);
  if (!match)
    goto done;

  name = calls_best_match_get_name (match);
  if (name)
    msg = g_strdup_printf (_("Missed call from <b>%s</b>"), name);

 done:
  if (msg == NULL)
    msg = g_strdup_printf (_("Missed call from unknown caller"));

  g_notification_set_body (notification, msg);
  ref = g_strdup_printf ("missed-call-%s", calls_call_get_number (call) ?: "unknown");
  g_application_send_notification (app, ref, notification);
}


static void
state_changed_cb (CallsNotifier  *self,
                  CallsCallState new_state,
                  CallsCallState old_state,
                  CallsCall      *call)
{
  guint n;

  g_return_if_fail (CALLS_IS_NOTIFIER (self));
  g_return_if_fail (CALLS_IS_CALL (call));
  g_return_if_fail (old_state != new_state);

  if (old_state == CALLS_CALL_STATE_INCOMING &&
      new_state == CALLS_CALL_STATE_DISCONNECTED)
    {
      notify (self, call);
    }

  /* Can use g_list_store_find with newer glib */
  n = g_list_model_get_n_items (G_LIST_MODEL (self->unanswered));
  for (int i = 0; i < n; i++)
    {
      g_autoptr(CallsCall) item = g_list_model_get_item (G_LIST_MODEL (self->unanswered), i);
      if (item == call)
        {
          g_list_store_remove (self->unanswered, i);
          g_signal_handlers_disconnect_by_data (item, self);
        }
    }
}

static void
call_added_cb (CallsNotifier *self, CallsCall *call)
{
  g_list_store_append(self->unanswered, call);

  g_signal_connect_swapped (call,
                            "state-changed",
                            G_CALLBACK (state_changed_cb),
                            self);
}


static void
calls_notifier_init (CallsNotifier *self)
{
  self->unanswered = g_list_store_new (CALLS_TYPE_CALL);
}


static void
calls_notifier_constructed (GObject *object)
{
  g_autoptr (GList) calls = NULL;
  GList *c;
  CallsNotifier *self = CALLS_NOTIFIER (object);

  g_signal_connect_swapped (calls_manager_get_default (),
                            "call-add",
                            G_CALLBACK (call_added_cb),
                            self);

  calls = calls_manager_get_calls (calls_manager_get_default ());
  for (c = calls; c != NULL; c = c->next)
    {
      call_added_cb (self, c->data);
    }

  G_OBJECT_CLASS (calls_notifier_parent_class)->constructed (object);
}


static void
calls_notifier_dispose (GObject *object)
{
  CallsNotifier *self = CALLS_NOTIFIER (object);

  g_list_store_remove_all (self->unanswered);
  g_clear_object (&self->unanswered);
  g_clear_object (&self->contacts);

  G_OBJECT_CLASS (calls_notifier_parent_class)->dispose (object);
}

static void
calls_notifier_set_property (GObject      *object,
                             guint         property_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  CallsNotifier *self = CALLS_NOTIFIER (object);

  switch (property_id)
    {
      case PROP_CONTACTS:
        g_set_object (&self->contacts,
                      CALLS_CONTACTS (g_value_get_object (value)));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
calls_notifier_class_init (CallsNotifierClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = calls_notifier_constructed;
  object_class->dispose = calls_notifier_dispose;
  object_class->set_property = calls_notifier_set_property;

  props[PROP_CONTACTS] =
    g_param_spec_object ("contacts",
                         "Contacts",
                         "Interface for libfolks",
                         CALLS_TYPE_CONTACTS,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);
}

CallsNotifier *
calls_notifier_new (CallsContacts *contacts)
{
  return g_object_new (CALLS_TYPE_NOTIFIER, "contacts", contacts, NULL);
}