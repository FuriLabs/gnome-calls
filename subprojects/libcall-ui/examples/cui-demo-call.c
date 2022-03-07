/*
 * Copyright (C) 2021 Purism SPC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Author: Guido Günther <agx@sigxcpu.org>
 */

#include "config.h"

#include "cui-demo-call.h"

#include <glib/gi18n.h>


#define AVATAR_ICON TOP_SOURCE_DIR "/examples/images/cat.jpg"

enum {
  PROP_0,
  PROP_AVATAR_ICON,
  PROP_DISPLAY_NAME,
  PROP_ID,
  PROP_STATE,
  PROP_ENCRYPTED,
  PROP_CAN_DTMF,
  PROP_ACTIVE_TIME,
  PROP_LAST_PROP,
};
static GParamSpec *props[PROP_LAST_PROP];

struct _CuiDemoCall
{
  GObject       parent_instance;

  GLoadableIcon *avatar_icon;
  gchar        *id;
  gchar        *display_name;
  CuiCallState  state;
  gboolean      encrypted;
  gboolean      can_dtmf;

  guint accept_timeout_id;
  guint hangup_timeout_id;

  GTimer       *timer;
  gdouble       active_time;
  guint         timer_id;
};

static void cui_demo_cui_call_interface_init (CuiCallInterface *iface);
G_DEFINE_TYPE_WITH_CODE (CuiDemoCall, cui_demo_call, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (CUI_TYPE_CALL,
                                                cui_demo_cui_call_interface_init))


static void
cui_demo_call_get_property (GObject    *object,
			    guint       prop_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
  CuiDemoCall *self = CUI_DEMO_CALL (object);

  switch (prop_id) {
  case PROP_AVATAR_ICON:
    g_value_set_object (value, self->avatar_icon);
    break;
  case PROP_ID:
    g_value_set_string (value, self->id);
    break;
  case PROP_DISPLAY_NAME:
    g_value_set_string (value, self->display_name);
    break;
  case PROP_STATE:
    g_value_set_enum (value, self->state);
    break;
  case PROP_ENCRYPTED:
    g_value_set_boolean (value, self->encrypted);
    break;
  case PROP_CAN_DTMF:
    g_value_set_boolean (value, self->can_dtmf);
    break;
  case PROP_ACTIVE_TIME:
    g_value_set_double (value, self->active_time);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}


static void
cui_demo_call_finalize (GObject *object)
{
  CuiDemoCall *self = CUI_DEMO_CALL (object);

  g_clear_object (&self->avatar_icon);
  g_clear_handle_id (&self->accept_timeout_id, g_source_remove);
  g_clear_handle_id (&self->hangup_timeout_id, g_source_remove);
  g_clear_handle_id (&self->timer_id, g_source_remove);
  g_clear_pointer (&self->timer, g_timer_destroy);

  G_OBJECT_CLASS (cui_demo_call_parent_class)->finalize (object);
}



static void
cui_demo_call_class_init (CuiDemoCallClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = cui_demo_call_finalize;
  object_class->get_property = cui_demo_call_get_property;

  g_object_class_override_property (object_class,
                                    PROP_AVATAR_ICON,
                                    "avatar-icon");

  g_object_class_override_property (object_class,
                                    PROP_ID,
                                    "id");

  g_object_class_override_property (object_class,
                                    PROP_DISPLAY_NAME,
                                    "display-name");

  g_object_class_override_property (object_class,
                                    PROP_STATE,
                                    "state");

  g_object_class_override_property (object_class,
                                    PROP_ENCRYPTED,
                                    "encrypted");

  g_object_class_override_property (object_class,
                                    PROP_CAN_DTMF,
                                    "can-dtmf");

  g_object_class_override_property (object_class,
                                    PROP_ACTIVE_TIME,
                                    "active-time");
  props[PROP_ACTIVE_TIME] =
    g_object_class_find_property (object_class, "active-time");
}


static GLoadableIcon *
cui_demo_call_get_avatar_icon (CuiCall *call)
{
  g_return_val_if_fail (CUI_IS_DEMO_CALL (call), NULL);

  return CUI_DEMO_CALL (call)->avatar_icon;
}


static const char *
cui_demo_call_get_id (CuiCall *call)
{
  g_return_val_if_fail (CUI_IS_DEMO_CALL (call), NULL);

  return CUI_DEMO_CALL (call)->id;
}


static const char *
cui_demo_call_get_display_name (CuiCall *call)
{
  g_return_val_if_fail (CUI_IS_DEMO_CALL (call), NULL);

  return CUI_DEMO_CALL (call)->display_name;
}


static CuiCallState
cui_demo_call_get_state (CuiCall *call)
{
  g_return_val_if_fail (CUI_IS_DEMO_CALL (call), CUI_CALL_STATE_UNKNOWN);

  return CUI_DEMO_CALL (call)->state;
}


static gboolean
cui_demo_call_get_encrypted (CuiCall *call)
{
  g_return_val_if_fail (CUI_IS_DEMO_CALL (call), CUI_CALL_STATE_UNKNOWN);

  return CUI_DEMO_CALL (call)->encrypted;
}


static gboolean
cui_demo_call_get_can_dtmf (CuiCall *call)
{
  g_return_val_if_fail (CUI_IS_DEMO_CALL (call), FALSE);

  return CUI_DEMO_CALL (call)->can_dtmf;
}


static gdouble
cui_demo_call_get_active_time (CuiCall *call)
{
  g_return_val_if_fail (CUI_DEMO_CALL (call), 0.0);

  return CUI_DEMO_CALL (call)->active_time;
}


static gboolean
on_timer_ticked (CuiDemoCall *self)
{
  g_assert (CUI_IS_DEMO_CALL (self));

  self->active_time = g_timer_elapsed (self->timer, NULL);
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ACTIVE_TIME]);

  return G_SOURCE_CONTINUE;
}


