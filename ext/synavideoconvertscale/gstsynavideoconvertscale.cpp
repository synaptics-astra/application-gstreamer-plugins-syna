/* GStreamer SynaVideoConvertScale
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
 * SECTION:element-gstsynavideoconvertscale
 *
 * The synavideoconvertscale element does OpenCV based color conversion between specific formats and downscaling.
 *
 * <refsect2>
 * <title>Example launch line for synavideoconvertscale </title>
 * |[
 * gst-launch-1.0 -v videotestsrc ! video/x-raw,width=640,height=480,format=NV12 ! synavideoconvertscale ! video/x-raw,width=640,height=480,format=BGRA ! autovideosink
 * ]|
 * Sample pipeline to do NV12 to BGRA conversion
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include "gstsynavideoconvertscale.h"

#include <opencv2/core.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include "opencv2/core/ocl.hpp"

GST_DEBUG_CATEGORY_STATIC (gst_syna_video_convert_scale_debug_category);
#define GST_CAT_DEFAULT gst_syna_video_convert_scale_debug_category

/* prototypes */


static void gst_syna_video_convert_scale_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_syna_video_convert_scale_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_syna_video_convert_scale_dispose (GObject * object);
static void gst_syna_video_convert_scale_finalize (GObject * object);

static GstCaps *gst_syna_video_convert_scale_transform_caps (GstBaseTransform *
    trans, GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static GstCaps *gst_syna_video_convert_scale_fixate_caps (GstBaseTransform *
    trans, GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static gboolean gst_syna_video_convert_scale_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean gst_syna_video_convert_scale_transform_size (GstBaseTransform *
    trans, GstPadDirection direction, GstCaps * caps, gsize size,
    GstCaps * othercaps, gsize * othersize);
static gboolean gst_syna_video_convert_scale_start (GstBaseTransform * trans);
static gboolean gst_syna_video_convert_scale_stop (GstBaseTransform * trans);
static gboolean gst_syna_video_convert_scale_sink_event (GstBaseTransform *
    trans, GstEvent * event);
static gboolean gst_syna_video_convert_scale_src_event (GstBaseTransform *
    trans, GstEvent * event);
static GstFlowReturn gst_syna_video_convert_scale_transform (GstBaseTransform *
    trans, GstBuffer * inbuf, GstBuffer * outbuf);

static gboolean gst_syna_video_convert_scale_cvmat_params_from_video_info
    (GstVideoInfo * info, gint * width, gint * height, int *cv_type,
    GError ** err);
static gboolean gst_syna_video_convert_scale_cvimage_type_from_video_format
    (GstVideoFormat format, int *cv_type, GError ** err);
static void gst_syna_video_convert_scale_YV12ToNV12 (const cv::UMat & input,
    cv::UMat & output);
static void gst_syna_video_convert_scale_YV444TOYUY2(const cv::UMat &input,
                                                     cv::UMat &output);

enum
{
  PROP_0
};

/* pad templates */

static GstStaticPadTemplate gst_syna_video_convert_scale_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ RGB, BGRA, NV12, I420, YUY2}"))
    );

static GstStaticPadTemplate gst_syna_video_convert_scale_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ RGB, BGRA, NV12, I420, YUY2}"))
    );


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstSynaVideoConvertScale, gst_syna_video_convert_scale,
    GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_syna_video_convert_scale_debug_category,
        "synavideoconvertscale", 0,
        "debug category for synavideoconvertscale element"));

static void
gst_syna_video_convert_scale_class_init (GstSynaVideoConvertScaleClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_syna_video_convert_scale_src_template);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_syna_video_convert_scale_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Plugin for Synaptics Color Conversion and Downscaling", "Generic/Video",
      "GStreamer Plugin for OpenCV based color conversion and downscaling",
      "https://www.synaptics.com/");

  gobject_class->set_property = gst_syna_video_convert_scale_set_property;
  gobject_class->get_property = gst_syna_video_convert_scale_get_property;
  gobject_class->dispose = gst_syna_video_convert_scale_dispose;
  gobject_class->finalize = gst_syna_video_convert_scale_finalize;
  base_transform_class->start =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_start);
  base_transform_class->stop =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_stop);
  base_transform_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_sink_event);
  base_transform_class->src_event =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_src_event);

  base_transform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_transform_caps);
  base_transform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_fixate_caps);
  base_transform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_set_caps);
  base_transform_class->transform_size =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_transform_size);
  base_transform_class->transform =
      GST_DEBUG_FUNCPTR (gst_syna_video_convert_scale_transform);

}

