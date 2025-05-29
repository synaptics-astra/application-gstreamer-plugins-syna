/* GStreamer Synap Image Processing
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
 * SECTION:element-gstsynapimageproc
 *
 * GStreamer Plugin to do AI processing with Synaptics Neural Network
 * Acceleration and Processing component (SyNAP) for Image Processing
 *
 * <refsect2>
 * <title>For Image Processing</title>
 * |[
 * gst-launch-1.0 v4l2src device=/dev/xxxx ! video/x-raw,<prop> ! videoconvert ! \
 *  synapimageproc model=<file> ! video/x-raw, width=<w>, height=<h> ! waylandsink
 * ]|
 * Gstreamer sample pipeline to do frame upscaling using synapimageproc
 *
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstsynapimageproc.h"

GST_DEBUG_CATEGORY_STATIC (gst_synap_image_proc_debug_category);
#define GST_CAT_DEFAULT gst_synap_image_proc_debug_category

/* prototypes */
static void gst_synap_image_proc_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_synap_image_proc_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_synap_image_proc_dispose (GObject * object);
static void gst_synap_image_proc_finalize (GObject * object);

static GstCaps *gst_synap_image_proc_transform_caps (GstBaseTransform *
    trans, GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static GstCaps *gst_synap_image_proc_fixate_caps (GstBaseTransform *
    trans, GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static gboolean gst_synap_image_proc_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean gst_synap_image_proc_transform_size (GstBaseTransform *
    trans, GstPadDirection direction, GstCaps * caps, gsize size,
    GstCaps * othercaps, gsize * othersize);
static gboolean gst_synap_image_proc_start (GstBaseTransform * trans);
static gboolean gst_synap_image_proc_stop (GstBaseTransform * trans);
static GstFlowReturn gst_synap_image_proc_transform (GstBaseTransform *
    trans, GstBuffer * inbuf, GstBuffer * outbuf);

enum
{
  PROP_0,
  PROP_MODEL,
};

/* pad templates */

static GstStaticPadTemplate gst_synap_image_proc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ NV12, I420 }"))
    );

static GstStaticPadTemplate gst_synap_image_proc_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ NV12, I420 }"))
    );

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstSynapImageProc, gst_synap_image_proc,
    GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_synap_image_proc_debug_category,
      "synapimageproc", 0, "debug category for synapimageproc element"));
GST_ELEMENT_REGISTER_DEFINE (synapimageproc, "synapimageproc", GST_RANK_NONE,
    GST_TYPE_SYNAP_IMAGE_PROC);

static void
gst_synap_image_proc_class_init (GstSynapImageProcClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
    GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_synap_image_proc_src_template);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_synap_image_proc_sink_template);
  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Plugin for Synaptics AI", "Generic/Video",
      "GStreamer layer for Synaptics Neural Network Acceleration and Processing component",
      "https://www.synaptics.com/");
  gobject_class->set_property = gst_synap_image_proc_set_property;
  gobject_class->get_property = gst_synap_image_proc_get_property;
  gobject_class->dispose = gst_synap_image_proc_dispose;
  gobject_class->finalize = gst_synap_image_proc_finalize;
  base_transform_class->set_caps = GST_DEBUG_FUNCPTR (gst_synap_image_proc_set_caps);
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_synap_image_proc_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_synap_image_proc_stop);
  base_transform_class->transform =
    GST_DEBUG_FUNCPTR (gst_synap_image_proc_transform);
  base_transform_class->transform_caps =
    GST_DEBUG_FUNCPTR (gst_synap_image_proc_transform_caps);
  base_transform_class->fixate_caps =
    GST_DEBUG_FUNCPTR (gst_synap_image_proc_fixate_caps);
  base_transform_class->transform_size =
    GST_DEBUG_FUNCPTR (gst_synap_image_proc_transform_size);

  /* Properties */
  g_object_class_install_property (gobject_class, PROP_MODEL,
      g_param_spec_string ("model", "Model filepath",
        "File path to the model file",
        NULL, (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_synap_image_proc_init (GstSynapImageProc * synapimageproc)
{
  /* Initialize parameters */
  synapimageproc->model = NULL;
}

void
gst_synap_image_proc_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (object);
  GST_DEBUG_OBJECT (synapimageproc, "set_property");
  g_return_if_fail (synapimageproc != NULL);
  g_return_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc));

  switch (property_id) {
    case PROP_MODEL:
      {
        if (g_value_get_string (value) != NULL) {
          synapimageproc->model = g_value_dup_string (value);
          GST_INFO ("Model: %s", synapimageproc->model);
        }
      }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_image_proc_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (object);
  GST_DEBUG_OBJECT (synapimageproc, "get_property");
  g_return_if_fail (synapimageproc != NULL);
  g_return_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc));

  switch (property_id) {
    case PROP_MODEL:
      {
        g_value_set_string (value, synapimageproc->model);
      }
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_image_proc_dispose (GObject * object)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (object);
  GST_DEBUG_OBJECT (synapimageproc, "dispose");

  G_OBJECT_CLASS (gst_synap_image_proc_parent_class)->dispose (object);
}

