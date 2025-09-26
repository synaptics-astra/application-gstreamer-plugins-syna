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
static gboolean gst_synap_infer_preproc_recognition (GstBaseTransform * trans,
    GstBuffer * buf, InputData &image);
static gboolean gst_synap_infer_embeddingproc_recognition (GstBaseTransform * trans,
    GstBuffer * buf, InputData &image);

static gboolean gst_synap_infer_head_pose (const Rect& face_rect,
    const std::vector<Dim2d>& landmarks);
static cv::Mat gst_synap_infer_get_affine_transform (const vector<Dim2d>& src_points);
static InputData gst_synap_infer_align_face (const InputData& input_image,
    const Rect& face_rect, const std::vector<Dim2d>& landmarks);
static InputData gst_synap_infer_recognize_face (GstSynapInfer *infer,
    const InputData& input_image, const Rect& face_rect,
    const std::vector<Dim2d>& landmarks);

enum
{
  PROP_0,
  PROP_MODEL,
  PROP_MODE,
  PROP_OUTPUT,
  PROP_CONFIDENCE_THRESHOLD,
  PROP_NUM_INFERENCE,
  PROP_FRAME_INTERVAL,
  PROP_INFER_STATISTICS,
};

#define GST_SYNAP_DEFAULT_INFERENCE_CLASSIFIER             (2)
#define GST_SYNAP_DEFAULT_THRESHOLD_CLASSIFIER             (11.0)
#define GST_SYNAP_DEFAULT_INFERENCE_DETECTOR               (5)
#define GST_SYNAP_DEFAULT_THRESHOLD_DETECTOR               (0.5)
#define GST_SYNAP_DEFAULT_SIDE_FACE_THRESHOLD_RECOGNITION  (0.6)

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

   g_object_class_install_property (gobject_class, PROP_INFER_STATISTICS,
      g_param_spec_boolean ("inferstatistics", "Synap Inference Statistics",
        "Enable logging statistics", FALSE, G_PARAM_READWRITE));
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
  synapinfer->infer_statistics_flag = FALSE;
  synapinfer->total_infer_time = 0;
  synapinfer->infer_count = 0;
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

    case PROP_INFER_STATISTICS:
    {
      synapinfer->infer_statistics_flag = g_value_get_boolean(value);
      GST_INFO("Inference statistics: %d", synapinfer->infer_statistics_flag);
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

    case PROP_INFER_STATISTICS:
    {
      g_value_set_boolean (value, synapinfer->infer_statistics_flag);
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
    } else if (g_strcmp0 (synapinfer->mode, "recognition") == 0) {
      GST_INFO ("Post-Processing Mode: Face Recognition");
      synapinfer->postproc_mode = GST_SYNAP_POSTPROCESSING_MODE_RECOGNITION;
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
  } else if (synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR ||
      synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_RECOGNITION) {
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

  if (synapinfer->infer_statistics_flag && synapinfer->infer_count > 0) {
    GstClockTime avg_infer_time_ns = synapinfer->total_infer_time / synapinfer->infer_count;
    double avg_infer_time_ms = (double) avg_infer_time_ns / 1000000.0;
    g_print("Average Inference Time: %.3f ms\n", avg_infer_time_ms);
  }

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
  if (synapinfer->postproc_mode != GST_SYNAP_POSTPROCESSING_MODE_RECOGNITION) {
    if (!synapinfer->preprocessor->assign (synapinfer->network->inputs, image, 0,
          &assigned_rect)) {
      GST_ERROR ("Error assigning input to tensor");
      return GST_FLOW_ERROR;
    }
  } else {
    if (!gst_synap_infer_preproc_recognition (trans, buf, image)) {
      GST_INFO ("No proper face found from pre-processing");
      gst_buffer_unmap (buf, &map);
      return GST_FLOW_OK;
    }
  }

  /* Track time for inference */
  GstClockTime start_time;
  if (synapinfer->infer_statistics_flag) {
    start_time = gst_clock_get_time(gst_element_get_clock(GST_ELEMENT(synapinfer)));
  }

  /* Execute inference */
  if (synapinfer->postproc_mode != GST_SYNAP_POSTPROCESSING_MODE_RECOGNITION) {
    if (!synapinfer->network->predict ()) {
      gst_buffer_unmap (buf, &map);
      GST_ERROR ("Inference failed");
      return GST_FLOW_ERROR;
    }
  }
  gst_buffer_unmap (buf, &map);

  /* Calculate inference time */
  if (synapinfer->infer_statistics_flag) {
    synapinfer->total_infer_time += (gst_clock_get_time(gst_element_get_clock(GST_ELEMENT(synapinfer))) - start_time);
    synapinfer->infer_count++;
  }

  /* Post Process output */
  gboolean postproc_result = FALSE;
  if (synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
    postproc_result = gst_synap_infer_postproc_classifier (trans, buf);
  } else if (synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
    postproc_result = gst_synap_infer_postproc_detector (trans, buf);
  } else if (synapinfer->postproc_mode == GST_SYNAP_POSTPROCESSING_MODE_RECOGNITION) {
    postproc_result = gst_synap_infer_embeddingproc_recognition (trans, buf, image);
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
    gst_buffer_add_classifier_synap_meta (buf, GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER, result,
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
    gst_buffer_add_detector_synap_meta (buf, GST_SYNAP_POSTPROCESSING_MODE_DETECTOR, result,
        GST_VIDEO_INFO_WIDTH (&synapinfer->videoinfo),
        GST_VIDEO_INFO_HEIGHT (&synapinfer->videoinfo));
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
gst_synap_infer_preproc_recognition (GstBaseTransform * trans, GstBuffer * buf, InputData &image)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (trans);

  GST_DEBUG_OBJECT (synapinfer, "preproc_recog");
  g_return_val_if_fail (synapinfer != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_INFER (synapinfer), FALSE);

  GstSynapMeta *meta = gst_buffer_get_synap_meta (buf);
  if (meta == NULL) {
    GST_ERROR ("Pre-Processing failed, meta is null");
    return FALSE;
  }

  Detector::Result * result = (Detector::Result *) meta->type.detector.data;
  if (result->items.empty()) {
    delete result;
    return FALSE;
  }

  std::vector<std::pair<uint32_t, double>> bounding_box_areas;
  int bounding_box_index = 0;
  for (const auto& item : result->items) {
    const auto& bounding_box = item.bounding_box;
    double bounding_box_area = bounding_box.size.x * bounding_box.size.y;
    std::pair<uint32_t, double> area_pair(bounding_box_index, bounding_box_area);
    bounding_box_areas.push_back(area_pair);
    bounding_box_index++;
  }

  sort(bounding_box_areas.begin(), bounding_box_areas.end(),
      [=](std::pair<uint32_t, double>& a, std::pair<uint32_t, double>& b) {
      return a.second > b.second;
      });

  synapinfer->recognition_items.clear();
  int total_size = 0;
  int j = 0;
  int max_size = bounding_box_areas.size();
  for (int i = 0; i < max_size; i++) {
    const auto& bounding_box = result->items[bounding_box_areas[i].first].bounding_box;
    double bounding_box_area = bounding_box.size.x * bounding_box.size.y;
    if (result->items[bounding_box_areas[i].first].confidence > 0.80 && bounding_box_area > 800) {
      synapinfer->recognition_items.push_back(result->items[bounding_box_areas[i].first]);
      auto detected_item = synapinfer->recognition_items[j];
      j += 1;
      total_size += detected_item.landmarks.size();
      synapinfer->detected_item_collection.push_back(detected_item);
    }
  }
  synapinfer->landmarks_2d_collection.reserve(total_size);
  max_size = synapinfer->detected_item_collection.size();
  for (int i = 0; i < max_size; i++) {
    std::vector<Dim2d> landmarks_2d;
    auto detected_item = synapinfer->recognition_items[i];
    std::transform(detected_item.landmarks.begin(), detected_item.landmarks.end(),
        std::back_inserter(landmarks_2d), [](const Landmark& landmark) {
        return Dim2d{landmark.x, landmark.y};
        });
    synapinfer->landmarks_2d_collection.push_back(landmarks_2d);
  }

  delete result;

  return TRUE;
}

static gboolean
gst_synap_infer_embeddingproc_recognition (GstBaseTransform * trans, GstBuffer * buf, InputData &image)
{
  GstSynapInfer *synapinfer = GST_SYNAP_INFER (trans);

  GST_DEBUG_OBJECT (synapinfer, "postproc_recog");
  g_return_val_if_fail (synapinfer != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_INFER (synapinfer), FALSE);

  GArray* origin_x = g_array_new(FALSE,FALSE, sizeof(gint));
  GArray* origin_y = g_array_new(FALSE,FALSE, sizeof(gint));
  GArray* size_x = g_array_new(FALSE,FALSE, sizeof(gint));
  GArray* size_y = g_array_new(FALSE,FALSE, sizeof(gint));

  Rect assigned_rect;
  GArray *out_collection = g_array_new(FALSE,FALSE, sizeof(guint8*));
  int max_size = synapinfer->detected_item_collection.size();
  for (int i = 0; i < max_size ; i++) {
    synapinfer->face_image = gst_synap_infer_recognize_face(synapinfer, image,
      synapinfer->detected_item_collection[i].bounding_box, synapinfer->landmarks_2d_collection[i]);
    if (!synapinfer->face_image.empty()) {
      if (!synapinfer->preprocessor->assign (synapinfer->network->inputs, synapinfer->face_image, 0,&assigned_rect)) {
        GST_ERROR ("Error assigning input to tensor");
        return FALSE;
      }
      if (!synapinfer->network->predict ()) {
        GST_ERROR ("Inference failed");
        return FALSE;
      }
      Tensor& out = synapinfer->network->outputs[0];
      guint8* original = (guint8*)out.data();
      gsize size = out.size();
      guint8* copy = (guint8*)(g_memdup2(original, size));
      g_array_append_val(out_collection, copy);

      g_array_append_val(origin_x, synapinfer->recognition_items[i].bounding_box.origin.x);
      g_array_append_val(origin_y, synapinfer->recognition_items[i].bounding_box.origin.y);
      g_array_append_val(size_x, synapinfer->recognition_items[i].bounding_box.size.x);
      g_array_append_val(size_y, synapinfer->recognition_items[i].bounding_box.size.y);
     }
  }
  synapinfer->landmarks_2d_collection.clear();
  synapinfer->detected_item_collection.clear();
  synapinfer->landmarks_2d_collection.shrink_to_fit();
  synapinfer->detected_item_collection.shrink_to_fit();
  if (out_collection->len == 0 ) {
    return TRUE;
  }

  if (!gst_buffer_remove_synap_meta (buf)) {
    GST_ERROR ("Failed to remove the metadata from the buffer: %p", buf);
    return FALSE;
  }

  gst_buffer_add_recognition_synap_meta (buf, synapinfer->postproc_mode,
      out_collection, origin_x, origin_y, size_x, size_y,
      GST_VIDEO_INFO_WIDTH (&synapinfer->videoinfo),
      GST_VIDEO_INFO_HEIGHT (&synapinfer->videoinfo));

  return TRUE;
}

static gboolean
gst_synap_infer_head_pose (const Rect& face_rect, const std::vector<Dim2d>& landmarks)
{
  if (face_rect.origin.x < 0 || face_rect.origin.y < 0 ||
      face_rect.size.x <= 0 || face_rect.size.y <= 0) {
    return FALSE;
  }

  std::vector<gdouble> x_coords;
  std::vector<gdouble> rotated_x;
  std::vector<gdouble> y_coords;
  std::vector<gdouble> rotated_y;

  for (const auto& point : landmarks) {
    x_coords.push_back(point.x);
    y_coords.push_back(point.y);
  }

  gdouble eye_dx = (x_coords[1] - x_coords[0]) > 1
    ? (x_coords[1] - x_coords[0]) : 1;
  gdouble eye_dy = y_coords[1] - y_coords[0];
  gdouble angle = atan2(eye_dy, eye_dx);
  gdouble cos_a = cos(angle);
  gdouble sin_a = sin(angle);

  for (guint i = 0; i < 5; i++) {
    gdouble temp_x = cos_a * x_coords[i] + sin_a * y_coords[i] + (1 - cos_a) *
      x_coords[2] / 2 - sin_a * y_coords[2] / 2;
    gdouble temp_y = -sin_a * x_coords[i] + cos_a * y_coords[i] + sin_a *
      x_coords[2] / 2 + (1 - cos_a) * y_coords[2] / 2;
    rotated_x.push_back(temp_x);
    rotated_y.push_back(temp_y);
  }

  gdouble total_dx = (rotated_x[1] - rotated_x[0] + rotated_x[4] - rotated_x[3]) / 2;
  gdouble total_dy = (rotated_y[3] - rotated_y[0] + rotated_y[4] - rotated_y[1]) / 2;
  gdouble nose_dx = (rotated_x[1] - rotated_x[2] + rotated_x[4] - rotated_x[2]) / 2;
  gdouble nose_dy = (rotated_y[3] - rotated_y[2] + rotated_y[4] - rotated_y[2]) / 2;

  gdouble yaw = 0;
  if (total_dx) {
    yaw = -90 + 90 / 0.5 * nose_dx / total_dx;
  }

  gdouble pitch = 0;
  if (total_dy) {
    pitch = -90 + 90 / 0.5 * nose_dy / total_dy;
  }

  gdouble roll = angle * 180 / M_PI;

  if (roll < -50 || roll > 50 || yaw < -200 || yaw > 200 || pitch < -30 || pitch > 30) {
    return FALSE;
  }

  return TRUE;
}

static cv::Mat
gst_synap_infer_get_affine_transform (const vector<Dim2d>& src_points)
{
  static float dst_points[5][2] = {
    {30.2946f + 8.0f, 51.6963f},
    {65.5318f + 8.0f, 51.5014f},
    {48.0252f + 8.0f, 71.7366f},
    {33.5493f + 8.0f, 92.3655f},
    {62.7299f + 8.0f, 92.2041f}
  };

  static const cv::Mat dst_mat (5, 2, CV_32FC1, dst_points);
  float src_points_data[5][2] = {
    {(float)src_points[0].x, (float)src_points[0].y},
    {(float)src_points[1].x, (float)src_points[1].y},
    {(float)src_points[2].x, (float)src_points[2].y},
    {(float)src_points[3].x, (float)src_points[3].y},
    {(float)src_points[4].x, (float)src_points[4].y}
  };

  cv::Mat src_mat (5, 2, CV_32FC1, src_points_data);
  cv::Mat affine_transform = cv::estimateAffine2D (src_mat, dst_mat);
  return affine_transform (cv::Rect (0, 0, 3, 2));
}

static InputData
gst_synap_infer_align_face (const InputData& input_image, const Rect& face_rect,
    const std::vector<Dim2d>& landmarks)
{
  if (input_image.empty()) return {};

  Dimensions image_dim = input_image.dimensions();
  if (!image_dim.w || !image_dim.h ||
      face_rect.origin.x < 0 || face_rect.origin.y < 0 ||
      face_rect.size.x <= 0 || face_rect.size.y <= 0) {
    return {};
  }

  if (face_rect.origin.x + face_rect.size.x > image_dim.w ||
      face_rect.origin.y + face_rect.size.y > image_dim.h) {
    return {};
  }

  int image_type = CV_8UC3;
  cv::Rect face_rect_cv(face_rect.origin.x, face_rect.origin.y,
      face_rect.size.x, face_rect.size.y);
  cv::Mat input_mat = cv::Mat(image_dim.h, image_dim.w, image_type,
      (uint8_t *)input_image.data());
  cv::Mat aligned_image;
  uint32_t target_size = 112;
  cv::Mat transform = gst_synap_infer_get_affine_transform(landmarks);
  cv::warpAffine(input_mat, aligned_image, transform,
      cv::Size(target_size, target_size), 1, 0, 0);

  size_t aligned_image_size =
    aligned_image.rows * aligned_image.cols * aligned_image.elemSize();
  vector<uint8_t> aligned_image_buf(aligned_image.data,
      aligned_image.data + aligned_image_size);
  Shape aligned_shape = {1, aligned_image.rows, aligned_image.cols, 3};
  InputData aligned_data(std::move(aligned_image_buf),
      InputType::image_8bits, aligned_shape);
  return aligned_data;
}

static InputData
gst_synap_infer_recognize_face (GstSynapInfer *infer, const InputData& input_image,
    const Rect& face_rect, const std::vector<Dim2d>& landmarks)
{
  if (input_image.empty()) {
    return {};
  }

  const Dimensions image_dim = input_image.dimensions();
  if (!image_dim.w || !image_dim.h ||
      face_rect.origin.x < 0 || face_rect.origin.y < 0 ||
      face_rect.size.x <= 0 || face_rect.size.y <= 0) {
    return {};
  }

  if (face_rect.origin.x + face_rect.size.x > image_dim.w ||
      face_rect.origin.y + face_rect.size.y > image_dim.h) {
    return {};
  }

  float eye_distance =
    hypot(landmarks[0].x - landmarks[1].x, landmarks[0].y - landmarks[1].y);
  float mouth_to_eye_distance =
    hypot(landmarks[4].x - landmarks[1].x, landmarks[4].y - landmarks[1].y);
  if ((eye_distance / mouth_to_eye_distance) <=
      GST_SYNAP_DEFAULT_SIDE_FACE_THRESHOLD_RECOGNITION ||
      (eye_distance / mouth_to_eye_distance) > 1) {
    return {};
  }

  if (!gst_synap_infer_head_pose(face_rect, landmarks)) {
    return {};
  }

  InputData aligned_face =
    gst_synap_infer_align_face(input_image, face_rect, landmarks);
  if (aligned_face.empty()) {
    return {};
  }

  return aligned_face;
}