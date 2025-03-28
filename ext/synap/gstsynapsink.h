/* GStreamer Synap Sink
 * Copyright (C) 2025 Synaptics Incorporated. All rights reserved
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

#ifndef _GST_SYNAP_SINK_H_
#define _GST_SYNAP_SINK_H_
#include <stdio.h>
#include <gst/base/gstbasesink.h>

G_BEGIN_DECLS

#define GST_TYPE_SYNAP_SINK   (gst_synap_sink_get_type())
#define GST_SYNAP_SINK(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_SYNAP_SINK,GstSynapSink))
#define GST_SYNAP_SINK_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_SYNAP_SINK,GstSynapSinkClass))
#define GST_IS_SYNAP_SINK(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_SYNAP_SINK))
#define GST_IS_SYNAP_SINK_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_SYNAP_SINK))

typedef struct _GstSynapSink GstSynapSink;
typedef struct _GstSynapSinkClass GstSynapSinkClass;

struct _GstSynapSink
{
  GstBaseSink base_synapsink;

  gchar *filename;
  FILE *file;
};

struct _GstSynapSinkClass
{
  GstBaseSinkClass base_synapsink_class;
};

GType gst_synap_sink_get_type (void);

GST_ELEMENT_REGISTER_DECLARE (synapsink);
G_END_DECLS

#endif