void
gst_synap_image_proc_finalize (GObject * object)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (object);
  GST_DEBUG_OBJECT (synapimageproc, "finalize");
  g_return_if_fail (synapimageproc != NULL);
  g_return_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc));

  /* clean up object here */
  g_free (synapimageproc->model);
  G_OBJECT_CLASS (gst_synap_image_proc_parent_class)->finalize (object);
}

/* states */
static gboolean
gst_synap_image_proc_start (GstBaseTransform * trans)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (trans);
  GST_DEBUG_OBJECT (synapimageproc, "start");
  g_return_val_if_fail (synapimageproc != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc), FALSE);

  /* Validate parameters */
  if (synapimageproc->model == NULL) {
    GST_ERROR ("Invalid Model Name");
    return FALSE;
  }

  /* Create SyNAP objects */
  synapimageproc->network = new Network();
  g_return_val_if_fail (synapimageproc->network != NULL, FALSE);

  /* Load Model */
  if (!synapimageproc->network->load_model (synapimageproc->model, "")) {
    GST_ERROR ("Model cannot be loaded");
    return FALSE;
  }

  /* Fetching model's dimensions */
  auto index = synapimageproc->network->outputs[0].size()
    > synapimageproc->network->outputs[1].size() ? 0 : 1;
  Dimensions dim;
  dim = synapimageproc->network->outputs[index].dimensions();
  synapimageproc->model_out_width = dim.w;
  synapimageproc->model_out_height = dim.h;

  /* Allocator */
  synapimageproc->allocator = std_allocator();
  if (!synapimageproc->allocator) {
    GST_ERROR ("Selected memory allocation method not available");
    return FALSE;
  }

  /* Set input tensors allocator */
  for (Tensor& tensor : synapimageproc->network->inputs) {
    if (!tensor.buffer()->set_allocator(synapimageproc->allocator))
    {
      GST_ERROR ("Can't change allocator, input buffer not empty");
      return FALSE;
    }
  }

  /* Set output tensors allocator */
  for (Tensor& tensor : synapimageproc->network->outputs) {
    if (!tensor.buffer()->set_allocator(synapimageproc->allocator))
    {
      GST_ERROR ("Can't change allocator, output buffer not empty");
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
gst_synap_image_proc_stop (GstBaseTransform * trans)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (trans);
  GST_DEBUG_OBJECT (synapimageproc, "stop");
  g_return_val_if_fail (synapimageproc != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc), FALSE);

  if (synapimageproc->network != NULL)
    delete synapimageproc->network;
  return TRUE;
}

static GstCaps *
gst_synap_image_proc_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (trans);
  GST_DEBUG_OBJECT (synapimageproc, "transform_caps");
  g_return_val_if_fail (synapimageproc != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc), FALSE);

  GstCaps *res_caps = gst_caps_new_empty ();

  guint i;
  for (i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *structure = gst_caps_get_structure (caps, i);
    GstCapsFeatures *features = gst_caps_get_features (caps, i);
    GstStructure *new_structure = gst_structure_copy (structure);

    if (direction == GST_PAD_SRC || direction == GST_PAD_SINK) {
      gst_structure_remove_field (new_structure, "format");
    }
    gst_structure_set (new_structure, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        NULL);
    gst_structure_set (new_structure, "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        NULL);
    GST_DEBUG_OBJECT (synapimageproc,
        "Set width and height to ranges for flexibility");
    gst_caps_append_structure_full (res_caps, new_structure,
        gst_caps_features_copy (features));
  }

  if (filter) {
    GstCaps *tmp_caps = res_caps;
    res_caps =
      gst_caps_intersect_full (filter, tmp_caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp_caps);
  }
  return res_caps;
}