static void
gst_syna_video_convert_scale_init (GstSynaVideoConvertScale *
    synavideoconvertscale)
{
  cv::ocl::setUseOpenCL(true);
}

void
gst_syna_video_convert_scale_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (object);

  GST_DEBUG_OBJECT (synavideoconvertscale, "set_property");

  switch (property_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_syna_video_convert_scale_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (object);

  GST_DEBUG_OBJECT (synavideoconvertscale, "get_property");

  switch (property_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_syna_video_convert_scale_dispose (GObject * object)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (object);

  GST_DEBUG_OBJECT (synavideoconvertscale, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_syna_video_convert_scale_parent_class)->dispose (object);
}

void
gst_syna_video_convert_scale_finalize (GObject * object)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (object);

  GST_DEBUG_OBJECT (synavideoconvertscale, "finalize");

  /* clean up object here */

  G_OBJECT_CLASS (gst_syna_video_convert_scale_parent_class)->finalize (object);
}

static GstCaps *
gst_syna_video_convert_scale_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);
  GstCaps *res_caps, *tmp_caps;
  guint i;

  GST_DEBUG_OBJECT (synavideoconvertscale, "transform_caps");

  res_caps = gst_caps_new_empty ();

  //iterating through each struture in the input caps
  for (i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *structure = gst_caps_get_structure (caps, i);
    GstCapsFeatures *features = gst_caps_get_features (caps, i);
    GstStructure *new_structure = gst_structure_copy (structure);

    if (direction == GST_PAD_SRC || direction == GST_PAD_SINK) {
      gst_structure_remove_field (new_structure, "format");
    }
    // Set width and height to ranges to handle various resolutions
    gst_structure_set (new_structure, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        NULL);
    gst_structure_set (new_structure, "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
        NULL);
    GST_DEBUG_OBJECT (synavideoconvertscale,
        "Set width and height to ranges for flexibility");
    gst_caps_append_structure_full (res_caps, new_structure,
        gst_caps_features_copy (features));
  }

  if (filter) {
    tmp_caps = res_caps;
    res_caps =
        gst_caps_intersect_full (filter, tmp_caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp_caps);
  }
  return res_caps;
}

static GstCaps *
gst_syna_video_convert_scale_fixate_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);

  GST_DEBUG_OBJECT (synavideoconvertscale, "fixate_caps");
  GstStructure *ins, *outs;
  gint from_w, from_h;
  gint w = 0, h = 0;
  const gchar *in_format;
  const GstVideoFormatInfo *in_info;

  GST_DEBUG_OBJECT (synavideoconvertscale, "Direction: %d", direction);
  GST_DEBUG_OBJECT (synavideoconvertscale, "Caps: %" GST_PTR_FORMAT, caps);
  GST_DEBUG_OBJECT (synavideoconvertscale, "Other Caps: %" GST_PTR_FORMAT,
      othercaps);

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);
  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  gst_structure_get_int (ins, "width", &from_w);
  gst_structure_get_int (ins, "height", &from_h);

  gst_structure_get_int (outs, "width", &w);
  gst_structure_get_int (outs, "height", &h);

  if (w && h) {
    GST_DEBUG_OBJECT (synavideoconvertscale,
        "Already fixated - Nothing to be done\n");
  } else {
    gst_structure_set (outs, "width", G_TYPE_INT, from_w, NULL);
    gst_structure_set (outs, "height", G_TYPE_INT, from_h, NULL);
  }

  // Fixate format
  in_format = gst_structure_get_string (ins, "format");
  if (!in_format)
    return NULL;
  in_info =
      gst_video_format_get_info (gst_video_format_from_string (in_format));
  if (!in_info)
    return NULL;

  guint capslen, i;
  capslen = gst_caps_get_size (othercaps);
  GST_DEBUG_OBJECT (synavideoconvertscale, "iterate %d structures", capslen);
  for (i = 0; i < capslen; i++) {
    GstStructure *tests;
    const GValue *format;

    tests = gst_caps_get_structure (othercaps, i);
    format = gst_structure_get_value (tests, "format");
    if (format == NULL)
      continue;
    if (GST_VALUE_HOLDS_LIST (format)) {
      GST_DEBUG_OBJECT (synavideoconvertscale, "Format list is present");
      gst_structure_set (outs, "format", G_TYPE_STRING,
          GST_VIDEO_FORMAT_INFO_NAME (in_info), NULL);
    }
  }

  GST_DEBUG_OBJECT (synavideoconvertscale, "Other Caps: %" GST_PTR_FORMAT,
      othercaps);
  return othercaps;
}

