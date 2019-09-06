/*
 * Copyright (C) 2018 Purism SPC
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
 * Authors:
 *     Bob Ham <bob.ham@puri.sm>
 *     Adrien Plazas <adrien.plazas@puri.sm>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#include "calls-call-window.h"
#include "calls-origin.h"
#include "calls-call-holder.h"
#include "calls-call-selector-item.h"
#include "calls-new-call-box.h"
#include "calls-enumerate.h"
#include "calls-wayland-config.h"
#include "util.h"

#include <glib/gi18n.h>
#include <glib-object.h>

#define HANDY_USE_UNSTABLE_API
#include <handy.h>

#ifdef CALLS_WAYLAND
#include <gdk/gdkwayland.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wayland/layersurface.h"
#endif // CALLS_WAYLAND

struct _CallsCallWindow
{
  GtkApplicationWindow parent_instance;

  GListStore *call_holders;

  GtkRevealer *info_revealer;
  guint info_timeout;
  GtkInfoBar *info;
  GtkLabel *info_label;

  GtkStack *main_stack;
  GtkStack *header_bar_stack;
  GtkButton *show_calls;
  GtkStack *call_stack;
  GtkFlowBox *call_selector;

#ifdef CALLS_WAYLAND
  gboolean screensaver_active;
  struct zwlr_layer_shell_v1 *layer_shell_iface;
  struct wl_output *output;
  GtkWindow *layer_surface;
#endif // CALLS_WAYLAND
};

G_DEFINE_TYPE (CallsCallWindow, calls_call_window, GTK_TYPE_APPLICATION_WINDOW);


enum {
  PROP_0,
  PROP_PROVIDER,
  PROP_LAST_PROP,
};
static GParamSpec *props[PROP_LAST_PROP];


#ifdef CALLS_WAYLAND
static void
tear_down_layer_surface (CallsCallWindow *self)
{
  GtkWidget *child;

  if (!self->layer_surface)
    {
      return;
    }

  g_debug ("Tearing down layer surface");

  // Remove window content from layer surface
  child = gtk_bin_get_child (GTK_BIN (self->layer_surface));
  g_object_ref (child);

  gtk_container_remove (GTK_CONTAINER (self->layer_surface),
                        child);

  // Add to the call window
  gtk_container_add (GTK_CONTAINER (self), child);
  g_object_unref (child);

  // Destroy layer surface
  gtk_widget_destroy (GTK_WIDGET (self->layer_surface));
  self->layer_surface = NULL;
}


static void
set_up_layer_surface (CallsCallWindow *self)
{
  GtkWidget *child;

  if (self->layer_surface
      || !self->layer_shell_iface
      || !self->output)
    {
      return;
    }

  g_debug ("Setting up layer surface");

  // Create layer surface
  self->layer_surface = g_object_new
    (PHOSH_TYPE_LAYER_SURFACE,
     "layer-shell", self->layer_shell_iface,
     "wl-output", self->output,
     "layer", ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
     "namespace", "calls",
     "anchor", ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
     | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
     | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
     | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
     NULL);
  g_assert (self->layer_surface);

  // Remove window content from call window
  child = gtk_bin_get_child (GTK_BIN (self));
  g_object_ref (child);

  gtk_container_remove (GTK_CONTAINER (self), child);

  // Add to the layer surface
  gtk_container_add (GTK_CONTAINER (self->layer_surface),
                     child);
  g_object_unref (child);

  // Show the layer surface
  gtk_window_set_keep_above (self->layer_surface, TRUE);
  gtk_widget_show (GTK_WIDGET (self->layer_surface));
}


static void
update_layer_surface (CallsCallWindow *self,
                      guint calls)
{
  g_debug ("Updating layer surface");

  if (calls == 0)
    {
      tear_down_layer_surface (self);
    }
  else if (self->screensaver_active)
    {
      set_up_layer_surface (self);
    }
}
#endif // CALLS_WAYLAND


static void
update_visibility (CallsCallWindow *self)
{
  guint calls = g_list_model_get_n_items (G_LIST_MODEL (self->call_holders));

#ifdef CALLS_WAYLAND
  update_layer_surface (self, calls);
#endif // CALLS_WAYLAND

  gtk_widget_set_visible (GTK_WIDGET (self), calls > 0);
  gtk_widget_set_sensitive (GTK_WIDGET (self->show_calls), calls > 1);

  if (calls == 0)
    {
      gtk_stack_set_visible_child_name (self->main_stack, "calls");
    }
  else if (calls == 1)
    {
      gtk_stack_set_visible_child_name (self->main_stack, "active-call");
    }
}


static gboolean
show_message_timeout_cb (CallsCallWindow *self)
{
  gtk_revealer_set_reveal_child (self->info_revealer, FALSE);
  self->info_timeout = 0;
  return FALSE;
}


static void
show_message (CallsCallWindow *self,
              const gchar     *text,
              GtkMessageType   type)
{
  gtk_info_bar_set_message_type (self->info, type);
  gtk_label_set_text (self->info_label, text);
  gtk_revealer_set_reveal_child (self->info_revealer, TRUE);

  if (self->info_timeout)
    {
      g_source_remove (self->info_timeout);
    }
  self->info_timeout = g_timeout_add_seconds
    (3,
     (GSourceFunc)show_message_timeout_cb,
     self);
}


static inline void
stop_info_timeout (CallsCallWindow *self)
{
  if (self->info_timeout)
    {
      g_source_remove (self->info_timeout);
      self->info_timeout = 0;
    }
}


static void
info_response_cb (GtkInfoBar      *infobar,
                  gint             response_id,
                  CallsCallWindow *self)
{
  stop_info_timeout (self);
  gtk_revealer_set_reveal_child (self->info_revealer, FALSE);
}


static GtkWidget *
call_holders_create_widget_cb (CallsCallHolder *holder,
                               CallsCallWindow *self)
{
  CallsCallSelectorItem *item =
    calls_call_holder_get_selector_item (holder);
  g_object_ref (G_OBJECT (item));
  return GTK_WIDGET (item);
}


static void
new_call_submitted_cb (CallsCallWindow *self,
                       CallsOrigin     *origin,
                       const gchar     *number,
                       CallsNewCallBox *new_call_box)
{
  g_return_if_fail (CALLS_IS_CALL_WINDOW (self));

  calls_origin_dial (origin, number);
}


typedef gboolean (*FindCallHolderFunc) (CallsCallHolder *holder,
                                        gpointer user_data);


static gboolean
find_call_holder_by_call (CallsCallHolder *holder,
                          gpointer         user_data)
{
  CallsCallData *data = calls_call_holder_get_data (holder);

  return calls_call_data_get_call (data) == user_data;
}


/** Search through the list of call holders, returning the total
    number of items in the list, the position of the holder within the
    list and a pointer to the holder itself. */
