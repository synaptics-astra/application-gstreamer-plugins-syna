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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_SYNAP_IMAGE_PROC_H_
#define _GST_SYNAP_IMAGE_PROC_H_

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include "synap/preprocessor.hpp"
#include "synap/network.hpp"

using namespace std;
using namespace synaptics::synap;

G_BEGIN_DECLS

#define GST_TYPE_SYNAP_IMAGE_PROC   (gst_synap_image_proc_get_type())
#define GST_SYNAP_IMAGE_PROC(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_SYNAP_IMAGE_PROC, GstSynapImageProc))
#define GST_SYNAP_IMAGE_PROC_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_SYNAP_IMAGE_PROC, GstSynapImageProcClass))
#define GST_IS_SYNAP_IMAGE_PROC(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_SYNAP_IMAGE_PROC))
#define GST_IS_SYNAP_IMAGE_PROC_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_SYNAP_IMAGE_PROC))

typedef struct _GstSynapImageProc GstSynapImageProc;
typedef struct _GstSynapImageProcClass GstSynapImageProcClass;

struct _GstSynapImageProc
{
  GstBaseTransform base_synapimageproc;

  GstVideoInfo videoinfo;
  GstVideoInfo videooutinfo;

  gint model_out_width, model_out_height;
  gint in_width, in_height;
  gint out_width, out_height;

  /* Parameters */
  gchar* model;

  /* SyNAP */
  Network *network;
  Preprocessor *preprocessor;
  Allocator* allocator;
};

struct _GstSynapImageProcClass
{
  GstBaseTransformClass base_synapimageproc_class;
};

GType gst_synap_image_proc_get_type (void);

GST_ELEMENT_REGISTER_DECLARE (synapimageproc);
G_END_DECLS

#endif
