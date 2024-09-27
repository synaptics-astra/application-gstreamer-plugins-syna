/* GStreamer Syna VideoConvertScale
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

#ifndef _GST_SYNA_VIDEO_CONVERT_SCALE_H_
#define _GST_SYNA_VIDEO_CONVERT_SCALE_H_

#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <opencv2/core.hpp>

G_BEGIN_DECLS
#define GST_TYPE_SYNA_VIDEO_CONVERT_SCALE   (gst_syna_video_convert_scale_get_type())
#define GST_SYNA_VIDEO_CONVERT_SCALE(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_SYNA_VIDEO_CONVERT_SCALE,GstSynaVideoConvertScale))
#define GST_SYNA_VIDEO_CONVERT_SCALE_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_SYNA_VIDEO_CONVERT_SCALE,GstSynaVideoConvertScaleClass))
#define GST_IS_SYNA_VIDEO_CONVERT_SCALE(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_SYNA_VIDEO_CONVERT_SCALE))
#define GST_IS_SYNA_VIDEO_CONVERT_SCALE_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_SYNA_VIDEO_CONVERT_SCALE))
typedef struct _GstSynaVideoConvertScale GstSynaVideoConvertScale;
typedef struct _GstSynaVideoConvertScaleClass GstSynaVideoConvertScaleClass;

struct _GstSynaVideoConvertScale
{
  GstBaseTransform base_synavideoconvertscale;

  GstVideoInfo videoinfo;
  GstVideoInfo videooutinfo;
  cv::Mat cvImage;
  cv::Mat cvOutImage;

  gint in_width, in_height;
  int in_cv_type;

  gint out_width, out_height;
  int out_cv_type;
};

struct _GstSynaVideoConvertScaleClass
{
  GstBaseTransformClass base_synavideoconvertscale_class;
};

GType gst_syna_video_convert_scale_get_type (void);

G_END_DECLS
#endif