static gboolean
find_call_holder (CallsCallWindow     *self,
                  guint               *n_itemsp,
                  guint               *positionp,
                  CallsCallHolder    **holderp,
                  FindCallHolderFunc   predicate,
                  gpointer             user_data)
{
  GListModel * const model = G_LIST_MODEL (self->call_holders);
  const guint n_items = g_list_model_get_n_items (model);
  guint position = 0;
  CallsCallHolder *holder;

  for (position = 0; position < n_items; ++position)
    {
      holder = CALLS_CALL_HOLDER (g_list_model_get_item (model, position));
      g_object_unref (G_OBJECT (holder));

      if (predicate (holder, user_data))
        {
#define out(var)                                \
          if (var##p)                           \
            {                                   \
              *var##p = var ;                   \
            }

          out (n_items);
          out (position);
          out (holder);

#undef out

          return TRUE;
        }
    }

  return FALSE;
}


static void
set_focus (CallsCallWindow  *self,
           CallsCallDisplay *display)
{
  gtk_stack_set_visible_child_name (self->main_stack, "active-call");
  gtk_stack_set_visible_child_name (self->header_bar_stack, "active-call");
  gtk_stack_set_visible_child (self->call_stack, GTK_WIDGET (display));
}


static void
show_calls_clicked_cb (GtkButton       *button,
                       CallsCallWindow *self)
{
  /* FIXME Setting only one of them should be enough as the properties are binded. */
  gtk_stack_set_visible_child_name (self->main_stack, "calls");
  gtk_stack_set_visible_child_name (self->header_bar_stack, "calls");
}


static void
call_selector_child_activated_cb (GtkFlowBox      *box,
                                  GtkFlowBoxChild *child,
                                  CallsCallWindow *self)
{
  GtkWidget *widget = gtk_bin_get_child (GTK_BIN (child));
  CallsCallSelectorItem *item = CALLS_CALL_SELECTOR_ITEM (widget);
  CallsCallDisplay *display = calls_call_selector_item_get_display (item);

  set_focus (self, display);
}


void
add_call (CallsCallWindow *self,
          CallsCall       *call)
{
  CallsCallHolder *holder;
  CallsCallDisplay *display;

  g_return_if_fail (CALLS_IS_CALL_WINDOW (self));
  g_return_if_fail (CALLS_IS_CALL (call));

  holder = calls_call_holder_new (call);

  display = calls_call_holder_get_display (holder);
  gtk_stack_add_named (self->call_stack, GTK_WIDGET (display),
                       calls_call_get_number (call));

  g_list_store_append (self->call_holders, holder);
  g_object_unref (holder);

  update_visibility (self);
  set_focus (self, display);
}


static void
remove_call_holder (CallsCallWindow *self,
                    guint            n_items,
                    guint            position,
                    CallsCallHolder *holder)
{
  gtk_container_remove (GTK_CONTAINER (self->call_stack),
                        GTK_WIDGET (calls_call_holder_get_display (holder)));
  g_list_store_remove (self->call_holders, position);

  update_visibility (self);
}

void
remove_call (CallsCallWindow *self,
             CallsCall       *call,
             const gchar     *reason)
{
  guint n_items, position;
  CallsCallHolder *holder;
  gboolean found;

  g_return_if_fail (CALLS_IS_CALL_WINDOW (self));
  g_return_if_fail (CALLS_IS_CALL (call));

  found = find_call_holder (self, &n_items, &position, &holder,
                            find_call_holder_by_call, call);
  g_return_if_fail (found);

  remove_call_holder (self, n_items, position, holder);

  show_message(self, reason, GTK_MESSAGE_INFO);
}


static void
remove_calls (CallsCallWindow *self)
{
  GList *children, *child;

  /* Safely remove the call stack's children. */
  children = gtk_container_get_children (GTK_CONTAINER (self->call_stack));
  for (child = children; child != NULL; child = child->next)
    gtk_container_remove (GTK_CONTAINER (self->call_stack),
                          GTK_WIDGET (child->data));
  g_list_free (children);

  g_list_store_remove_all (self->call_holders);

  update_visibility (self);
}


static void
set_provider (CallsCallWindow *self, CallsProvider *provider)
{
  CallsEnumerateParams *params;

  params = calls_enumerate_params_new (self);

  calls_enumerate_params_add
    (params, CALLS_TYPE_ORIGIN, "call-added", G_CALLBACK (add_call));
  calls_enumerate_params_add
    (params, CALLS_TYPE_ORIGIN, "call-removed", G_CALLBACK (remove_call));

  calls_enumerate_params_add
    (params, CALLS_TYPE_CALL, "message", G_CALLBACK (show_message));

  calls_enumerate (provider, params);

  g_object_unref (params);
}


static void
set_property (GObject      *object,
              guint         property_id,
              const GValue *value,
              GParamSpec   *pspec)
{
  CallsCallWindow *self = CALLS_CALL_WINDOW (object);

  switch (property_id)
    {
      case PROP_PROVIDER:
        set_provider (self, CALLS_PROVIDER (g_value_get_object (value)));
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}


#ifdef CALLS_WAYLAND
static void
get_screensaver_active (CallsCallWindow *self,
                        GtkApplication  *app)
{
  g_object_get (app,
                "screensaver-active", &self->screensaver_active,
                NULL);
}


static void
notify_screensaver_active_cb (CallsCallWindow *self,
                              GParamSpec      *pspec,
                              GtkApplication  *app)
{
  get_screensaver_active (self, app);
  g_debug ("Screensaver became %sactive",
           self->screensaver_active ? "" : "in");

  update_visibility (self);
}


static gboolean
set_up_screensaver (CallsCallWindow *self)
{
  GtkApplication *app;
  gboolean reg;

  app = gtk_window_get_application (GTK_WINDOW (self));
  g_assert (app != NULL);

  g_object_get (app,
                "register-session", &reg,
                NULL);
  if (!reg)
    {
      g_warning ("Cannot monitor screensaver state because"
                 " Application is not set to register with session");
      return FALSE;
    }

  g_signal_connect_swapped (app,
                            "notify::screensaver-active",
                            G_CALLBACK (notify_screensaver_active_cb),
                            self);

  get_screensaver_active (self, app);
  g_debug ("Screensaver is %sactive at startup",
           self->screensaver_active ? "" : "in");

  return TRUE;
}


static void
registry_global_cb (void               *data,
                    struct wl_registry *registry,
                    uint32_t            name,
                    const char         *interface,
                    uint32_t            version)
{
  CallsCallWindow *self = CALLS_CALL_WINDOW (data);

  if (strcmp (interface, zwlr_layer_shell_v1_interface.name) == 0)
    {
      self->layer_shell_iface = wl_registry_bind
        (registry, name, &zwlr_layer_shell_v1_interface, version);
    }
  else if (strcmp (interface, "wl_output") == 0)
    {
      // FIXME: Deal with more than one output
      if (!self->output)
        {
          self->output = wl_registry_bind
            (registry, name, &wl_output_interface, version);
        }
    }
}

static const struct wl_registry_listener registry_listener =
{
  registry_global_cb,
  // FIXME: Add a remove callback
};


static void
set_up_wayland (CallsCallWindow *self)
{
  GdkDisplay *gdk_display;
  struct wl_display *display;
  struct wl_registry *registry;

  gdk_display = gdk_display_get_default ();
  g_assert (gdk_display != NULL);

  if (!GDK_IS_WAYLAND_DISPLAY (gdk_display))
    {
      g_debug ("GDK display is not a Wayland display");
      return;
    }

  display = gdk_wayland_display_get_wl_display (gdk_display);
  g_assert (display != NULL);

  registry = wl_display_get_registry (display);
  g_assert (registry != NULL);

  wl_registry_add_listener (registry, &registry_listener, self);
  wl_display_roundtrip (display);


  if (!self->layer_shell_iface)
    {
      g_warning ("Wayland display has no layer shell interface");
    }
  if (!self->output)
    {
      g_warning ("Wayland display has no output");
    }
}


static void
application_cb (CallsCallWindow *self)
{
  gboolean ok;

  ok = set_up_screensaver (self);

  if (ok)
    {
      set_up_wayland (self);
    }
}


static void
notify (GObject    *object,
        GParamSpec *pspec)
{
  CallsCallWindow *self = CALLS_CALL_WINDOW (object);
  const gchar *name;

  name = g_param_spec_get_name (pspec);
  if (strcmp (name, "application") == 0)
    {
      application_cb (self);
    }

  return;
}
#endif // CALLS_WAYLAND


static void
constructed (GObject *object)
{
  GObjectClass *parent_class = g_type_class_peek (GTK_TYPE_APPLICATION_WINDOW);
  CallsCallWindow *self = CALLS_CALL_WINDOW (object);

  gtk_flow_box_bind_model (self->call_selector,
                           G_LIST_MODEL (self->call_holders),
                           (GtkFlowBoxCreateWidgetFunc) call_holders_create_widget_cb,
                           NULL, NULL);

  update_visibility (self);

  parent_class->constructed (object);
}


static void
calls_call_window_init (CallsCallWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->call_holders = g_list_store_new (CALLS_TYPE_CALL_HOLDER);
}


static void
dispose (GObject *object)
{
  GObjectClass *parent_class = g_type_class_peek (GTK_TYPE_APPLICATION_WINDOW);
  CallsCallWindow *self = CALLS_CALL_WINDOW (object);

  if (self->call_holders)
    {
      remove_calls (self);
    }

  g_clear_object (&self->call_holders);
  stop_info_timeout (self);

  parent_class->dispose (object);
}


static void
calls_call_window_class_init (CallsCallWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->set_property = set_property;
  object_class->constructed = constructed;
  object_class->dispose = dispose;

#ifdef CALLS_WAYLAND
  // The "application" property is not a construction property so we
  // have to wait for it to be set before setting up wayland & co.
  object_class->notify = notify;
#endif // CALLS_WAYLAND

  props[PROP_PROVIDER] =
    g_param_spec_object ("provider",
                         _("Provider"),
                         _("An object implementing low-level call-making functionality"),
                         CALLS_TYPE_PROVIDER,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);

  g_object_class_install_properties (object_class, PROP_LAST_PROP, props);

  gtk_widget_class_set_template_from_resource (widget_class, "/sm/puri/calls/ui/call-window.ui");
  gtk_widget_class_bind_template_child (widget_class, CallsCallWindow, info_revealer);
  gtk_widget_class_bind_template_child (widget_class, CallsCallWindow, info);
  gtk_widget_class_bind_template_child (widget_class, CallsCallWindow, info_label);
  gtk_widget_class_bind_template_child (widget_class, CallsCallWindow, main_stack);
  gtk_widget_class_bind_template_child (widget_class, CallsCallWindow, header_bar_stack);
  gtk_widget_class_bind_template_child (widget_class, CallsCallWindow, show_calls);
  gtk_widget_class_bind_template_child (widget_class, CallsCallWindow, call_stack);
  gtk_widget_class_bind_template_child (widget_class, CallsCallWindow, call_selector);
  gtk_widget_class_bind_template_callback (widget_class, info_response_cb);
  gtk_widget_class_bind_template_callback (widget_class, call_selector_child_activated_cb);
  gtk_widget_class_bind_template_callback (widget_class, show_calls_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, new_call_submitted_cb);
}


CallsCallWindow *
calls_call_window_new (GtkApplication *application,
                       CallsProvider  *provider)
{
  return g_object_new (CALLS_TYPE_CALL_WINDOW,
                       "application", application,
                       "provider", provider,
                       NULL);
}