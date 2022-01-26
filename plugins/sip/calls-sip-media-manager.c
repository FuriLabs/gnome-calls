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

#define G_LOG_DOMAIN "CallsSipMediaManager"

#include "calls-settings.h"
#include "calls-sip-media-manager.h"
#include "gst-rfc3551.h"

#include <gst/gst.h>

/**
 * SECTION:sip-media-manager
 * @short_description: The media manager singleton
 * @Title: CallsSipMediaManager
 *
 * #CallsSipMediaManager is mainly responsible for generating appropriate
 * SDP messages for the set of supported codecs. In the future it
 * shall also manage the #CallsSipMediaPipeline objects that are in use.
 */

typedef struct _CallsSipMediaManager
{
  GObject parent;

  int            address_family;
  struct         addrinfo hints;

  CallsSettings *settings;
  GList         *preferred_codecs;
} CallsSipMediaManager;

G_DEFINE_TYPE (CallsSipMediaManager, calls_sip_media_manager, G_TYPE_OBJECT);


static const char *
get_address_family_string (CallsSipMediaManager *self,
                           const char           *ip)
{
  struct addrinfo *result = NULL;
  const char *family;

  if (getaddrinfo (ip, NULL, &self->hints, &result) != 0) {
    g_warning ("Cannot parse session IP %s", ip);
    return NULL;
  }

  /* check if IP is IPv4 or IPv6. We need to specify this in the c= line of SDP */
  self->address_family = result->ai_family;

  if (result->ai_family == AF_INET)
    family = "IP4";
  else if (result->ai_family == AF_INET6)
    family = "IP6";
  else
    family = NULL;

  freeaddrinfo (result);

  return family;
}


static void
on_notify_preferred_audio_codecs (CallsSipMediaManager *self)
{
  GList *supported_codecs;
  g_auto (GStrv) settings_codec_preference = NULL;

  g_assert (CALLS_IS_SIP_MEDIA_MANAGER (self));

  g_clear_list (&self->preferred_codecs, NULL);
  supported_codecs = media_codecs_get_candidates ();

  if (!supported_codecs) {
    g_warning ("There aren't any supported codecs installed on your system");
    return;
  }

  settings_codec_preference = calls_settings_get_preferred_audio_codecs (self->settings);

  if (!settings_codec_preference) {
    g_debug ("No audio codec preference set. Using all supported codecs");
    self->preferred_codecs = supported_codecs;
    return;
  }

  for (guint i = 0; settings_codec_preference[i] != NULL; i++) {
    MediaCodecInfo *codec = media_codec_by_name (settings_codec_preference[i]);

    if (!codec) {
      g_debug ("Did not find audio codec %s", settings_codec_preference[i]);
      continue;
    }
    if (media_codec_available_in_gst (codec))
      self->preferred_codecs = g_list_append (self->preferred_codecs, codec);
  }

  if (!self->preferred_codecs) {
    g_warning ("Cannot satisfy audio codec preference, "
               "falling back to all supported codecs");
    self->preferred_codecs = supported_codecs;
  } else {
    g_list_free (supported_codecs);
  }
}


static void
calls_sip_media_manager_finalize (GObject *object)
{
  CallsSipMediaManager *self = CALLS_SIP_MEDIA_MANAGER (object);
  gst_deinit ();

  g_list_free (self->preferred_codecs);
  g_object_unref (self->settings);

  G_OBJECT_CLASS (calls_sip_media_manager_parent_class)->finalize (object);
}


static void
calls_sip_media_manager_class_init (CallsSipMediaManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = calls_sip_media_manager_finalize;
}


static void
calls_sip_media_manager_init (CallsSipMediaManager *self)
{
  gst_init (NULL, NULL);

  self->settings = calls_settings_new ();
  g_signal_connect_swapped (self->settings,
                            "notify::preferred-audio-codecs",
                            G_CALLBACK (on_notify_preferred_audio_codecs),
                            self);
  on_notify_preferred_audio_codecs (self);

  /* Hints are used with getaddrinfo() when setting the session IP */
  self->hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG | AI_NUMERICHOST;
  self->hints.ai_family = AF_UNSPEC;
}


/* Public functions */

CallsSipMediaManager *
calls_sip_media_manager_default (void)
{
  static CallsSipMediaManager *instance = NULL;

  if (instance == NULL) {
    g_debug ("Creating CallsSipMediaManager");
    instance = g_object_new (CALLS_TYPE_SIP_MEDIA_MANAGER, NULL);
    g_object_add_weak_pointer (G_OBJECT (instance), (gpointer *) &instance);
  }
  return instance;
}


/* calls_sip_media_manager_get_capabilities:
 *
 * @self: A #CallsSipMediaManager
 * @port: Should eventually come from the ICE stack
 * @use_srtp: Whether to use srtp (not really handled)
 * @supported_codecs: A #GList of #MediaCodecInfo
 *
 * Returns: (transfer full): string describing capabilities
 * to be used in the session description (SDP)
 */
