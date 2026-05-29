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

#define IC_RENDER_CTRL_LOW_THRESHOLD_DEFAULT        (8.5)
#define IC_RENDER_CTRL_PERSISTENCE_FRAMES_DEFAULT   (3)

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

static void pushOverlayData (GstSynapOverlay *synapoverlay,
    GstSynapMeta *meta, GstClockTime pts);
static OverlayData popOverlayData (GstSynapOverlay *synapoverlay,
    GstClockTime pts);

enum
{
  PROP_0,
  PROP_LABEL,
  PROP_RENDERCTRL_LOW_THRESHOLD,
  PROP_RENDERCTRL_PERSISTENCE_FRAMES,
  PROP_PREDICTIVE,
  PROP_EMA_ALPHA,
  PROP_EXTRAPOLATE_MAX_MS,
  PROP_SMOOTH_VELOCITY,
  PROP_VEL_ALPHA,
  PROP_SMOOTH_DRAW,
  PROP_DRAW_EMA_ALPHA,
  PROP_SEGMENTATION_THRESHOLD,
  PROP_SEGMENTATION_ALPHA
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

  g_object_class_install_property (gobject_class, PROP_RENDERCTRL_LOW_THRESHOLD,
      g_param_spec_float ("lowthreshold","Lowthreshold",
          "Render control Low threshold for hiding", 0, G_MAXFLOAT, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_RENDERCTRL_PERSISTENCE_FRAMES,
      g_param_spec_int ("persistenceframes", "PersistenceFrames", "Render control number of frames to persist", 0,
          G_MAXINT, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /* Prediction controls */
  g_object_class_install_property (gobject_class, PROP_PREDICTIVE,
      g_param_spec_boolean ("predictive", "PredictiveOverlay",
          "Predict boxes forward to display PTS using constant-velocity model",
          TRUE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_EMA_ALPHA,
      g_param_spec_double ("ema-alpha", "EMA Alpha",
          "Residual EMA alpha (0..1) applied to predicted boxes",
          0.0, 1.0, 0.5,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_EXTRAPOLATE_MAX_MS,
      g_param_spec_int ("extrapolate-max-ms", "ExtrapolateMaxMilliseconds",
          "Cap forward prediction to this many milliseconds (0 = unlimited)",
          0, 2000, 250,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_SMOOTH_VELOCITY,
      g_param_spec_boolean ("smooth-velocity", "SmoothVelocity",
          "Low-pass the estimated box velocity (lag-minimizing smoothing)",
          TRUE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_VEL_ALPHA,
      g_param_spec_double ("vel-alpha", "Velocity Alpha",
          "EMA alpha (0..1) for velocity smoothing",
          0.0, 1.0, 0.5,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_SMOOTH_DRAW,
      g_param_spec_boolean ("smooth-draw", "SmoothDraw",
          "EMA on predicted boxes to reduce visual flicker",
          TRUE, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_DRAW_EMA_ALPHA,
      g_param_spec_double ("draw-ema-alpha", "Draw EMA Alpha",
          "EMA alpha (0..1) across drawn frames",
          0.0, 1.0, 0.4,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_SEGMENTATION_THRESHOLD,
      g_param_spec_float ("segmentation-threshold", "Segmentation Threshold",
          "Threshold for segmentation mask alpha",
          0.0, 5.0, 2.0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_SEGMENTATION_ALPHA,
      g_param_spec_float ("segmentation-alpha", "Segmentation Alpha",
          "Transparency (alpha) for segmentation masks",
          0.0, 1.0, 0.3,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
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
  synapoverlay->label = NULL;
  synapoverlay->renderctrlLowThreshold = IC_RENDER_CTRL_LOW_THRESHOLD_DEFAULT;
  synapoverlay->renderctrlPersistenceFrames = IC_RENDER_CTRL_PERSISTENCE_FRAMES_DEFAULT;
  synapoverlay->queue = queue<OverlayData>();
  strncpy(synapoverlay->icState.label, "", sizeof( synapoverlay->icState.label));
  synapoverlay->icState.confidence = 0.0;
  synapoverlay->icState.persistence_count = 0;
  synapoverlay->predictive = TRUE;
  synapoverlay->ema_alpha = 0.5;
  synapoverlay->extrapolate_max_ms = 250;
  synapoverlay->prev_det_copy = NULL;
  synapoverlay->curr_det_copy = NULL;
  synapoverlay->prev_det_pts = GST_CLOCK_TIME_NONE;
  synapoverlay->curr_det_pts = GST_CLOCK_TIME_NONE;

  /* Velocity smoothing defaults */
  synapoverlay->smooth_velocity = TRUE;
  synapoverlay->vel_alpha = 0.5;
  synapoverlay->vel_state.clear();
  synapoverlay->smooth_draw = FALSE;
  synapoverlay->draw_ema_alpha = 0.3;
  synapoverlay->last_draw_copy = NULL;
  synapoverlay->last_draw_pts = GST_CLOCK_TIME_NONE;

  /* Segmentation masking defaults */
  synapoverlay->segmentation_threshold = 2.0;
  synapoverlay->segmentation_alpha = 0.3;
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

    case PROP_RENDERCTRL_LOW_THRESHOLD:
    {
      synapoverlay->renderctrlLowThreshold = g_value_get_float (value);
      GST_INFO ("Render Control Low Threshold: %f", synapoverlay->renderctrlLowThreshold);
    }
    break;

    case PROP_RENDERCTRL_PERSISTENCE_FRAMES:
    {
      synapoverlay->renderctrlPersistenceFrames = g_value_get_int (value);
      GST_INFO ("Render Control Low Threshold: %d", synapoverlay->renderctrlPersistenceFrames);
    }
    break;

    case PROP_PREDICTIVE:
    {
      synapoverlay->predictive = g_value_get_boolean (value);
    }
    break;

    case PROP_EMA_ALPHA:
    {
      synapoverlay->ema_alpha = g_value_get_double (value);
      GST_INFO ("EMA alpha: %f", synapoverlay->ema_alpha);
    }
    break;

    case PROP_EXTRAPOLATE_MAX_MS:
    {
      synapoverlay->extrapolate_max_ms = g_value_get_int (value);
      GST_INFO ("Extrapolate max ms: %d", synapoverlay->extrapolate_max_ms);
    }
    break;

    case PROP_SMOOTH_VELOCITY:
    {
      synapoverlay->smooth_velocity = g_value_get_boolean (value);
    }
    break;

    case PROP_VEL_ALPHA:
    {
      synapoverlay->vel_alpha = g_value_get_double (value);
      GST_INFO ("Vel alpha: %f", synapoverlay->vel_alpha);
    }
    break;

    case PROP_SMOOTH_DRAW:
    {
      synapoverlay->smooth_draw = g_value_get_boolean (value);
    }
    break;

    case PROP_DRAW_EMA_ALPHA:
    {
      synapoverlay->draw_ema_alpha = g_value_get_double (value);
      GST_INFO ("Draw EMA alpha: %f", synapoverlay->draw_ema_alpha);
    }
    break;

    case PROP_SEGMENTATION_THRESHOLD:
    {
      synapoverlay->segmentation_threshold = g_value_get_float (value);
      GST_INFO ("Segmentation threshold: %f", synapoverlay->segmentation_threshold);
    }
    break;

    case PROP_SEGMENTATION_ALPHA:
    {
      synapoverlay->segmentation_alpha = g_value_get_float (value);
      GST_INFO ("Segmentation alpha: %f", synapoverlay->segmentation_alpha);
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

    case PROP_RENDERCTRL_LOW_THRESHOLD:
    {
      g_value_set_float (value, synapoverlay->renderctrlLowThreshold);
    }
    break;

    case PROP_RENDERCTRL_PERSISTENCE_FRAMES:
    {
      g_value_set_int (value, synapoverlay->renderctrlPersistenceFrames);
    }
    break;

    case PROP_PREDICTIVE:
    {
      g_value_set_boolean (value, synapoverlay->predictive);
    }
    break;

    case PROP_EMA_ALPHA:
    {
      g_value_set_double (value, synapoverlay->ema_alpha);
    }
    break;

    case PROP_EXTRAPOLATE_MAX_MS:
    {
      g_value_set_int (value, synapoverlay->extrapolate_max_ms);
    }
    break;

    case PROP_SMOOTH_VELOCITY:
    {
      g_value_set_boolean (value, synapoverlay->smooth_velocity);
    }
    break;

    case PROP_VEL_ALPHA:
    {
      g_value_set_double (value, synapoverlay->vel_alpha);
    }
    break;

    case PROP_SMOOTH_DRAW:
    {
      g_value_set_boolean (value, synapoverlay->smooth_draw);
    }
    break;

    case PROP_DRAW_EMA_ALPHA:
    {
      g_value_set_double (value, synapoverlay->draw_ema_alpha);
    }
    break;

    case PROP_SEGMENTATION_THRESHOLD:
    {
      g_value_set_float (value, synapoverlay->segmentation_threshold);
    }
    break;

    case PROP_SEGMENTATION_ALPHA:
    {
      g_value_set_float (value, synapoverlay->segmentation_alpha);
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

  if (synapoverlay->prev_det_copy) { delete synapoverlay->prev_det_copy; synapoverlay->prev_det_copy = NULL; }
  if (synapoverlay->curr_det_copy) { delete synapoverlay->curr_det_copy; synapoverlay->curr_det_copy = NULL; }
  if (synapoverlay->last_draw_copy) { delete synapoverlay->last_draw_copy; synapoverlay->last_draw_copy = NULL; }
  synapoverlay->vel_state.clear();

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
  g_mutex_lock (&synapoverlay->data_lock);
  while (!synapoverlay->queue.empty()) {
    OverlayData overlay_data = synapoverlay->queue.front();
    if (overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
      if (overlay_data.synapmeta.type.classifier.data != NULL)
        delete ((Classifier::Result*)overlay_data.synapmeta.type.classifier.data);
    } else if (overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
      if (overlay_data.synapmeta.type.detector.data != NULL)
        delete ((Detector::Result*)overlay_data.synapmeta.type.detector.data);
    } else {
      GST_ERROR ("Invalid mode - Should not reach here");
    }
    synapoverlay->queue.pop();
  }
  g_mutex_unlock (&synapoverlay->data_lock);
  return TRUE;
}

static GstFlowReturn
gst_synap_overlay_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (trans);

  GST_DEBUG_OBJECT (synapoverlay, "transform_ip");

  buf = gst_buffer_make_writable (buf);
  g_mutex_lock (&synapoverlay->data_lock);
  OverlayData overlay_data = popOverlayData (synapoverlay, buf->pts);
  if (overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
    if (overlay_data.synapmeta.type.classifier.data != NULL)
      synapoverlay->overlay_data = overlay_data;
  } else if (overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
    if (overlay_data.synapmeta.type.detector.data != NULL)
      synapoverlay->overlay_data = overlay_data;
  } else {
    GST_ERROR ("Invalid mode - Should not reach here");
  }
  g_mutex_unlock (&synapoverlay->data_lock);
  if (synapoverlay->overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
    if (synapoverlay->overlay_data.synapmeta.type.classifier.data != NULL)
      gst_synap_overlay_render (trans, buf);
  } else if (synapoverlay->overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
    if (synapoverlay->overlay_data.synapmeta.type.detector.data != NULL) {
      gst_synap_overlay_render (trans, buf);
    }
  } else {
    GST_ERROR ("Invalid mode - Should not reach here");
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

  g_mutex_lock (&synapoverlay->data_lock);
  if (meta->mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
    if (meta->type.classifier.data != NULL)
      pushOverlayData (synapoverlay, meta, buffer->pts);
  } else if (meta->mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
    if (meta->type.detector.data != NULL) {
      pushOverlayData (synapoverlay, meta, buffer->pts);
      synapoverlay->prev_det_pts = synapoverlay->curr_det_pts;
      if (synapoverlay->prev_det_copy) { delete synapoverlay->prev_det_copy; synapoverlay->prev_det_copy = NULL; }
      synapoverlay->prev_det_copy = synapoverlay->curr_det_copy;
      synapoverlay->curr_det_pts = buffer->pts;
      synapoverlay->curr_det_copy = new Detector::Result (*((Detector::Result*) meta->type.detector.data));
    }
  } else {
    GST_ERROR ("Invalid mode - Should not reach here");
  }
  g_mutex_unlock (&synapoverlay->data_lock);

  if (!gst_buffer_remove_synap_meta (buffer)) {
    GST_ERROR ("Failed to remove the metadata from the buffer: %p", buffer);
    return GST_FLOW_ERROR;
  }

  gst_clear_buffer (&buffer);
  return GST_FLOW_OK;
}

static void
pushOverlayData (GstSynapOverlay *synapoverlay,
    GstSynapMeta *meta, GstClockTime pts)
{
  OverlayData overlay_data;
  overlay_data.synapmeta = *meta;
  overlay_data.pts = pts;
  synapoverlay->queue.push(overlay_data);
}

static OverlayData
popOverlayData (GstSynapOverlay *synapoverlay, GstClockTime pts)
{
  while (!synapoverlay->queue.empty()) {
    OverlayData overlay_data = synapoverlay->queue.front();
    if (synapoverlay->queue.size() == 1 || pts <= overlay_data.pts) {
      GST_DEBUG ("Ret.PTS = [%lu] Inc.PTS = [%lu]", overlay_data.pts, pts);
      return overlay_data;
    } else if (pts > overlay_data.pts) {
      if (overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
        if (overlay_data.synapmeta.type.classifier.data != NULL)
          delete ((Classifier::Result*)overlay_data.synapmeta.type.classifier.data);
      } else if (overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
        if (overlay_data.synapmeta.type.detector.data != NULL)
          delete ((Detector::Result*)overlay_data.synapmeta.type.detector.data);
      } else {
        GST_ERROR ("Invalid mode - Should not reach here");
      }
      synapoverlay->queue.pop();
    }
  }
  return (OverlayData){
    .synapmeta = { .mode = GST_SYNAP_POSTPROCESSING_MODE_NONE },
    .pts = GST_CLOCK_TIME_NONE
  };
}
