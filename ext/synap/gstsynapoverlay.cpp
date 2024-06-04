/* GStreamer SyNAPOverlay
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstsynapoverlay
 *
 * GStreamer Plugin to do AI overlay for SyNAP
 *
 * |[
 * <refsect2>
 * <title>For AI inference + overlay</title>
 * |[
 * gst-launch-1.0 v4l2src device=/dev/xxxx ! video/x-raw,<prop> ! videoconvert ! tee name=t_data \
 *  t_data. ! queue ! synapoverlay name=overlay label=<file> ! videoconvert ! waylandsink \
 *  t_data. ! queue ! videoconvert ! videoscale ! video/x-raw,width=xx,height=xx,format=RGB  ! \
 *  synapinfer model=<file> mode=detector frameinterval=3 ! overlay.inference_sink
 * ]|
 * Gstreamer pipeline to do inferencing with synapinfer and do the overlay drawing with synapoverlay
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include "gstsynapoverlay.h"
#include "gstsynapoverlayrender.h"
#include "synapmeta.h"

GST_DEBUG_CATEGORY_STATIC (gst_synap_overlay_debug_category);
#define GST_CAT_DEFAULT gst_synap_overlay_debug_category

/* prototypes */


static void gst_synap_overlay_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_synap_overlay_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_synap_overlay_dispose (GObject * object);
static void gst_synap_overlay_finalize (GObject * object);

static gboolean gst_synap_overlay_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean gst_synap_overlay_start (GstBaseTransform * trans);
static gboolean gst_synap_overlay_stop (GstBaseTransform * trans);
static GstFlowReturn gst_synap_overlay_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

static GstFlowReturn
gst_synap_overlay_inference_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer);

enum
{
  PROP_0,
  PROP_LABEL,
};

/* pad templates */
/* RGB16 is native-endianness in GStreamer */
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#define TEMPLATE_CAPS GST_VIDEO_CAPS_MAKE("{ BGRx, BGRA, RGB16 }")
#else
#define TEMPLATE_CAPS GST_VIDEO_CAPS_MAKE("{ xRGB, ARGB, RGB16 }")
#endif

static GstStaticPadTemplate gst_synap_overlay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (TEMPLATE_CAPS)
    );

static GstStaticPadTemplate gst_synap_overlay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (TEMPLATE_CAPS)
    );

static GstStaticPadTemplate inference_sink_template =
GST_STATIC_PAD_TEMPLATE ("inference_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ RGB }"))
    );

/* class initialization */
G_DEFINE_TYPE_WITH_CODE (GstSynapOverlay, gst_synap_overlay,
    GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_synap_overlay_debug_category, "synapoverlay",
        0, "debug category for synapoverlay element"));
GST_ELEMENT_REGISTER_DEFINE (synapoverlay, "synapoverlay", GST_RANK_NONE,
    GST_TYPE_SYNAP_OVERLAY);

