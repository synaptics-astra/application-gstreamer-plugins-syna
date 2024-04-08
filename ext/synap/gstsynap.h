/* GStreamer SyNAP
 * Copyright (C) 2020 Synaptics Incorporated. All rights reserved.
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

#ifndef _GST_SYNAP_H_
#define _GST_SYNAP_H_

#include <gst/base/gstbasetransform.h>
#include "synap/preprocessor.hpp"
#include "synap/network.hpp"
#include "synap/classifier.hpp"
#include "synap/detector.hpp"
#include "synap/label_info.hpp"

using namespace std;
using namespace synaptics::synap;

G_BEGIN_DECLS

#define GST_TYPE_SYNAP   (gst_synap_get_type())
#define GST_SYNAP(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_SYNAP,GstSynap))
#define GST_SYNAP_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_SYNAP,GstSynapClass))
#define GST_IS_SYNAP(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_SYNAP))
#define GST_IS_SYNAP_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_SYNAP))

typedef struct _GstSynap GstSynap;
typedef struct _GstSynapClass GstSynapClass;

/**
 * @brief Post-Processing Mode
 */
typedef enum {
  POST_PROCESSING_MODE_NONE         = 0,    /**< Mode None */
  POST_PROCESSING_MODE_CLASSIFIER   = 1,    /**< Classification Mode */
  POST_PROCESSING_MODE_DETECTOR     = 2,    /**< Detection Mode */
} synap_postprocessing_mode;

struct _GstSynap
{
  GstBaseTransform base_synap;

  /* Parameters */
  gchar* model;
  gchar* mode;

  synap_postprocessing_mode ppmode;

  /* SyNAP */
  Network *network;
  Preprocessor *preprocessor;
  Classifier *classifier;
  Detector *detector;
  LabelInfo *info;
};

struct _GstSynapClass
{
  GstBaseTransformClass base_synap_class;
};

GType gst_synap_get_type (void);

G_END_DECLS

#endif