char *
calls_sip_media_manager_get_capabilities (CallsSipMediaManager *self,
                                          const char           *own_ip,
                                          guint                 port,
                                          gboolean              use_srtp,
                                          GList                *supported_codecs)
{
  char *payload_type = use_srtp ? "SAVP" : "AVP";
  g_autoptr (GString) media_line = NULL;
  g_autoptr (GString) attribute_lines = NULL;
  GList *node;
  const char *address_family_string;

  g_return_val_if_fail (CALLS_IS_SIP_MEDIA_MANAGER (self), NULL);

  media_line = g_string_new (NULL);
  attribute_lines = g_string_new (NULL);

  if (supported_codecs == NULL) {
    g_warning ("No supported codecs found. Can't build meaningful SDP message");
    g_string_append_printf (media_line, "m=audio 0 RTP/AVP");
    goto done;
  }

  /* media lines look f.e like "audio 31337 RTP/AVP 9 8 0" */
  g_string_append_printf (media_line,
                          "m=audio %d RTP/%s", port, payload_type);

  for (node = supported_codecs; node != NULL; node = node->next) {
    MediaCodecInfo *codec = node->data;

    g_string_append_printf (media_line, " %u", codec->payload_id);
    g_string_append_printf (attribute_lines,
                            "a=rtpmap:%u %s/%u%s",
                            codec->payload_id,
                            codec->name,
                            codec->clock_rate,
                            "\r\n");
  }

  g_string_append_printf (attribute_lines, "a=rtcp:%d\r\n", port + 1);

 done:
  if (own_ip && *own_ip)
    address_family_string = get_address_family_string (self, own_ip);

  if (own_ip && *own_ip && address_family_string)
    return g_strdup_printf ("v=0\r\n"
                            "c=IN %s %s\r\n"
                            "%s\r\n"
                            "%s\r\n",
                            address_family_string,
                            own_ip,
                            media_line->str,
                            attribute_lines->str);
  else
    return g_strdup_printf ("v=0\r\n"
                            "%s\r\n"
                            "%s\r\n",
                            media_line->str,
                            attribute_lines->str);

}


/* calls_sip_media_manager_static_capabilities:
 *
 * @self: A #CallsSipMediaManager
 * @port: Should eventually come from the ICE stack
 * @use_srtp: Whether to use srtp (not really handled)
 *
 * Returns: (transfer full): string describing capabilities
 * to be used in the session description (SDP)
 */
char *
calls_sip_media_manager_static_capabilities (CallsSipMediaManager *self,
                                             const char           *own_ip,
                                             guint                 port,
                                             gboolean              use_srtp)
{
  g_return_val_if_fail (CALLS_IS_SIP_MEDIA_MANAGER (self), NULL);

  return calls_sip_media_manager_get_capabilities (self,
                                                   own_ip,
                                                   port,
                                                   use_srtp,
                                                   self->preferred_codecs);
}


MediaCodecInfo*
get_best_codec (CallsSipMediaManager *self)
{
  return media_codec_by_name ("PCMA");
}


/* calls_sip_media_manager_codec_candiates:
 *
 * @self: A #CallsSipMediaManager
 *
 * Returns: (transfer none): A #GList of supported
 * #MediaCodecInfo
 */
GList *
calls_sip_media_manager_codec_candidates (CallsSipMediaManager *self)
{
  g_return_val_if_fail (CALLS_IS_SIP_MEDIA_MANAGER (self), NULL);

  return self->preferred_codecs;
}


/* calls_sip_media_manager_get_codecs_from_sdp
 *
 * @self: A #CallsSipMediaManager
 * @sdp: A #sdp_media_t media description
 *
 * Returns: (transfer container): A #GList of codecs found in the
 * SDP message
 */
GList *
calls_sip_media_manager_get_codecs_from_sdp (CallsSipMediaManager *self,
                                             sdp_media_t          *sdp_media)
{
  GList *codecs = NULL;
  sdp_rtpmap_t *rtpmap = NULL;

  g_return_val_if_fail (CALLS_IS_SIP_MEDIA_MANAGER (self), NULL);
  g_return_val_if_fail (sdp_media, NULL);

  if (sdp_media->m_type != sdp_media_audio) {
    g_warning ("Only the 'audio' media type is supported");
    return NULL;
  }

  for (rtpmap = sdp_media->m_rtpmaps; rtpmap != NULL; rtpmap = rtpmap->rm_next) {
    MediaCodecInfo *codec = media_codec_by_payload_id (rtpmap->rm_pt);
    if (codec)
      codecs = g_list_append (codecs, codec);
  }

  if (sdp_media->m_next != NULL)
    g_warning ("Currently only a single media session is supported");

  if (codecs == NULL)
    g_warning ("Did not find any common codecs");

  return codecs;
}