static gboolean
gst_syna_video_convert_scale_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);
  GError *err = NULL;
  GST_DEBUG_OBJECT (synavideoconvertscale, "set_caps");

  if (!gst_video_info_from_caps (&synavideoconvertscale->videoinfo, incaps))
    return FALSE;

  if (!gst_video_info_from_caps (&synavideoconvertscale->videooutinfo, outcaps))
    return FALSE;

  if (!gst_syna_video_convert_scale_cvmat_params_from_video_info
      (&synavideoconvertscale->videoinfo, &synavideoconvertscale->in_width,
          &synavideoconvertscale->in_height, &synavideoconvertscale->in_cv_type,
          &err)) {
    GST_WARNING_OBJECT (trans, "Failed to parse input caps: %s", err->message);
    g_error_free (err);
    return FALSE;
  }

  if (!gst_syna_video_convert_scale_cvmat_params_from_video_info
      (&synavideoconvertscale->videooutinfo, &synavideoconvertscale->out_width,
          &synavideoconvertscale->out_height,
          &synavideoconvertscale->out_cv_type, &err)) {
    GST_WARNING_OBJECT (trans, "Failed to parse input caps: %s", err->message);
    g_error_free (err);
    return FALSE;
  }

  return TRUE;
}

/* transform size */
static gboolean
gst_syna_video_convert_scale_transform_size (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, gsize size, GstCaps * othercaps,
    gsize * othersize)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);
  GstVideoInfo info;
  gboolean ret = TRUE;

  GST_DEBUG_OBJECT (synavideoconvertscale, "transform_size");
  ret = gst_video_info_from_caps (&info, othercaps);
  if (ret) {
    *othersize = info.size;
  }
  return TRUE;
}

/* states */
static gboolean
gst_syna_video_convert_scale_start (GstBaseTransform * trans)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);

  GST_DEBUG_OBJECT (synavideoconvertscale, "start");

  return TRUE;
}

static gboolean
gst_syna_video_convert_scale_stop (GstBaseTransform * trans)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);

  GST_DEBUG_OBJECT (synavideoconvertscale, "stop");

  return TRUE;
}

/* sink and src pad event handlers */
static gboolean
gst_syna_video_convert_scale_sink_event (GstBaseTransform * trans,
    GstEvent * event)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);

  GST_DEBUG_OBJECT (synavideoconvertscale, "sink_event");

  return GST_BASE_TRANSFORM_CLASS (gst_syna_video_convert_scale_parent_class)->
      sink_event (trans, event);
}

static gboolean
gst_syna_video_convert_scale_src_event (GstBaseTransform * trans,
    GstEvent * event)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);

  GST_DEBUG_OBJECT (synavideoconvertscale, "src_event");

  return GST_BASE_TRANSFORM_CLASS (gst_syna_video_convert_scale_parent_class)->
      src_event (trans, event);
}

