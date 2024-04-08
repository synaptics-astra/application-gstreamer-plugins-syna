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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstsynap
 *
 * GStreamer Plugin to do AI processing with Synaptics Neural Network Acceleration and Processing component (SyNAP)
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v filesrc location=xxxx ! synap ! filesink location=yyyy
 * ]|
 * GStreamer plugin to interface with SyNAP for AI processing use-cases.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include "gstsynap.h"

#include "synap/input_data.hpp"
#include "synap/preprocessor.hpp"
#include "synap/network.hpp"
#include "synap/classifier.hpp"
#include "synap/detector.hpp"
#include "synap/label_info.hpp"
#include "synap/file_utils.hpp"

GST_DEBUG_CATEGORY_STATIC (gst_synap_debug_category);
#define GST_CAT_DEFAULT gst_synap_debug_category

/* prototypes */


static void gst_synap_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_synap_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_synap_dispose (GObject * object);
static void gst_synap_finalize (GObject * object);

static gboolean gst_synap_start (GstBaseTransform * trans);
static gboolean gst_synap_stop (GstBaseTransform * trans);
static GstFlowReturn gst_synap_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

enum
{
  PROP_0,
  PROP_MODEL,
  PROP_MODE,
};

/* pad templates */

static GstStaticPadTemplate gst_synap_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ RGB }"))
    );

static GstStaticPadTemplate gst_synap_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ RGB }"))
    );


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstSynap, gst_synap, GST_TYPE_BASE_TRANSFORM,
  GST_DEBUG_CATEGORY_INIT (gst_synap_debug_category, "synap", 0,
  "debug category for synap element"));

