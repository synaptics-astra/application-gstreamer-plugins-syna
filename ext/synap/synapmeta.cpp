/* GStreamer Synap Meta
 * Copyright (C) 2024 Synaptics Incorporated. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "synapmeta.h"

GType
gst_synap_meta_api_get_type (void)
{
  static GType type;
  static const gchar *tags[] = { "memory", NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstSynapMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

gboolean
gst_synap_meta_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  return TRUE;
}

void
gst_synap_meta_free (GstMeta * meta, GstBuffer * buffer)
{

}

const GstMetaInfo *
gst_synap_meta_get_info (void)
{
  static const GstMetaInfo *synap_meta_info = NULL;

  if (g_once_init_enter (&synap_meta_info)) {
    const GstMetaInfo *meta =
        gst_meta_register (GST_SYNAP_META_API_TYPE, "GstSynapMeta",
        sizeof (GstSynapMeta), gst_synap_meta_init,
        gst_synap_meta_free, (GstMetaTransformFunction) NULL);
    g_once_init_leave (&synap_meta_info, meta);
  }
  return synap_meta_info;
}

/**
 * gst_buffer_add_synap_meta:
 * @buffer: (transfer none): #GstBuffer holding synap text, to which
 * synap metadata should be added.
 * @regions: (transfer full): A #GPtrArray of #GstSynapRegions.
 *
 * Attaches synap metadata to a #GstBuffer.
 *
 * Returns: A pointer to the added #GstSynapMeta if successful; %NULL if
 * unsuccessful.
 */
GstSynapMeta *
gst_buffer_add_synap_meta (GstBuffer * buffer, gpointer data,
    GstSynapPostProcessingMode postproc_mode, gint width, gint height, gfloat confthreshold)
{
  GstSynapMeta *meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);

  meta =
      (GstSynapMeta *) gst_buffer_add_meta (buffer, GST_SYNAP_META_INFO, NULL);
  meta->data = data;
  meta->postproc_mode = postproc_mode;
  meta->width = width;
  meta->height = height;
  meta->confthreshold = confthreshold;
  return meta;
}