static void
gst_synap_overlay_class_init (GstSynapOverlayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_synap_overlay_src_template);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_synap_overlay_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Plugin for Synaptics AI Overlay", "Generic/Video",
      "GStreamer layer for Synaptics AI Overlay", "https://www.synaptics.com/");

  gobject_class->set_property = gst_synap_overlay_set_property;
  gobject_class->get_property = gst_synap_overlay_get_property;
  gobject_class->dispose = gst_synap_overlay_dispose;
  gobject_class->finalize = gst_synap_overlay_finalize;
  base_transform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_synap_overlay_set_caps);
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_synap_overlay_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_synap_overlay_stop);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_synap_overlay_transform_ip);

  /* Properties */
  g_object_class_install_property (gobject_class, PROP_LABEL,
      g_param_spec_string ("label", "Label filepath",
          "File path to the label file",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_synap_overlay_init (GstSynapOverlay * synapoverlay)
{
  g_mutex_init (&synapoverlay->data_lock);

  /* Inference Pad */
  synapoverlay->inference_pad =
      gst_pad_new_from_static_template (&inference_sink_template,
      "inference_sink");
  gst_pad_set_chain_function (synapoverlay->inference_pad,
      GST_DEBUG_FUNCPTR (gst_synap_overlay_inference_sink_chain));
  gst_element_add_pad (GST_ELEMENT (synapoverlay), synapoverlay->inference_pad);
  gst_pad_set_active (synapoverlay->inference_pad, TRUE);

  /* Initialize parameters */
  synapoverlay->overlaydata = NULL;
  synapoverlay->label = NULL;
}

void
gst_synap_overlay_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (object);

  GST_DEBUG_OBJECT (synapoverlay, "set_property");

  switch (property_id) {
    case PROP_LABEL:
    {
      if (g_value_get_string (value) != NULL) {
        synapoverlay->label = g_value_dup_string (value);
        GST_INFO ("Label: %s", synapoverlay->label);
      }
    }
    break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_overlay_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (object);

  GST_DEBUG_OBJECT (synapoverlay, "get_property");

  switch (property_id) {
    case PROP_LABEL:
    {
      g_value_set_string (value, synapoverlay->label);
    }
    break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_overlay_dispose (GObject * object)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (object);

  GST_DEBUG_OBJECT (synapoverlay, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_synap_overlay_parent_class)->dispose (object);
}

void
gst_synap_overlay_finalize (GObject * object)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (object);

  GST_DEBUG_OBJECT (synapoverlay, "finalize");

  /* clean up object here */
  g_mutex_clear (&synapoverlay->data_lock);
  g_free (synapoverlay->label);
  G_OBJECT_CLASS (gst_synap_overlay_parent_class)->finalize (object);
}

static gboolean
gst_synap_overlay_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (trans);

  GST_DEBUG_OBJECT (synapoverlay, "set_caps");
  if (!gst_video_info_from_caps (&synapoverlay->videoinfo, incaps))
    return FALSE;

  return TRUE;
}

/* states */
static gboolean
gst_synap_overlay_start (GstBaseTransform * trans)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (trans);

  GST_DEBUG_OBJECT (synapoverlay, "start");

  if (synapoverlay->label != NULL) {
    synapoverlay->labelinfo = new LabelInfo ();
    g_return_val_if_fail (synapoverlay->labelinfo != NULL, FALSE);

    if (!synapoverlay->labelinfo->init (synapoverlay->label)) {
      GST_ERROR ("Missing label file");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
gst_synap_overlay_stop (GstBaseTransform * trans)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (trans);

  GST_DEBUG_OBJECT (synapoverlay, "stop");
  if (synapoverlay->labelinfo != NULL)
    delete synapoverlay->labelinfo;
  return TRUE;
}

static GstFlowReturn
gst_synap_overlay_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (trans);

  GST_DEBUG_OBJECT (synapoverlay, "transform_ip");

  buf = gst_buffer_make_writable (buf);
  if (synapoverlay->overlaydata != NULL) {
    gst_synap_overlay_render (trans, buf);
  }
  return GST_FLOW_OK;
}

static GstFlowReturn
gst_synap_overlay_inference_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (GST_PAD_PARENT (pad));

  GST_DEBUG_OBJECT (synapoverlay, "inference_sink_chain");
  GstSynapMeta *meta = gst_buffer_get_synap_meta (buffer);
  if (meta != NULL && meta->data != NULL) {
    /* Clear existing one */
    g_mutex_lock (&synapoverlay->data_lock);
    if (synapoverlay->overlaydata != NULL) {
      if (synapoverlay->mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER)
        delete ((Classifier::Result*)synapoverlay->overlaydata);
      else if (synapoverlay->mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR)
        delete ((Detector::Result*)synapoverlay->overlaydata);
      else
        GST_ERROR ("Invalid mode - Should not reach here");
    }
    synapoverlay->overlaydata = meta->data;
    synapoverlay->mode = meta->postproc_mode;
    synapoverlay->width = meta->width;
    synapoverlay->height = meta->height;
    synapoverlay->confthreshold = meta->confthreshold;
    g_mutex_unlock (&synapoverlay->data_lock);
  }
  gst_clear_buffer (&buffer);
  return GST_FLOW_OK;
}