static void
gst_synap_class_init (GstSynapClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS(klass),
      &gst_synap_src_template);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS(klass),
      &gst_synap_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Plugin for Synaptics AI", "Generic/Video", "GStreamer layer for Synaptics Neural Network Acceleration and Processing component",
      "https://www.synaptics.com/");

  gobject_class->set_property = gst_synap_set_property;
  gobject_class->get_property = gst_synap_get_property;
  gobject_class->dispose = gst_synap_dispose;
  gobject_class->finalize = gst_synap_finalize;
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_synap_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_synap_stop);
  base_transform_class->transform_ip = GST_DEBUG_FUNCPTR (gst_synap_transform_ip);
  g_object_class_install_property (gobject_class, PROP_MODEL,
      g_param_spec_string ("model", "Model filepath",
          "File path to the model file",
          NULL, (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_string ("mode", "Mode", "Post Processing mode", NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_synap_init (GstSynap *synap)
{
    static gsize res = FALSE;
    static const gchar *tags[] = { NULL };
    if (g_once_init_enter (&res)) {
        gst_meta_register_custom ("GstSynapMeta", tags, NULL, NULL, NULL);
        g_once_init_leave (&res, TRUE);
    }

    // Initialize parameters
    synap->model = NULL;
    synap->mode = NULL;
    synap->ppmode = POST_PROCESSING_MODE_NONE;
}

void
gst_synap_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSynap *synap = GST_SYNAP (object);

  GST_DEBUG_OBJECT (synap, "set_property");

  g_return_if_fail (synap != NULL);
  g_return_if_fail (GST_IS_SYNAP (synap));

  switch (property_id) {
    case PROP_MODEL:
    {
        if (g_value_get_string (value) != NULL) {
            synap->model =  g_value_dup_string (value);
            GST_INFO ("Model: %s", synap->model);
        }
    }
    break;

    case PROP_MODE:
    {
        if (g_value_get_string (value) != NULL) {
            synap->mode =  g_value_dup_string (value);
            GST_INFO ("Mode: %s", synap->mode);
        }
    }
    break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSynap *synap = GST_SYNAP (object);

  GST_DEBUG_OBJECT (synap, "get_property");

  switch (property_id) {
    case PROP_MODEL:
    {
        g_value_set_string (value, synap->model);
    }
    break;

    case PROP_MODE:
    {
        g_value_set_string (value, synap->mode);
    }
    break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_synap_dispose (GObject * object)
{
  GstSynap *synap = GST_SYNAP (object);

  GST_DEBUG_OBJECT (synap, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_synap_parent_class)->dispose (object);
}

void
gst_synap_finalize (GObject * object)
{
  GstSynap *synap = GST_SYNAP (object);

  GST_DEBUG_OBJECT (synap, "finalize");

  g_return_if_fail (synap != NULL);
  g_return_if_fail (GST_IS_SYNAP (synap));

  /* clean up object here */
  g_free (synap->model);
  g_free (synap->mode);

  G_OBJECT_CLASS (gst_synap_parent_class)->finalize (object);
}

/* states */
static gboolean
gst_synap_start (GstBaseTransform * trans)
{
  GstSynap *synap = GST_SYNAP (trans);

  GST_DEBUG_OBJECT (synap, "start");

  g_return_val_if_fail (synap != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP (synap), FALSE);

  GST_DEBUG_OBJECT (synap, "start");

  // Validate parameters
  if (synap->model == NULL) {
    GST_ERROR ("Invalid Model Name");
    return FALSE;
  }

  if (synap->mode != NULL) {
      if (g_strcmp0 (synap->mode, "classifier") == 0) {
          GST_INFO ("Post-Processing Mode: Classifier");
          synap->ppmode = POST_PROCESSING_MODE_CLASSIFIER;
      }
  }

  // Create SyNAP objects
  synap->network = new Network();
  g_return_val_if_fail (synap->network != NULL, FALSE);

  if (!synap->network->load_model(synap->model, ""))  {
       GST_ERROR ("Model cannot be loaded");
       return FALSE;
  }

  if (synap->ppmode == POST_PROCESSING_MODE_CLASSIFIER) {
    synap->classifier = new Classifier(2);
    g_return_val_if_fail (synap->classifier != NULL, FALSE);

    synap->info = new LabelInfo();
    g_return_val_if_fail (synap->info != NULL, FALSE);

    if (!synap->info->init (file_find_up("info.json", filename_path(synap->model)))) {
        GST_ERROR ("Missing label file");
        return FALSE;
    }
  }

  return TRUE;
}

static gboolean
gst_synap_stop (GstBaseTransform * trans)
{
  GstSynap *synap = GST_SYNAP (trans);

  GST_DEBUG_OBJECT (synap, "stop");

  g_return_val_if_fail (synap != NULL, FALSE);
  g_return_val_if_fail (GST_IS_SYNAP (synap), FALSE);

  GST_DEBUG_OBJECT (synap, "stop");
  if (synap->network != NULL)
      delete synap->network;
  if (synap->preprocessor != NULL)
      delete synap->preprocessor;
  if (synap->classifier != NULL)
      delete synap->classifier;
  if (synap->detector != NULL)
      delete synap->detector;
  return TRUE;
}

static GstFlowReturn
gst_synap_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstSynap *synap = GST_SYNAP (trans);
  GST_DEBUG_OBJECT (synap, "transform");

  if (!buf) {
      return GST_FLOW_ERROR;
  }

  GstMapInfo map;
  gst_buffer_map (buf, &map, GST_MAP_READ);
  guint8 *raw = (guint8 *) map.data;

  /* Process with SyNAP */
  synap->network->inputs[0].assign (raw, map.size);

  /* Execute inference */
  if (!synap->network->predict()) {
    gst_buffer_unmap (buf, &map);
    GST_ERROR("Inference failed");
    return GST_FLOW_ERROR;
  }
  gst_buffer_unmap (buf, &map);

  /* Post Process output */
  if (synap->ppmode == POST_PROCESSING_MODE_CLASSIFIER) {
    // Image Classification
    // Postprocess network outputs
    Classifier::Result result = synap->classifier->process(synap->network->outputs);
    if (!result.success) {
        GST_ERROR ("Classification failed");
        return GST_FLOW_ERROR;
    }

    // Convert classification result to json string
    std::string resstr = to_json_str(result);
	GstCustomMeta *meta = gst_buffer_add_custom_meta (buf, "GstSynapMeta");
	if (meta) {
		GstStructure *s = gst_custom_meta_get_structure (meta);
		if (s) {
			gst_structure_set (s, "ic-result", G_TYPE_STRING, resstr.c_str(), NULL);
		}
	}
  }
  return GST_FLOW_OK;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "synap", GST_RANK_NONE,
      GST_TYPE_SYNAP);
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
    synap,
    "GStreamer plugin to do AI processing with Synaptics Neural Network Acceleration and Processing component (SyNAP)",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
