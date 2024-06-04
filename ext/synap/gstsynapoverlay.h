/* GStreamer Synap Overlay
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

#ifndef _GST_SYNAP_OVERLAY_H_
#define _GST_SYNAP_OVERLAY_H_

#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include "synap/classifier.hpp"
#include "synap/detector.hpp"
#include "synap/label_info.hpp"
#include "synapmeta.h"

using namespace std;
using namespace synaptics::synap;

G_BEGIN_DECLS

#define GST_TYPE_SYNAP_OVERLAY   (gst_synap_overlay_get_type())
#define GST_SYNAP_OVERLAY(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_SYNAP_OVERLAY,GstSynapOverlay))
#define GST_SYNAP_OVERLAY_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_SYNAP_OVERLAY,GstSynapOverlayClass))
#define GST_IS_SYNAP_OVERLAY(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_SYNAP_OVERLAY))
#define GST_IS_SYNAP_OVERLAY_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_SYNAP_OVERLAY))

typedef struct _GstSynapOverlay GstSynapOverlay;
typedef struct _GstSynapOverlayClass GstSynapOverlayClass;

struct _GstSynapOverlay
{
  GstBaseTransform base_synapoverlay;

  GstPad *inference_pad;
  GMutex data_lock;

  /* Parameters */
  gchar* label;

  GstVideoInfo videoinfo;
  GstSynapPostProcessingMode mode;
  gpointer overlaydata;
  gint width;
  gint height;
  gfloat confthreshold;

  /* SyNAP */
  LabelInfo *labelinfo;
};

struct _GstSynapOverlayClass
{
  GstBaseTransformClass base_synapoverlay_class;
};

GType gst_synap_overlay_get_type (void);

GST_ELEMENT_REGISTER_DECLARE (synapoverlay);
G_END_DECLS

#endif