/* Transform */
static GstFlowReturn
gst_syna_video_convert_scale_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstSynaVideoConvertScale *synavideoconvertscale =
      GST_SYNA_VIDEO_CONVERT_SCALE (trans);

  GST_DEBUG_OBJECT (synavideoconvertscale, "transform");
  GstVideoFrame inframe;
  GstVideoFrame outframe;

  // Map input frame for reading
  if (!gst_video_frame_map (&inframe, &synavideoconvertscale->videoinfo, inbuf,
          GST_MAP_READ)) {
    GST_ERROR ("Frame map error for input frame");
    return GST_FLOW_ERROR;
  }
  // Map output frame for writing
  if (!gst_video_frame_map (&outframe, &synavideoconvertscale->videooutinfo,
          outbuf, GST_MAP_READWRITE)) {
    GST_ERROR ("Frame map error for output frame");
    return GST_FLOW_ERROR;
  }

  // Get the output format negotiated
  GstVideoFormat output_format =
      GST_VIDEO_INFO_FORMAT (&synavideoconvertscale->videooutinfo);
  GstVideoFormat input_format =
      GST_VIDEO_INFO_FORMAT (&synavideoconvertscale->videoinfo);

  // Input format handling
  if (input_format == GST_VIDEO_FORMAT_RGB) {
    // RGB: Width x Height with 3 channels
    synavideoconvertscale->cvImage = cv::Mat (cv::Size (synavideoconvertscale->
            in_width, synavideoconvertscale->in_height), CV_8UC3, (unsigned char *) inframe.data[0]);
  } else if (input_format == GST_VIDEO_FORMAT_BGRA) {
    // BGRA: Width x Height with 4 channels
    synavideoconvertscale->cvImage = cv::Mat (cv::Size (synavideoconvertscale->
            in_width, synavideoconvertscale->in_height), CV_8UC4, inframe.data[0]);
  } else if (input_format == GST_VIDEO_FORMAT_NV12 || input_format == GST_VIDEO_FORMAT_I420) {
    // NV12 and I420: Single channel with height * 3/2
    synavideoconvertscale->cvImage = cv::Mat (cv::Size (synavideoconvertscale->
            in_width, synavideoconvertscale->in_height * 3 / 2), CV_8UC1, inframe.data[0]);
  } else if (input_format == GST_VIDEO_FORMAT_YUY2) {
    // YUY2 is interleaved, width x height with 2 channels
    synavideoconvertscale->cvImage = cv::Mat (cv::Size(synavideoconvertscale->in_width,
        synavideoconvertscale->in_height), CV_8UC2, inframe.data[0]);
  } else {
    // Unsupported format
    GST_ERROR ("Unsupported input format for synavideoconvertscale plugin: %d",
        input_format);
    return GST_FLOW_ERROR;
  }

  // Output format handling
  if (output_format == GST_VIDEO_FORMAT_RGB) {
    // RGB: Width x Height with 3 channels
    synavideoconvertscale->cvOutImage = cv::Mat (cv::Size (synavideoconvertscale->
            out_width, synavideoconvertscale->out_height), CV_8UC3, (unsigned char *) outframe.data[0]);
  } else if (output_format == GST_VIDEO_FORMAT_BGRA) {
    // BGRA: Width x Height with 4 channels
    synavideoconvertscale->cvOutImage = cv::Mat (cv::Size (synavideoconvertscale->
            out_width, synavideoconvertscale->out_height), CV_8UC4, (unsigned char *) outframe.data[0]);
  } else if (output_format == GST_VIDEO_FORMAT_NV12 || output_format == GST_VIDEO_FORMAT_I420) {
    // NV12: Similar to I420, with width x height * 3/2
    synavideoconvertscale->cvOutImage = cv::Mat (cv::Size (synavideoconvertscale->
            out_width, synavideoconvertscale->out_height * 3 / 2), CV_8UC1, (unsigned char *) outframe.data[0]);
  } else if (output_format == GST_VIDEO_FORMAT_YUY2) {
    // YUY2 is interleaved, width x height with 2 channels
    synavideoconvertscale->cvOutImage = cv::Mat(cv::Size(synavideoconvertscale->in_width,
            synavideoconvertscale->in_height), CV_8UC2, (unsigned char *) outframe.data[0]);
  } else {
    // Unsupported output format
    GST_ERROR ("Unsupported output format for synavideoconvertscale plugin: %d",
        output_format);
    return GST_FLOW_ERROR;
  }

  gboolean resize_reqd =
      (synavideoconvertscale->in_width != synavideoconvertscale->out_width)
      || (synavideoconvertscale->in_height !=
      synavideoconvertscale->out_height);

  synavideoconvertscale->cvImage.copyTo(synavideoconvertscale->cvUInFrame);

  synavideoconvertscale->tmp1 =
      cv::UMat(cv::Size(synavideoconvertscale->in_width, synavideoconvertscale->in_height), output_format, cv::USAGE_ALLOCATE_DEVICE_MEMORY);
  synavideoconvertscale->cvUOutFrame =
      cv::UMat(cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), output_format, cv::USAGE_ALLOCATE_DEVICE_MEMORY);

  if (input_format == output_format) {
    if (input_format == GST_VIDEO_FORMAT_NV12) {
      // Check for resize
      if (resize_reqd)
      {
        cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2BGRA_NV12);
        cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                   cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
        cv::UMat yv12_image;
        cv::cvtColor(synavideoconvertscale->tmp1, yv12_image, cv::COLOR_BGRA2YUV_YV12);
        gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
        synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
      }
      else
      {
        synavideoconvertscale->cvUInFrame.copyTo(synavideoconvertscale->cvOutImage);
      }
    }

    else if (input_format == GST_VIDEO_FORMAT_YUY2)
    {
      if (resize_reqd)
      {
        cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_YUY2);
        cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                   cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
        cv::UMat yuv_image;
        cv::cvtColor(synavideoconvertscale->tmp1, yuv_image, cv::COLOR_RGB2YUV);
        gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
        synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
      }
      else
      {
        synavideoconvertscale->cvUInFrame.copyTo(synavideoconvertscale->cvOutImage);
      }
    }
    else if (input_format == GST_VIDEO_FORMAT_I420)
    {
      if (resize_reqd)
      {
        cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_I420);
        cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                   cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
        cv::cvtColor(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame, cv::COLOR_RGB2YUV_I420);
        synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
      }
      else
      {
        synavideoconvertscale->cvUInFrame.copyTo(synavideoconvertscale->cvOutImage);
      }
    }

    else
    {
      if (resize_reqd)
      {
        cv::resize(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame,
                   cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
        synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
      }
      else
      {
        synavideoconvertscale->cvUInFrame.copyTo(synavideoconvertscale->cvOutImage);
      }
    }
  }

  if (input_format == GST_VIDEO_FORMAT_NV12 && output_format == GST_VIDEO_FORMAT_BGRA)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale, "NV12 - BGRA\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2BGRA_NV12);
      cv::resize(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2BGRA_NV12);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }
  else if (input_format == GST_VIDEO_FORMAT_NV12 && output_format == GST_VIDEO_FORMAT_RGB)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"NV12 - RGB\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_NV12);
      cv::resize(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_NV12);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_BGRA && output_format == GST_VIDEO_FORMAT_RGB)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"BGRA - RGB\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_BGRA2RGB);
      cv::resize(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_BGRA2RGB);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }
  else if (input_format == GST_VIDEO_FORMAT_RGB && output_format == GST_VIDEO_FORMAT_BGRA)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"RGB - BGRA\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_RGB2BGRA);
      cv::resize(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_RGB2BGRA);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
     GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_RGB && output_format == GST_VIDEO_FORMAT_NV12)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"RGB - NV12\n");
    if (resize_reqd)
    {

      cv::resize(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::UMat yv12_image;
      cv::cvtColor(synavideoconvertscale->tmp1, yv12_image, cv::COLOR_RGB2YUV_YV12);
      gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
    }

    else
    {
      cv::UMat yv12_image;
      cv::cvtColor(synavideoconvertscale->cvUInFrame, yv12_image, cv::COLOR_RGB2YUV_YV12);
      gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_BGRA && output_format == GST_VIDEO_FORMAT_NV12)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"BGRA - NV12\n");
    if (resize_reqd)
    {
      cv::resize(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::UMat yv12_image;
      cv::cvtColor(synavideoconvertscale->tmp1, yv12_image, cv::COLOR_BGRA2YUV_YV12);
      gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
    }

    else
    {
      cv::UMat yv12_image;
      cv::cvtColor(synavideoconvertscale->cvUInFrame, yv12_image, cv::COLOR_BGRA2YUV_YV12);
      gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_I420 && output_format == GST_VIDEO_FORMAT_RGB)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"I420 - RGB\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_I420);
      cv::resize(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_I420);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_I420 && output_format == GST_VIDEO_FORMAT_BGRA)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"I420 - BGRA\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2BGRA_I420);
      cv::resize(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2BGRA_I420);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_RGB && output_format == GST_VIDEO_FORMAT_I420)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"RGB - I420\n");
    if (resize_reqd)
    {

      cv::resize(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::cvtColor(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame, cv::COLOR_RGB2YUV_I420);

      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_RGB2YUV_I420);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_BGRA && output_format == GST_VIDEO_FORMAT_I420)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"BGRA - I420\n");
    if (resize_reqd)
    {

      cv::resize(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::cvtColor(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame, cv::COLOR_BGRA2YUV_I420);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_BGRA2YUV_I420);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_NV12 && output_format == GST_VIDEO_FORMAT_I420)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"NV12 -RGB- I420\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_NV12);
      cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::cvtColor(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame, cv::COLOR_RGB2YUV_I420);
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_NV12);
      cv::cvtColor(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame, cv::COLOR_RGB2YUV_I420);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_I420 && output_format == GST_VIDEO_FORMAT_NV12)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"I420 -RGB- NV12\n");
    cv::UMat yv12_image;

    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_I420);
      cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::cvtColor(synavideoconvertscale->tmp1, yv12_image, cv::COLOR_RGB2YUV_YV12);
      gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_I420);
      cv::cvtColor(synavideoconvertscale->tmp1, yv12_image, cv::COLOR_RGB2YUV_YV12);
      gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }
  else if (input_format == GST_VIDEO_FORMAT_YUY2 && output_format == GST_VIDEO_FORMAT_BGRA)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"YUY2 - BGRA\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2BGRA_YUY2);
      cv::resize(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2BGRA_YUY2);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }
  else if (input_format == GST_VIDEO_FORMAT_YUY2 && output_format == GST_VIDEO_FORMAT_RGB)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"YUY2 - RGB\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_YUY2);
      cv::resize(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_YUY2);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }
  else if (input_format == GST_VIDEO_FORMAT_RGB && output_format == GST_VIDEO_FORMAT_YUY2)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"RGB - YUY2\n");
    if (resize_reqd)
    {
      cv::resize(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::UMat yuv_image;
      cv::cvtColor(synavideoconvertscale->tmp1, yuv_image, cv::COLOR_RGB2YUV);
      gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
    }
    else
    {
      cv::UMat yuv_image;
      cv::cvtColor(synavideoconvertscale->cvUInFrame, yuv_image, cv::COLOR_RGB2YUV);
      gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
    }

    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_BGRA && output_format == GST_VIDEO_FORMAT_YUY2)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"BGRA - YUY2\n");
    if (resize_reqd)
    {
      cv::resize(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::UMat yuv_image;
      cv::UMat bgr_image;
      cv::cvtColor(synavideoconvertscale->tmp1, bgr_image, cv::COLOR_BGRA2BGR);
      cv::cvtColor(bgr_image, yuv_image, cv::COLOR_BGR2YUV);
      gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
    }
    else
    {
      cv::UMat yuv_image;
      cv::UMat bgr_image;
      cv::cvtColor(synavideoconvertscale->cvUInFrame, bgr_image, cv::COLOR_BGRA2BGR);
      cv::cvtColor(bgr_image, yuv_image, cv::COLOR_BGR2YUV);
      gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
    }

    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_NV12 && output_format == GST_VIDEO_FORMAT_YUY2)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"NV12 - YUY2\n");
    cv::UMat yuv_image;
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_NV12);
      cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::cvtColor(synavideoconvertscale->tmp1, yuv_image, cv::COLOR_RGB2YUV);
      gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_NV12);
      cv::cvtColor(synavideoconvertscale->tmp1, yuv_image, cv::COLOR_RGB2YUV);
      gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }
  else if (input_format == GST_VIDEO_FORMAT_I420 && output_format == GST_VIDEO_FORMAT_YUY2)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"I420 - YUY2\n");
    cv::UMat yuv_image;
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_I420);
      cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::cvtColor(synavideoconvertscale->tmp1, yuv_image, cv::COLOR_RGB2YUV);
      gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_I420);
      cv::cvtColor(synavideoconvertscale->tmp1, yuv_image, cv::COLOR_RGB2YUV);
      gst_syna_video_convert_scale_YV444TOYUY2(yuv_image, synavideoconvertscale->cvUOutFrame);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }
  else if (input_format == GST_VIDEO_FORMAT_YUY2 && output_format == GST_VIDEO_FORMAT_I420)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"YUY2 - I420\n");
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_YUY2);
      cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::cvtColor(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame, cv::COLOR_RGB2YUV_I420);
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_YUY2);
      cv::cvtColor(synavideoconvertscale->tmp1, synavideoconvertscale->cvUOutFrame, cv::COLOR_RGB2YUV_I420);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  else if (input_format == GST_VIDEO_FORMAT_YUY2 && output_format == GST_VIDEO_FORMAT_NV12)
  {
    GST_DEBUG_OBJECT (synavideoconvertscale,"YUY2 - NV12");
    cv::UMat yv12_image;
    if (resize_reqd)
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->cvUOutFrame, cv::COLOR_YUV2RGB_YUY2);
      cv::resize(synavideoconvertscale->cvUOutFrame, synavideoconvertscale->tmp1,
                 cv::Size(synavideoconvertscale->out_width, synavideoconvertscale->out_height), cv::INTER_AREA);
      cv::cvtColor(synavideoconvertscale->tmp1, yv12_image, cv::COLOR_RGB2YUV_YV12);
      gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
    }
    else
    {
      cv::cvtColor(synavideoconvertscale->cvUInFrame, synavideoconvertscale->tmp1, cv::COLOR_YUV2RGB_YUY2);
      cv::cvtColor(synavideoconvertscale->tmp1, yv12_image, cv::COLOR_RGB2YUV_YV12);
      gst_syna_video_convert_scale_YV12ToNV12(yv12_image, synavideoconvertscale->cvUOutFrame);
    }
    synavideoconvertscale->cvUOutFrame.copyTo(synavideoconvertscale->cvOutImage);
    GST_DEBUG_OBJECT (synavideoconvertscale,"End transform\n");
  }

  synavideoconvertscale->tmp1.release();
  synavideoconvertscale->cvUOutFrame.release();
  synavideoconvertscale->cvUInFrame.release();

  if (inframe.buffer)
  {
    gst_video_frame_unmap(&inframe);
  }

  if (outframe.buffer)
  {
    gst_video_frame_unmap(&outframe);
  }
  return GST_FLOW_OK;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "synavideoconvertscale", GST_RANK_NONE,
      GST_TYPE_SYNA_VIDEO_CONVERT_SCALE);
}