static GstCaps *
gst_synap_image_proc_fixate_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (trans);
  GST_DEBUG_OBJECT (synapimageproc, "fixate_caps");
  g_return_val_if_fail (synapimageproc != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc), FALSE);

  GstStructure *ins, *outs;
  gint ins_w, ins_h;
  gint outs_w = 0, outs_h = 0;
  const gchar *in_format;
  const GstVideoFormatInfo *in_info;

  GST_DEBUG_OBJECT (synapimageproc, "Direction: %d", direction);
  GST_DEBUG_OBJECT (synapimageproc, "Caps: %" GST_PTR_FORMAT, caps);
  GST_DEBUG_OBJECT (synapimageproc, "Other Caps: %" GST_PTR_FORMAT,
      othercaps);

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);
  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  gst_structure_get_int (ins, "width", &ins_w);
  gst_structure_get_int (ins, "height", &ins_h);

  gst_structure_get_int (outs, "width", &outs_w);
  gst_structure_get_int (outs, "height", &outs_h);

  if (outs_w && outs_h) {
    GST_DEBUG_OBJECT (synapimageproc,
        "Already fixated - Nothing to be done\n");
  } else {
    gst_structure_set (outs, "width", G_TYPE_INT, synapimageproc->model_out_width, NULL);
    gst_structure_set (outs, "height", G_TYPE_INT, synapimageproc->model_out_height, NULL);
  }

  in_format = gst_structure_get_string (ins, "format");
  if (!in_format)
    return NULL;
  in_info =
    gst_video_format_get_info (gst_video_format_from_string (in_format));
  if (!in_info)
    return NULL;

  guint capslen, i;
  capslen = gst_caps_get_size (othercaps);
  GST_DEBUG_OBJECT (synapimageproc, "iterate %d structures", capslen);
  for (i = 0; i < capslen; i++) {
    GstStructure *str;
    const GValue *format;
    str = gst_caps_get_structure (othercaps, i);
    format = gst_structure_get_value (str, "format");
    if (format == NULL)
      continue;
    if (GST_VALUE_HOLDS_LIST (format)) {
      GST_DEBUG_OBJECT (synapimageproc, "Format list is present");
      gst_structure_set (outs, "format", G_TYPE_STRING,
          GST_VIDEO_FORMAT_INFO_NAME (in_info), NULL);
    }
  }

  GST_DEBUG_OBJECT (synapimageproc, "Other Caps: %" GST_PTR_FORMAT,
      othercaps);
  return othercaps;
}

static gboolean
gst_synap_image_proc_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (trans);
  GST_DEBUG_OBJECT (synapimageproc, "set_caps");
  g_return_val_if_fail (synapimageproc != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc), FALSE);

  if (!gst_video_info_from_caps (&synapimageproc->in_videoinfo, incaps))
    return FALSE;

  if (!gst_video_info_from_caps (&synapimageproc->out_videoinfo, outcaps))
    return FALSE;

  GstVideoInfo *info = &synapimageproc->in_videoinfo;
  synapimageproc->in_width = GST_VIDEO_INFO_WIDTH (info);
  synapimageproc->in_height = GST_VIDEO_INFO_HEIGHT (info);

  info = &synapimageproc->out_videoinfo;
  synapimageproc->out_width = GST_VIDEO_INFO_WIDTH (info);
  synapimageproc->out_height = GST_VIDEO_INFO_HEIGHT (info);

  synapimageproc->in_format = GST_VIDEO_INFO_FORMAT (&synapimageproc->in_videoinfo);
  if (synapimageproc->in_format == GST_VIDEO_FORMAT_NV12) {
    // NV12
  } else if (synapimageproc->in_format == GST_VIDEO_FORMAT_I420) {
    // I420
  } else {
    g_warning ("Unsupported input format for synapimageproc plugin: %d",
        synapimageproc->in_format);
    return FALSE;
  }

  synapimageproc->out_format = GST_VIDEO_INFO_FORMAT (&synapimageproc->out_videoinfo);
  if (synapimageproc->out_format == GST_VIDEO_FORMAT_NV12) {
    // NV12
  } else if (synapimageproc->out_format == GST_VIDEO_FORMAT_I420) {
    // I420
  } else {
    g_warning ("Unsupported output format for synapimageproc plugin: %d",
        synapimageproc->out_format);
    return FALSE;
  }

  return TRUE;
}

/* Transform size */
static gboolean
gst_synap_image_proc_transform_size (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, gsize size, GstCaps * othercaps,
    gsize * othersize)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (trans);
  GST_DEBUG_OBJECT (synapimageproc, "transform_size");
  g_return_val_if_fail (synapimageproc != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc), FALSE);

  GstVideoInfo info;
  gboolean ret = FALSE;

  ret = gst_video_info_from_caps (&info, othercaps);
  if (ret) {
    *othersize = info.size;
  }
  return ret;
}

