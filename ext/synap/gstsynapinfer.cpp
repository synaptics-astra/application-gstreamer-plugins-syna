/* GStreamer SyNAPInfer
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
 * SECTION:element-gstsynapinfer
 *
 * GStreamer Plugin to do AI processing with Synaptics Neural Network Acceleration and Processing component (SyNAP)
 *
 * <refsect2>
 * <title>For AI inference + overlay</title>
 * |[
 * gst-launch-1.0 v4l2src device=/dev/xxxx ! video/x-raw,<prop> ! videoconvert ! tee name=t_data \
 *  t_data. ! queue ! synapoverlay name=overlay label=<file> ! videoconvert ! waylandsink \
 *  t_data. ! queue ! videoconvert ! videoscale ! video/x-raw,width=xx,height=xx,format=RGB  ! \
 *  synapinfer model=<file> mode=detector frameinterval=3 ! overlay.inference_sink
 * ]|
 * Gstreamer pipeline to do inferencing with synapinfer and do the overlay drawing with synapoverlay
 *
 * <title>For AI inference + application handling inference overlay </title>
 * |[
 * gst-launch-1.0 filesrc location=<> ! decodebin ! videoconvert ! video/x-raw,format=RGB ! tee name=t_data \
 * t_data. ! queue ! videoconvert ! videoscale ! video/x-raw,width=xxx,height=xxx,format=RGB ! \
 * synapinfer model=<file> mode=classifier frameinterval=3 output=json ! appsink name=synap_sink \
 * t_data. ! queue ! textoverlay name=ic_label font-desc=Sans,24 ! videoconvert ! waylandsink
 * ]|
 * Gstreamer pipeline to do inferencing with synapinfer and return inference as json string. Application to
 * handle overlay part with inferences.
 *
 * </refsect2>
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include "gstsynapinfer.h"
#include "synapmeta.h"

GST_DEBUG_CATEGORY_STATIC (gst_synap_infer_debug_category);
#define GST_CAT_DEFAULT gst_synap_infer_debug_category

/* prototypes */
static void gst_synap_infer_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_synap_infer_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_synap_infer_dispose (GObject * object);
static void gst_synap_infer_finalize (GObject * object);

static gboolean gst_synap_infer_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean gst_synap_infer_start (GstBaseTransform * trans);
static gboolean gst_synap_infer_stop (GstBaseTransform * trans);
static GstFlowReturn gst_synap_infer_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

static gboolean gst_synap_infer_postproc_classifier (GstBaseTransform * trans,
    GstBuffer * buf);
static gboolean gst_synap_infer_postproc_detector (GstBaseTransform * trans,
    GstBuffer * buf);

enum
{
  PROP_0,
  PROP_MODEL,
  PROP_MODE,
  PROP_OUTPUT,
  PROP_CONFIDENCE_THRESHOLD,
  PROP_NUM_INFERENCE,
  PROP_FRAME_INTERVAL,
};

#define GST_SYNAP_DEFAULT_INFERENCE_CLASSIFIER  (2)
#define GST_SYNAP_DEFAULT_THRESHOLD_CLASSIFIER  (11.0)
#define GST_SYNAP_DEFAULT_INFERENCE_DETECTOR    (5)
#define GST_SYNAP_DEFAULT_THRESHOLD_DETECTOR    (0.5)

/* pad templates */

static GstStaticPadTemplate gst_synap_infer_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ RGB }"))
    );

static GstStaticPadTemplate gst_synap_infer_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ RGB }"))
    );


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstSynapInfer, gst_synap_infer,
    GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_synap_infer_debug_category, "synapinfer", 0,
        "debug category for synapinfer element"));
GST_ELEMENT_REGISTER_DEFINE (synapinfer, "synapinfer", GST_RANK_NONE,
    GST_TYPE_SYNAP_INFER);