static void
gst_syna_video_convert_scale_YV12ToNV12 (const cv::UMat & input,
    cv::UMat & output)
{
  // Get the input as Mat
  cv::Mat inputMat = input.getMat(cv::ACCESS_READ);
  int width = input.cols;
  int height = input.rows * 2 / 3;
  int stride = (int)inputMat.step[0];

  // Copy input to output as UMat
  input.copyTo(output);
  cv::Mat outputMat = output.getMat(cv::ACCESS_WRITE); // Access output as Mat for element access

  // Create U and V channel matrices
  cv::Mat inV = cv::Mat(cv::Size(width / 2, height / 2), CV_8UC1,
                        (unsigned char *)inputMat.data + stride * height, stride / 2);
  cv::Mat inU = cv::Mat(cv::Size(width / 2, height / 2), CV_8UC1,
                        (unsigned char *)inputMat.data + stride * height + (stride / 2) * (height / 2), stride / 2);

  // Combine U and V planes into NV12 format
  for (int row = 0; row < height / 2; row++)
  {
    for (int col = 0; col < width / 2; col++)
    {
      outputMat.at<uchar>(height + row, 2 * col) = inU.at<uchar>(row, col);     // U plane
      outputMat.at<uchar>(height + row, 2 * col + 1) = inV.at<uchar>(row, col); // V plane
    }
  }
}