static gboolean
on_accept_timeout (CuiDemoCall *self)
{
  g_assert (CUI_IS_DEMO_CALL (self));

  self->state = CUI_CALL_STATE_ACTIVE;
  g_object_notify (G_OBJECT (self), "state");

  self->accept_timeout_id = 0;

  self->timer = g_timer_new ();
  self->timer_id = g_timeout_add (500,
                                  G_SOURCE_FUNC (on_timer_ticked),
                                  self);

  return G_SOURCE_REMOVE;
}


static gboolean
on_hang_up_timeout (CuiDemoCall *self)
{
  g_assert (CUI_IS_DEMO_CALL (self));

  g_clear_handle_id (&self->timer_id, g_source_remove);
  g_clear_pointer (&self->timer, g_timer_destroy);

  self->state = CUI_CALL_STATE_DISCONNECTED;
  g_object_notify (G_OBJECT (self), "state");

  self->hangup_timeout_id = 0;

  return G_SOURCE_REMOVE;
}


static void
cui_demo_call_accept (CuiCall *call)
{
  CuiDemoCall *self = CUI_DEMO_CALL (call);
  g_return_if_fail (CUI_IS_DEMO_CALL (self));

  if (self->accept_timeout_id) {
    g_debug ("Accepting call already pending");
    return;
  }

  if (self->hangup_timeout_id) {
    g_debug ("Hang-up pending, cannot accept call");
    return;
  }

  /* Delay accepting the call as "real" calls can take some time until state changes */
  self->accept_timeout_id =
    g_timeout_add_seconds (1, G_SOURCE_FUNC (on_accept_timeout), call);
}


static void
cui_demo_call_hang_up (CuiCall *call)
{
  CuiDemoCall *self = CUI_DEMO_CALL (call);
  g_return_if_fail (CUI_IS_DEMO_CALL (self));

  if (self->hangup_timeout_id) {
    g_debug ("Hang-up already pending");
    return;
  }

  if (self->accept_timeout_id)
    g_clear_handle_id (&self->accept_timeout_id, g_source_remove);

  /* Delay hanging up the call as "real" calls can take some time until state changes */
  self->hangup_timeout_id =
    g_timeout_add (250, G_SOURCE_FUNC (on_hang_up_timeout), call);
}


static void
cui_demo_call_send_dtmf (CuiCall *call, const gchar *dtmf)
{
  g_return_if_fail (CUI_IS_DEMO_CALL (call));

  g_message ("DTMF: %s", dtmf);
}


static void
cui_demo_cui_call_interface_init (CuiCallInterface *iface)
{
  iface->get_avatar_icon = cui_demo_call_get_avatar_icon;
  iface->get_id = cui_demo_call_get_id;
  iface->get_display_name = cui_demo_call_get_display_name;
  iface->get_state = cui_demo_call_get_state;
  iface->get_encrypted = cui_demo_call_get_encrypted;
  iface->get_can_dtmf = cui_demo_call_get_can_dtmf;
  iface->get_active_time = cui_demo_call_get_active_time;

  iface->accept = cui_demo_call_accept;
  iface->hang_up = cui_demo_call_hang_up;
  iface->send_dtmf = cui_demo_call_send_dtmf;
}


static void
cui_demo_call_init (CuiDemoCall *self)
{
  g_autoptr (GFile) file = g_file_new_for_path (AVATAR_ICON);

  self->display_name = "John Doe";
  self->id = "0800 1234";
  self->state = CUI_CALL_STATE_INCOMING;
  self->can_dtmf = TRUE;
  self->avatar_icon = G_LOADABLE_ICON (g_file_icon_new (file));

  g_assert (G_IS_LOADABLE_ICON (self->avatar_icon));
}


CuiDemoCall *
cui_demo_call_new (void)
{
   return g_object_new (CUI_TYPE_DEMO_CALL, NULL);
}