static void
gst_synap_infer_class_init (GstSynapInferClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_synap_infer_src_template);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_synap_infer_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Plugin for Synaptics AI", "Generic/Video",
      "GStreamer layer for Synaptics Neural Network Acceleration and Processing component",
      "https://www.synaptics.com/");

  gobject_class->set_property = gst_synap_infer_set_property;
  gobject_class->get_property = gst_synap_infer_get_property;
  gobject_class->dispose = gst_synap_infer_dispose;
  gobject_class->finalize = gst_synap_infer_finalize;
  base_transform_class->set_caps = GST_DEBUG_FUNCPTR (gst_synap_infer_set_caps);
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_synap_infer_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_synap_infer_stop);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_synap_infer_transform_ip);

  /* Properties */
  g_object_class_install_property (gobject_class, PROP_MODEL,
      g_param_spec_string ("model", "Model filepath",
          "File path to the model file",
          NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_string ("mode", "Mode", "Post Processing mode", NULL,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_OUTPUT,
      g_param_spec_string ("output", "Output",
          "Output as overlay or json strings", NULL,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_CONFIDENCE_THRESHOLD,
      g_param_spec_float ("threshold", "ConfidenceThreshold",
          "Confidence threshold for inferences", 0, G_MAXFLOAT, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_NUM_INFERENCE,
      g_param_spec_int ("numinference", "NumberOfInference",
          "Max number of inferences", 0, G_MAXINT, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_FRAME_INTERVAL,
      g_param_spec_int ("frameinterval", "FrameInterval", "Frame Interval", 0,
          G_MAXINT, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_synap_infer_init (GstSynapInfer * synapinfer)
{
  static gsize res = FALSE;
  static const gchar *tags[] = { NULL };
  if (g_once_init_enter (&res)) {
    gst_meta_register_custom ("GstSynapStrMeta", tags, NULL, NULL, NULL);
    g_once_init_leave (&res, TRUE);
  }
  /* Initialize parameters */
  synapinfer->model = NULL;
  synapinfer->mode = NULL;
  synapinfer->postproc_mode = GST_SYNAP_POSTPROCESSING_MODE_NONE;
  synapinfer->frame_interval = 0;
  synapinfer->numinference = -1;
  synapinfer->confthreshold = -1;
  synapinfer->output = NULL;
  synapinfer->outputmode = GST_SYNAP_OUTPUT_MODE_OVERLAY;
}

void
gst_synap_infer_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (object);

  GST_DEBUG_OBJECT (synapinfer, "set_property");
  g_return_if_fail (synapinfer != NULL);
  g_return_if_fail (GST_IS_SYNAP_INFER (synapinfer));

  switch (property_id) {
    case PROP_MODEL:
    {
      if (g_value_get_string (value) != NULL) {
        synapinfer->model = g_value_dup_string (value);
        GST_INFO ("Model: %s", synapinfer->model);
      }
    }
    break;

    case PROP_MODE:
    {
      if (g_value_get_string (value) != NULL) {
        synapinfer->mode = g_value_dup_string (value);
        GST_INFO ("Mode: %s", synapinfer->mode);
      }
    }
    break;

    case PROP_OUTPUT:
    {
      if (g_value_get_string (value) != NULL) {
        synapinfer->output = g_value_dup_string (value);
        GST_INFO ("Output: %s", synapinfer->output);
      }
    }
    break;

    case PROP_CONFIDENCE_THRESHOLD:
    {
      synapinfer->confthreshold = g_value_get_float (value);
      GST_INFO ("Confidence Threshold: %f", synapinfer->confthreshold);
    }
    break;

    case PROP_NUM_INFERENCE:
    {
      synapinfer->numinference = g_value_get_int (value);
      GST_INFO ("Number of inferences: %d", synapinfer->numinference);
    }
    break;

    case PROP_FRAME_INTERVAL:
    {
      synapinfer->frame_interval = g_value_get_int (value);
      synapinfer->frame_counter = synapinfer->frame_interval;
      GST_INFO ("Frame Interval: %d", synapinfer->frame_interval);
    }
    break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_infer_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (object);

  GST_DEBUG_OBJECT (synapinfer, "get_property");
  g_return_if_fail (synapinfer != NULL);
  g_return_if_fail (GST_IS_SYNAP_INFER (synapinfer));

  switch (property_id) {
    case PROP_MODEL:
    {
      g_value_set_string (value, synapinfer->model);
    }
    break;

    case PROP_MODE:
    {
      g_value_set_string (value, synapinfer->mode);
    }
    break;

    case PROP_OUTPUT:
    {
      g_value_set_string (value, synapinfer->output);
    }
    break;

    case PROP_CONFIDENCE_THRESHOLD:
    {
      g_value_set_float (value, synapinfer->confthreshold);
    }
    break;

    case PROP_NUM_INFERENCE:
    {
      g_value_set_int (value, synapinfer->numinference);
    }
    break;

    case PROP_FRAME_INTERVAL:
    {
      g_value_set_int (value, synapinfer->frame_interval);
    }
    break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_infer_dispose (GObject * object)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (object);

  GST_DEBUG_OBJECT (synapinfer, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_synap_infer_parent_class)->dispose (object);
}

void
gst_synap_infer_finalize (GObject * object)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (object);

  GST_DEBUG_OBJECT (synapinfer, "finalize");
  g_return_if_fail (synapinfer != NULL);
  g_return_if_fail (GST_IS_SYNAP_INFER (synapinfer));

  /* clean up object here */
  g_free (synapinfer->model);
  g_free (synapinfer->mode);

  G_OBJECT_CLASS (gst_synap_infer_parent_class)->finalize (object);
}

static gboolean
gst_synap_infer_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (trans);

  GST_DEBUG_OBJECT (synapinfer, "set_caps");

  if (!gst_video_info_from_caps (&synapinfer->videoinfo, incaps))
    return FALSE;

  return TRUE;
}

/* states */
static gboolean
gst_synap_infer_start (GstBaseTransform * trans)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (trans);

  GST_DEBUG_OBJECT (synapinfer, "start");
  g_return_val_if_fail (synapinfer != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_INFER (synapinfer), FALSE);

  /* Validate parameters */
  if (synapinfer->model == NULL) {
    GST_ERROR ("Invalid Model Name");
    return FALSE;
  }
  if (synapinfer->mode != NULL) {
    if (g_strcmp0 (synapinfer->mode, "classifier") == 0) {
      GST_INFO ("Post-Processing Mode: Classifier");
      synapinfer->postproc_mode = GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER;
    } else if (g_strcmp0 (synapinfer->mode, "detector") == 0) {
      GST_INFO ("Post-Processing Mode: Detector");
      synapinfer->postproc_mode = GST_SYNAP_POSTPROCESSING_MODE_DETECTOR;
    }
  }
  if (synapinfer->output != NULL) {
    if (g_strcmp0 (synapinfer->output, "overlay") == 0) {
      GST_INFO ("Output Mode: Overlay");
      synapinfer->outputmode = GST_SYNAP_OUTPUT_MODE_OVERLAY;
    } else if (g_strcmp0 (synapinfer->output, "json") == 0) {
      GST_INFO ("Output Mode: JSON");
      synapinfer->outputmode = GST_SYNAP_OUTPUT_MODE_JSON;
    }
  }

  /* Create SyNAP objects */
  synapinfer->network = new Network ();
  g_return_val_if_fail (synapinfer->network != NULL, FALSE);

  synapinfer->preprocessor = new Preprocessor ();
  g_return_val_if_fail (synapinfer->preprocessor != NULL, FALSE);

  /* Load Model */
  if (!synapinfer->network->load_model (synapinfer->model, "")) {
    GST_ERROR ("Model cannot be loaded");
    return FALSE;
  }

  if (synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
    /* Classifier */
    synapinfer->numinference = (synapinfer->numinference == -1) ? GST_SYNAP_DEFAULT_INFERENCE_CLASSIFIER : synapinfer->numinference;
    synapinfer->confthreshold = (synapinfer->confthreshold == -1) ? GST_SYNAP_DEFAULT_THRESHOLD_CLASSIFIER : synapinfer->confthreshold;
    synapinfer->classifier = new Classifier (synapinfer->numinference);
    g_return_val_if_fail (synapinfer->classifier != NULL, FALSE);
  } else if (synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
    /* Detector */
    synapinfer->numinference = (synapinfer->numinference == -1) ? GST_SYNAP_DEFAULT_INFERENCE_DETECTOR : synapinfer->numinference;
    synapinfer->confthreshold = (synapinfer->confthreshold == -1) ? GST_SYNAP_DEFAULT_THRESHOLD_DETECTOR : synapinfer->confthreshold;
    synapinfer->detector = new Detector (synapinfer->confthreshold, synapinfer->numinference, true, 0.5, true);
    g_return_val_if_fail (synapinfer->detector != NULL, FALSE);
  }
  GST_INFO ("Max Inferences: %d\n Confidence Threshold: %f\n", synapinfer->numinference, synapinfer->confthreshold);
  return TRUE;
}

static gboolean
gst_synap_infer_stop (GstBaseTransform * trans)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (trans);

  GST_DEBUG_OBJECT (synapinfer, "stop");
  g_return_val_if_fail (synapinfer != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_INFER (synapinfer), FALSE);

  if (synapinfer->network != NULL)
    delete synapinfer->network;
  if (synapinfer->preprocessor != NULL)
    delete synapinfer->preprocessor;
  if (synapinfer->classifier != NULL)
    delete synapinfer->classifier;
  if (synapinfer->detector != NULL)
    delete synapinfer->detector;
  return TRUE;
}

static GstFlowReturn
gst_synap_infer_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (trans);

  GST_DEBUG_OBJECT (synapinfer, "transform_ip");
  g_return_val_if_fail (synapinfer != NULL, GST_FLOW_ERROR);
  g_return_val_if_fail (GST_IS_SYNAP_INFER (synapinfer), GST_FLOW_ERROR);
  g_return_val_if_fail (buf != NULL, GST_FLOW_ERROR);

  /* Check if inference to be run on the frame */
  if (synapinfer->frame_counter > 0) {
    synapinfer->frame_counter--;
    return GST_BASE_TRANSFORM_FLOW_DROPPED;
  } else if (synapinfer->frame_counter == 0) {
    synapinfer->frame_counter = synapinfer->frame_interval;
  }

  /* Map input buffer */
  GstMapInfo map;
  gst_buffer_map (buf, &map, GST_MAP_READ);
  guint8 *raw = (guint8 *) map.data;
  g_return_val_if_fail (raw != NULL, GST_FLOW_ERROR);

  /* Process with SyNAP */
  Shape shape = { 1, GST_VIDEO_INFO_HEIGHT (&synapinfer->videoinfo),
    GST_VIDEO_INFO_WIDTH (&synapinfer->videoinfo), 3
  };
  InputData image (raw, map.size, synaptics::synap::InputType::image_8bits,
      shape, Layout::nhwc);
  if (image.empty ()) {
    GST_ERROR ("Error in assigning input data");
    return GST_FLOW_ERROR;
  }
  Rect assigned_rect;
  if (!synapinfer->preprocessor->assign (synapinfer->network->inputs, image, 0,
          &assigned_rect)) {
    GST_ERROR ("Error assigning input to tensor");
    return GST_FLOW_ERROR;
  }
  /* Execute inference */
  if (!synapinfer->network->predict ()) {
    gst_buffer_unmap (buf, &map);
    GST_ERROR ("Inference failed");
    return GST_FLOW_ERROR;
  }
  gst_buffer_unmap (buf, &map);

  /* Post Process output */
  gboolean postproc_result = FALSE;
  if (synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
    postproc_result = gst_synap_infer_postproc_classifier (trans, buf);
  } else if (synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
    postproc_result = gst_synap_infer_postproc_detector (trans, buf);
  }
  return (postproc_result ? GST_FLOW_OK : GST_FLOW_ERROR);
}

static gboolean
gst_synap_infer_postproc_classifier (GstBaseTransform * trans, GstBuffer * buf)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (trans);

  GST_DEBUG_OBJECT (synapinfer, "postproc_classifier");
  g_return_val_if_fail (synapinfer != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_INFER (synapinfer), FALSE);

  Classifier::Result * result = new (Classifier::Result);
  g_return_val_if_fail (result != NULL, FALSE);

  *result = synapinfer->classifier->process (synapinfer->network->outputs);
  if (!result->success) {
    GST_ERROR ("Classification failed");
    return FALSE;
  }

  if (synapinfer->outputmode == GST_SYNAP_OUTPUT_MODE_OVERLAY) {
    gst_buffer_add_synap_meta (buf, result, synapinfer->postproc_mode,
        GST_VIDEO_INFO_WIDTH (&synapinfer->videoinfo),
        GST_VIDEO_INFO_HEIGHT (&synapinfer->videoinfo),
        synapinfer->confthreshold);
  }
  else if (synapinfer->outputmode == GST_SYNAP_OUTPUT_MODE_JSON) {
    std::string resstr = to_json_str (*result);
    GstCustomMeta *meta = gst_buffer_add_custom_meta (buf, "GstSynapStrMeta");
    if (meta) {
      GstStructure *s = gst_custom_meta_get_structure (meta);
      if (s) {
        gst_structure_set (s, "result", G_TYPE_STRING, resstr.c_str (), NULL);
      }
    }
    delete result;
  }
  else {
    delete result;
  }
  return TRUE;
}

static gboolean
gst_synap_infer_postproc_detector (GstBaseTransform * trans, GstBuffer * buf)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (trans);

  GST_DEBUG_OBJECT (synapinfer, "postproc_detector");
  g_return_val_if_fail (synapinfer != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_INFER (synapinfer), FALSE);

  Detector::Result * result = new (Detector::Result);
  g_return_val_if_fail (result != NULL, FALSE);

  *result = synapinfer->detector->process (synapinfer->network->outputs, Rect {{0, 0},
            {GST_VIDEO_INFO_WIDTH (&synapinfer->videoinfo),
             GST_VIDEO_INFO_HEIGHT (&synapinfer->videoinfo)}});
  if (!result->success) {
    GST_ERROR ("Detection failed");
    return FALSE;
  }
  if (synapinfer->outputmode == GST_SYNAP_OUTPUT_MODE_OVERLAY) {
    gst_buffer_add_synap_meta (buf, result, synapinfer->postproc_mode,
      GST_VIDEO_INFO_WIDTH (&synapinfer->videoinfo),
      GST_VIDEO_INFO_HEIGHT (&synapinfer->videoinfo),
      synapinfer->confthreshold);
  }
  else if (synapinfer->outputmode == GST_SYNAP_OUTPUT_MODE_JSON) {
    std::string resstr = to_json_str (*result);
    GstCustomMeta *meta = gst_buffer_add_custom_meta (buf, "GstSynapStrMeta");
    if (meta) {
      GstStructure *s = gst_custom_meta_get_structure (meta);
      if (s) {
        gst_structure_set (s, "result", G_TYPE_STRING, resstr.c_str (), NULL);
      }
    }
    delete result;
  }
  else {
    delete result;
  }
  return TRUE;
}