static void gst_syna_video_convert_scale_YV444TOYUY2(const cv::UMat &yuv444, cv::UMat &yuy2)
{
  int width = yuv444.cols;
  int height = yuv444.rows;

  // Create an empty 2-channel YUY2 image (since YUY2 is interleaved)
  yuy2.create(height, width, CV_8UC2);

  // Access the YUV444 data
  cv::Mat yuv444Mat = yuv444.getMat(cv::ACCESS_READ);
  cv::Mat yuy2Mat = yuy2.getMat(cv::ACCESS_WRITE);

  // Iterate over each pixel pair to pack YUV444 into YUY2 format
  for (int y = 0; y < height; y++)
  {
    for (int x = 0; x < width; x += 2)
    {
      // Get YUV values from YUV444 (3 channels)
      uchar Y1 = yuv444Mat.at<cv::Vec3b>(y, x)[0];
      uchar U = yuv444Mat.at<cv::Vec3b>(y, x)[1];
      uchar V = yuv444Mat.at<cv::Vec3b>(y, x)[2];
      uchar Y2 = yuv444Mat.at<cv::Vec3b>(y, x + 1)[0];

      // Pack into YUY2 format
      yuy2Mat.at<cv::Vec2b>(y, x)[0] = Y1;     // Y1
      yuy2Mat.at<cv::Vec2b>(y, x)[1] = U;      // U (shared between Y1 and Y2)
      yuy2Mat.at<cv::Vec2b>(y, x + 1)[0] = Y2; // Y2
      yuy2Mat.at<cv::Vec2b>(y, x + 1)[1] = V;  // V (shared between Y1 and Y2)
    }
  }
}