static GstFlowReturn
gst_synap_image_proc_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstSynapImageProc *synapimageproc = GST_SYNAP_IMAGE_PROC (trans);
  GST_DEBUG_OBJECT (synapimageproc, "transform");
  g_return_val_if_fail (synapimageproc != NULL, GST_FLOW_ERROR);
  g_return_val_if_fail (GST_IS_SYNAP_IMAGE_PROC (synapimageproc), GST_FLOW_ERROR);
  g_return_val_if_fail (inbuf != NULL, GST_FLOW_ERROR);
  g_return_val_if_fail (outbuf != NULL, GST_FLOW_ERROR);

  if (synapimageproc->out_width != synapimageproc->model_out_width ||
      synapimageproc->out_height != synapimageproc->model_out_height) {
    GST_ERROR("Given model supports output dimensions %dx%d, but received %dx%d.",
        synapimageproc->model_out_width, synapimageproc->model_out_height,
        synapimageproc->out_width, synapimageproc->out_height);
    return GST_FLOW_ERROR;
  }

  /* Map input buffer */
  GstMapInfo in_map_info, out_map_info;
  gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ);
  gst_buffer_map(outbuf, &out_map_info, GST_MAP_WRITE);

  guint8 *in_data = (guint8 *)in_map_info.data;
  guint8 *out_data = (guint8 *)out_map_info.data;

  /* Process with SyNAP */
  Tensor* y{};
  Tensor* uv{};

  if (synapimageproc->network->inputs[0].format() == "y8"
      && synapimageproc->network->inputs[1].format() == "uv8") {
    y = &synapimageproc->network->inputs[0];
    uv = &synapimageproc->network->inputs[1];
  } else if (synapimageproc->network->inputs[1].format() == "y8"
      && synapimageproc->network->inputs[0].format() == "uv8") {
    y = &synapimageproc->network->inputs[1];
    uv = &synapimageproc->network->inputs[0];
  } else {
    gst_buffer_unmap (inbuf, &in_map_info);
    gst_buffer_unmap (outbuf, &out_map_info);
    GST_ERROR ("Upscaling failed");
    return GST_FLOW_ERROR;
  }

  uint32_t size_in_y = synapimageproc->in_height * synapimageproc->in_width;
  if (!y->assign(in_data, size_in_y)) {
    GST_ERROR ("Failed to assign data to input tensors");
    return GST_FLOW_ERROR;
  }

  if (synapimageproc->in_format == GST_VIDEO_FORMAT_NV12) {
    if (!uv->assign(in_data + size_in_y, size_in_y / 2)) {
      GST_ERROR ("Failed to assign data to input tensors");
      return GST_FLOW_ERROR;
    }
  } else if (synapimageproc->in_format == GST_VIDEO_FORMAT_I420) {
    uint8_t* in_u = in_data + size_in_y;
    uint8_t* in_v = in_u + size_in_y / 4;

    uint8_t* in_uv = new uint8_t[size_in_y/2];
    for (int i = 0; i < size_in_y / 4; i++) {
      in_uv[i * 2] = in_u[i];
      in_uv[i * 2 + 1] = in_v[i];
    }
    if (!uv->assign(in_uv, size_in_y / 2)) {
      GST_ERROR ("Failed to assign data to input tensors");
      delete in_uv;
      return GST_FLOW_ERROR;
    }
    delete in_uv;
  }

  /* Execute inference */
  if (!synapimageproc->network->predict ()) {
    gst_buffer_unmap (inbuf, &in_map_info);
    gst_buffer_unmap (outbuf, &out_map_info);
    GST_ERROR ("Upscaling failed");
    return GST_FLOW_ERROR;
  }

  /* Post Process output */
  if (synapimageproc->network->outputs[0].format() == "y8"
      && synapimageproc->network->outputs[1].format() == "uv8") {
    y = &synapimageproc->network->outputs[0];
    uv = &synapimageproc->network->outputs[1];
  } else if (synapimageproc->network->outputs[1].format() == "y8"
      && synapimageproc->network->outputs[0].format() == "uv8") {
    y = &synapimageproc->network->outputs[1];
    uv = &synapimageproc->network->outputs[0];
  } else {
    gst_buffer_unmap (inbuf, &in_map_info);
    gst_buffer_unmap (outbuf, &out_map_info);
    GST_ERROR ("Upscaling failed");
    return GST_FLOW_ERROR;
  }

  const uint8_t* out_y = (uint8_t*)y->data();
  const uint8_t* out_uv = (uint8_t*)uv->data();

  if (out_y != nullptr && out_uv != nullptr) {
    uint32_t size_out_y = synapimageproc->out_height * synapimageproc->out_width;
    memcpy(out_data, out_y, size_out_y);
    if (synapimageproc->out_format == GST_VIDEO_FORMAT_NV12) {
      memcpy(out_data + size_out_y, out_uv, size_out_y / 2);
    } else if (synapimageproc->out_format == GST_VIDEO_FORMAT_I420) {
      uint8_t* out_u = out_data + size_out_y;
      uint8_t* out_v = out_u + size_out_y / 4;
      for (int i = 0; i < size_out_y / 4; i++) {
        out_u[i] = out_uv[i * 2];
        out_v[i] = out_uv[i * 2 + 1];
      }
    }
  }

  gst_buffer_unmap(inbuf, &in_map_info);
  gst_buffer_unmap(outbuf, &out_map_info);

  return GST_FLOW_OK;
}
