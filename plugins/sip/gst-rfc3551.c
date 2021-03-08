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

#include "gst-rfc3551.h"

#include <glib.h>

/* TODO check available codecs during runtime */
static MediaCodecInfo gst_codecs[] = {
  {"0", "PCMU", "8000", 1, "rtppcmupay", "rtppcmudepay", "mulawenc", "mulawdec"},
  {"3", "GSM", "8000", 1, "rtpgsmpay", "rtpgsmdepay", "gsmenc", "gsmdec"},
  {"4", "G723", "8000", 1, "rtpg723pay", "rtpg723depay", "avenc_g723_1", "avdec_g723_1"}, // does not seem to work
  {"8", "PCMA", "8000", 1, "rtppcmapay", "rtppcmadepay", "alawenc", "alawdec"},
  {"9", "G722", "8000", 1, "rtpg722pay", "rtpg722depay", "avenc_g722", "avdec_g722"},
};



MediaCodecInfo *
media_codec_by_name (const char *name)
{
  g_return_val_if_fail (name != NULL, NULL);

  for (guint i = 0; i < G_N_ELEMENTS (gst_codecs); i++) {
    if (g_strcmp0 (name, gst_codecs[i].name) == 0)
      return &gst_codecs[i];
  }

  return NULL;
}

gchar *
media_codec_get_gst_capabilities (MediaCodecInfo *codec)
{
  return g_strdup_printf ("application/x-rtp,media=(string)audio,clock-rate=(int)%s"
                          ",encoding-name=(string)%s,payload=(int)%s",
                          codec->clock_rate,
                          codec->name,
                          codec->payload_id);
}