static gboolean gst_syna_video_convert_scale_cvmat_params_from_video_info
    (GstVideoInfo * info, gint * width, gint * height, int *cv_type,
    GError ** err)
{
  GstVideoFormat format;

  format = GST_VIDEO_INFO_FORMAT (info);
  if (!gst_syna_video_convert_scale_cvimage_type_from_video_format (format,
          cv_type, err)) {
    return FALSE;
  }

  *width = GST_VIDEO_INFO_WIDTH (info);
  *height = GST_VIDEO_INFO_HEIGHT (info);

  return TRUE;
}

static gboolean gst_syna_video_convert_scale_cvimage_type_from_video_format
    (GstVideoFormat format, int *cv_type, GError ** err)
{
  const gchar *format_str;

  switch (format) {
    case GST_VIDEO_FORMAT_GRAY8:
      *cv_type = CV_8UC1;
      break;
    case GST_VIDEO_FORMAT_RGB:
      *cv_type = CV_8UC3;
      break;
    case GST_VIDEO_FORMAT_BGR:
      *cv_type = CV_8UC3;
      break;
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_xRGB:
      *cv_type = CV_8UC4;
      break;
    case GST_VIDEO_FORMAT_BGRA:
      *cv_type = CV_8UC4;
      break;
    case GST_VIDEO_FORMAT_ARGB:
      *cv_type = CV_8UC4;
      break;
    case GST_VIDEO_FORMAT_NV12:
      *cv_type = CV_8UC1;
      break;
    case GST_VIDEO_FORMAT_YUY2:
      *cv_type = CV_8UC2;       // YUYV is interleaved 2-channel format
      break;
    case GST_VIDEO_FORMAT_I420:
      *cv_type = CV_8UC1;       // Similar to NV12, only the Y channel
      break;

    default:
      format_str = gst_video_format_to_string (format);
      g_set_error (err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION,
          "Unsupported video format %s", format_str);
      return FALSE;
  }
  return TRUE;
}

#ifndef PACKAGE
#define PACKAGE "gstreamer-plugins-syna"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "gstreamer-plugins-syna"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://www.synaptics.com/"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    synavideoconvertscale,
    "GStreamer plugin for OpenCV based color conversion between specific formats and downscaling",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
