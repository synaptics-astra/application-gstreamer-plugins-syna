/* GStreamer Synap Sink
 * Copyright (C) 2025 Synaptics Incorporated. All rights reserved.
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstsynapsink
 *
 * GStreamer plugin to dump synap inferences to file
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 filesrc location=Animals.mp4 ! qtdemux name=demux demux.video_0 ! queue ! \
 * h264parse ! v4l2h264dec ! tee name=t_data \
 * t_data. ! queue ! synavideoconvertscale ! video/x-raw,width=224,height=224,format=RGB ! \
 * synapinfer model=/usr/share/synap/models/image_classification/imagenet/model/mobilenet_v2_1.0_224_quant/model.synap \
 * mode=classifier frameinterval=3 output=json ! synapsink location=ai.txt t_data. ! queue ! waylandsink
 * ]|
 * Gstreamer pipeline to do synap infer and dump to file
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
#include "gstsynapsink.h"

GST_DEBUG_CATEGORY_STATIC (gst_synap_sink_debug_category);
#define GST_CAT_DEFAULT gst_synap_sink_debug_category

/* prototypes */


static void gst_synap_sink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_synap_sink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_synap_sink_dispose (GObject * object);
static void gst_synap_sink_finalize (GObject * object);
static gboolean gst_synap_sink_start (GstBaseSink * sink);
static gboolean gst_synap_sink_stop (GstBaseSink * sink);
static GstFlowReturn gst_synap_sink_render (GstBaseSink * sink,
    GstBuffer * buf);
static gboolean
gst_synap_sink_set_location (GstSynapSink * sink, const gchar * location,
    GError ** error);

enum
{
  PROP_0,
  PROP_LOCATION,
};

/* pad templates */

static GstStaticPadTemplate gst_synap_sink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstSynapSink, gst_synap_sink, GST_TYPE_BASE_SINK,
  GST_DEBUG_CATEGORY_INIT (gst_synap_sink_debug_category, "synapsink", 0,
  "debug category for synapsink element"));
GST_ELEMENT_REGISTER_DEFINE (synapsink, "synapsink", GST_RANK_NONE,
    GST_TYPE_SYNAP_SINK);

static void
gst_synap_sink_class_init (GstSynapSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS(klass),
      &gst_synap_sink_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Plugin for Synap Sink", "Generic/Sink",
      "GStreamer layer for Synap Sink", "https://www.synaptics.com/");

  gobject_class->set_property = gst_synap_sink_set_property;
  gobject_class->get_property = gst_synap_sink_get_property;
  gobject_class->dispose = gst_synap_sink_dispose;

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "File Location",
          "Location of the file to write", NULL,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  base_sink_class->start = GST_DEBUG_FUNCPTR (gst_synap_sink_start);
  base_sink_class->stop = GST_DEBUG_FUNCPTR (gst_synap_sink_stop);
  base_sink_class->render = GST_DEBUG_FUNCPTR (gst_synap_sink_render);

}

static void
gst_synap_sink_init (GstSynapSink *synapsink)
{
}

void
gst_synap_sink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSynapSink *synapsink = GST_SYNAP_SINK (object);

  GST_DEBUG_OBJECT (synapsink, "set_property");

  switch (property_id) {
    case PROP_LOCATION:
      gst_synap_sink_set_location (synapsink, g_value_get_string (value), NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_sink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSynapSink *synapsink = GST_SYNAP_SINK (object);

  GST_DEBUG_OBJECT (synapsink, "get_property");

  switch (property_id) {
    case PROP_LOCATION:
      g_value_set_string (value, synapsink->filename);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_sink_dispose (GObject * object)
{
  GstSynapSink *synapsink = GST_SYNAP_SINK (object);

  GST_DEBUG_OBJECT (synapsink, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_synap_sink_parent_class)->dispose (object);
}

void
gst_synap_sink_finalize (GObject * object)
{
  GstSynapSink *synapsink = GST_SYNAP_SINK (object);

  GST_DEBUG_OBJECT (synapsink, "finalize");

  /* clean up object here */

  G_OBJECT_CLASS (gst_synap_sink_parent_class)->finalize (object);
}

/* start and stop processing, ideal for opening/closing the resource */
static gboolean
gst_synap_sink_start (GstBaseSink * sink)
{
  GstSynapSink *synapsink = GST_SYNAP_SINK (sink);

  GST_DEBUG_OBJECT (synapsink, "start");
  /* open the file */
  if (synapsink->filename == NULL || synapsink->filename[0] == '\0')
    goto no_filename;
  synapsink->file = fopen (synapsink->filename, "wb");
  if (synapsink->file == NULL)
    goto open_failed;
  return TRUE;

  /* ERRORS */
  no_filename:
  {
    return FALSE;
  }
  open_failed:
  {
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_synap_sink_stop (GstBaseSink * sink)
{
  GstSynapSink *synapsink = GST_SYNAP_SINK (sink);

  GST_DEBUG_OBJECT (synapsink, "stop");
  if (synapsink->file) {
    if (fclose (synapsink->file) != 0) {
    }
    synapsink->file = NULL;
  }
  return TRUE;
}

static GstFlowReturn
gst_synap_sink_render (GstBaseSink * sink, GstBuffer * buf)
{
  GstSynapSink *synapsink = GST_SYNAP_SINK (sink);

  GST_DEBUG_OBJECT (synapsink, "render");

  GstCustomMeta *meta = gst_buffer_get_custom_meta (buf, "GstSynapStrMeta");
  const gchar *resStr = NULL;
  if (meta) {
    GstStructure *s = gst_custom_meta_get_structure (meta);
    if (s) {
      resStr = gst_structure_get_string (s, "result");
      if (resStr) {
        fwrite(resStr, 1, strlen(resStr), synapsink->file);
        fflush(synapsink->file);
      }
    }
  }
  return GST_FLOW_OK;
}

static gboolean
gst_synap_sink_set_location (GstSynapSink * sink, const gchar * location,
    GError ** error)
{
  if (sink->file)
    goto was_open;

  g_free (sink->filename);
  if (location != NULL) {
    /* we store the filename as we received it from the application. On Windows
     * this should be in UTF8 */
    sink->filename = g_strdup (location);
    GST_INFO_OBJECT (sink, "filename : %s", sink->filename);
  } else {
    sink->filename = NULL;
  }

  return TRUE;

  /* ERRORS */
was_open:
  {
    g_warning ("Changing the `location' property on filesink when a file is "
        "open is not supported.");
    g_set_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_STATE,
        "Changing the 'location' property on filesink when a file is "
        "open is not supported");
    return FALSE;
  }
}

