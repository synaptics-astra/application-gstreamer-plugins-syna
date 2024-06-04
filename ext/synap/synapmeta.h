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

#ifndef __GST_SYNAP_META_H__
#define __GST_SYNAP_META_H__

#include <gst/gst.h>

G_BEGIN_DECLS
/**
 * @brief Synap Post-Processing Mode
 */
    typedef enum
{
  GST_SYNAP_POSTPROCESSING_MODE_NONE = 0,               /**< Mode None */
  GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER = 1,         /**< Classification Mode */
  GST_SYNAP_POSTPROCESSING_MODE_DETECTOR = 2,           /**< Detection Mode */
} GstSynapPostProcessingMode;

typedef struct _GstSynapMeta GstSynapMeta;

struct _GstSynapMeta
{
  GstMeta meta;

  GstSynapPostProcessingMode postproc_mode;
  gpointer data;
  gint width;
  gint height;
  gfloat confthreshold;
};

GType gst_synap_meta_api_get_type (void);
#define GST_SYNAP_META_API_TYPE (gst_synap_meta_api_get_type())

#define gst_buffer_get_synap_meta(b) \
    ((GstSynapMeta*)gst_buffer_get_meta ((b), GST_SYNAP_META_API_TYPE))

#define GST_SYNAP_META_INFO (gst_synap_meta_get_info())

gboolean gst_synap_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer);

void gst_synap_meta_free (GstMeta * meta, GstBuffer * buffer);

const GstMetaInfo * gst_synap_meta_get_info (void);

GstSynapMeta *gst_buffer_add_synap_meta (GstBuffer * buffer,
    gpointer data, GstSynapPostProcessingMode postproc_mode,
    gint width, gint height, gfloat confthreshold);

G_END_DECLS

#endif /* __GST_SYNAP_META_H__ */
