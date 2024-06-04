/* GStreamer Synap Infer
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

#ifndef _GST_SYNAP_INFER_H_
#define _GST_SYNAP_INFER_H_

#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include "synap/preprocessor.hpp"
#include "synap/network.hpp"
#include "synap/classifier.hpp"
#include "synap/detector.hpp"
#include "synapmeta.h"

using namespace std;
using namespace synaptics::synap;

G_BEGIN_DECLS

#define GST_TYPE_SYNAP_INFER   (gst_synap_infer_get_type())
#define GST_SYNAP_INFER(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_SYNAP_INFER,GstSynapInfer))
#define GST_SYNAP_INFER_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_SYNAP_INFER,GstSynapInferClass))
#define GST_IS_SYNAP_INFER(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_SYNAP_INFER))
#define GST_IS_SYNAP_INFER_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_SYNAP_INFER))

typedef struct _GstSynapInfer GstSynapInfer;
typedef struct _GstSynapInferClass GstSynapInferClass;

/**
 * @brief Synap Output Type
 */
    typedef enum
{
  GST_SYNAP_OUTPUT_MODE_NONE    = 0,               /**< Mode None */
  GST_SYNAP_OUTPUT_MODE_OVERLAY = 1,               /**< Output for overlay */
  GST_SYNAP_OUTPUT_MODE_JSON    = 2,               /**< Output as Json string */
} GstSynapOutputMode;

struct _GstSynapInfer
{
  GstBaseTransform base_synapinfer;

  GstVideoInfo videoinfo;

  /* Parameters */
  gchar* model;
  gchar* mode;
  gchar* output;
  gfloat confthreshold;
  gint numinference;
  gint frame_interval;
  gint frame_counter;
  GstSynapPostProcessingMode postproc_mode;
  GstSynapOutputMode outputmode;

  /* SyNAP */
  Network *network;
  Preprocessor *preprocessor;
  Classifier *classifier;
  Detector *detector;
};

struct _GstSynapInferClass
{
  GstBaseTransformClass base_synapinfer_class;
};

GType gst_synap_infer_get_type (void);

GST_ELEMENT_REGISTER_DECLARE (synapinfer);
G_END_DECLS

#endif
