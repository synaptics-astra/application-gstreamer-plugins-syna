/* GStreamer SynaOverlay
 * Copyright (C) 2026 Synaptics Incorporated. All rights reserved.
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
 * SECTION:element-gstsynaoverlay ( synaoverlay )
 *
 * GStreamer Plugin for overlaying images with support for both static and external data sources
 *
 * Two Modes of Operation:
 *
 * 1. STATIC MODE (standalone):
 * - Uses GStreamer properties: image-path, image-x, image-y
 * - Loads image from file ONCE, caches it in memory
 * - Works without external application integration
 * - Simple single image overlay
 * - Sample usage: gst-launch-1.0 filesrc location=/path/to/video.mp4 ! qtdemux name=demux demux.video_0 ! h264parse ! avdec_h264 ! synavideoconvertscale ! video/x-raw,format=RGB ! synaoverlay image-path=/path/to/image.png image-x=xx image-y=xx mode=static ! synavideoconvertscale ! video/x-raw, format=BGRA ! waylandsink
 *
 * 2. EXTERNAL MODE (app-driven):
 * :- Using image path
 *    - External application sends image path with coordinates
 *    - Plugin loads image, adds to overlay list
 * :- Using raw pointer
 *    - External application sends raw image data pointer with metadata (size, format, coordinates)
 *    - Plugin wraps pointer in OpenCV Mat, adds to overlay list
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <opencv2/imgproc.hpp>
#include <cstdio>
#include <new>
#include "gstsynaoverlay.h"

GST_DEBUG_CATEGORY_STATIC (gst_syna_overlay_debug_category);
#define GST_CAT_DEFAULT gst_syna_overlay_debug_category

/* prototypes */
static void gst_syna_overlay_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_syna_overlay_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_syna_overlay_dispose (GObject * object);
static void gst_syna_overlay_finalize (GObject * object);

static gboolean gst_syna_overlay_set_caps (GstBaseTransform * trans,
    GstCaps * incaps, GstCaps * outcaps);
static gboolean gst_syna_overlay_start (GstBaseTransform * trans);
static gboolean gst_syna_overlay_stop (GstBaseTransform * trans);
static GstFlowReturn gst_syna_overlay_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

/* Forward declarations for static helper functions */
static cv::Mat gst_syna_overlay_load_image (const gchar * path);
static void gst_syna_overlay_draw_overlay (cv::Mat & frame,
  const cv::Mat & overlay_image, gint x, gint y, GstVideoFormat frame_format);

enum
{
  PROP_0,
  PROP_MODE,
  PROP_IMAGE_PATH,
  PROP_IMAGE_X,
  PROP_IMAGE_Y,
  PROP_OVERLAY_COMMAND,
  PROP_OVERLAY_IMAGE_DATA,
  PROP_OVERLAY_IMAGE_SIZE
};

/* pad templates */
static GstStaticPadTemplate gst_syna_overlay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE("{ RGB, BGR }"))
    );

static GstStaticPadTemplate gst_syna_overlay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE("{ RGB, BGR }"))
    );

/* class initialization */
G_DEFINE_TYPE_WITH_CODE (GstSynaOverlay, gst_syna_overlay,
    GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_syna_overlay_debug_category,
    "synaoverlay", 0,
    "debug category for synaoverlay element"));
GST_ELEMENT_REGISTER_DEFINE (synaoverlay, "synaoverlay",
    GST_RANK_NONE, GST_TYPE_SYNA_OVERLAY);

static gboolean
plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (synaoverlay, plugin);
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
    synaoverlay,
    "GStreamer SynaOverlay plugin",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)

static void
gst_syna_overlay_class_init (GstSynaOverlayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_syna_overlay_src_template);
  gst_element_class_add_static_pad_template (GST_ELEMENT_CLASS (klass),
      &gst_syna_overlay_sink_template);

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Plugin for Synaptics OpenCV External Source Overlay", "Generic/Video",
      "GStreamer layer for Synaptics OpenCV External Source Overlay",
      "https://www.synaptics.com/");

  gobject_class->set_property = gst_syna_overlay_set_property;
  gobject_class->get_property = gst_syna_overlay_get_property;
  gobject_class->dispose = gst_syna_overlay_dispose;
  gobject_class->finalize = gst_syna_overlay_finalize;
  base_transform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_syna_overlay_set_caps);
  base_transform_class->start = GST_DEBUG_FUNCPTR (gst_syna_overlay_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_syna_overlay_stop);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_syna_overlay_transform_ip);

  /* Properties */
  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_string ("mode", "Overlay Mode",
          "Operation mode: 'static' (properties) or 'external' (app-provided data)",
          "static", (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_IMAGE_PATH,
      g_param_spec_string ("image-path", "Image Path",
          "Path to overlay image (used in static mode only)",
          "", (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_IMAGE_X,
      g_param_spec_int ("image-x", "Image X",
          "X coordinate for image placement (static mode)",
          0, G_MAXINT, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_IMAGE_Y,
      g_param_spec_int ("image-y", "Image Y",
          "Y coordinate for image placement (static mode)",
          0, G_MAXINT, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_OVERLAY_COMMAND,
      g_param_spec_string ("overlay-command", "Overlay Command",
        "Command string: 'add:/path/to/image.png:100:150' to add overlay from file, 'add-pointer:x:y:width:height:channels:data-size' to add from raw pointer (channels=3 for BGR or 4 for BGRA), or 'clear' to clear all overlays.",
          "", (GParamFlags) (G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject_class, PROP_OVERLAY_IMAGE_DATA,
      g_param_spec_pointer ("overlay-image-data", "Overlay Image Data",
        "Pointer to raw image data (BGR or BGRA). Caller retains ownership and must keep memory valid until add-pointer is consumed.",
          (GParamFlags) (G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)));

    g_object_class_install_property (gobject_class, PROP_OVERLAY_IMAGE_SIZE,
      g_param_spec_uint64 ("overlay-image-size", "Overlay Image Size",
        "Size in bytes of overlay-image-data for bounds validation",
        0, G_MAXUINT64, 0,
        (GParamFlags) (G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_syna_overlay_init (GstSynaOverlay * overlay)
{
  /* GObject allocation does not run C++ constructors for instance fields. */
  new (&overlay->cached_static_image) cv::Mat ();
  new (&overlay->cached_external_items) std::vector<OverlayItem> ();

  g_mutex_init (&overlay->static_lock);
  g_mutex_init (&overlay->external_lock);

  /* Initialize properties */
  overlay->mode = g_strdup ("static");
  overlay->image_path = g_strdup ("");
  overlay->image_x = 0;
  overlay->image_y = 0;

  /* Initialize static mode caching */
  overlay->static_image_loaded = FALSE;

  /* Initialize external data */
  overlay->external_data = new ExternalOverlayData ();
  overlay->cached_external_items.clear ();
  overlay->data_changed_flag = FALSE;

  /* Initialize pending pointer */
  overlay->pending_image_data = NULL;
  overlay->pending_image_size = 0;

  GST_DEBUG ("SynaOverlay plugin initialized");
}

void
gst_syna_overlay_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSynaOverlay *overlay = GST_SYNA_OVERLAY (object);

  GST_DEBUG_OBJECT (overlay, "set_property");

  switch (property_id) {
    case PROP_MODE:
    {
      g_mutex_lock (&overlay->static_lock);
      g_free (overlay->mode);
      overlay->mode = g_value_dup_string (value);
      GST_INFO ("Overlay mode set to: %s", overlay->mode);
      g_mutex_unlock (&overlay->static_lock);
    }
    break;

    case PROP_IMAGE_PATH:
    {
      gchar *new_path = g_value_dup_string (value);
      cv::Mat loaded_image;
      gboolean load_attempted = FALSE;

      if (new_path != NULL && new_path[0] != '\0') {
        load_attempted = TRUE;
        loaded_image = gst_syna_overlay_load_image (new_path);
      }

      g_mutex_lock (&overlay->static_lock);
      g_free (overlay->image_path);
      overlay->image_path = new_path;
      overlay->cached_static_image.release ();
      if (load_attempted && !loaded_image.empty ()) {
        overlay->cached_static_image = loaded_image;
      }
      overlay->static_image_loaded = load_attempted;
      GST_INFO ("Image path set to: %s", overlay->image_path ? overlay->image_path : "");
      g_mutex_unlock (&overlay->static_lock);
    }
    break;

    case PROP_IMAGE_X:
    {
      gint image_x = g_value_get_int (value);
      g_mutex_lock (&overlay->static_lock);
      overlay->image_x = image_x;
      g_mutex_unlock (&overlay->static_lock);
      GST_INFO ("Image X position: %d", image_x);
    }
    break;

    case PROP_IMAGE_Y:
    {
      gint image_y = g_value_get_int (value);
      g_mutex_lock (&overlay->static_lock);
      overlay->image_y = image_y;
      g_mutex_unlock (&overlay->static_lock);
      GST_INFO ("Image Y position: %d", image_y);
    }
    break;

    case PROP_OVERLAY_COMMAND:
    {
      const gchar *cmd = g_value_get_string (value);
      if (cmd == NULL || strlen (cmd) == 0)
        break;

      if (g_str_has_prefix (cmd, "add:")) {
        /* Parse: "add:/path/to/image.png:x:y" */
        const gchar *cmd_data = cmd + 4;  /* Skip "add:" prefix */
        gchar *last_colon = g_strrstr (cmd_data, ":");
        if (last_colon == NULL)
          break;
        gchar *second_last_colon = g_strrstr_len (cmd_data, last_colon - cmd_data, ":");
        if (second_last_colon == NULL)
          break;

        gchar *image_path = g_strndup (cmd_data, second_last_colon - cmd_data);
        gint x = atoi (second_last_colon + 1);
        gint y = atoi (last_colon + 1);

        if (overlay->external_data != NULL) {
          cv::Mat image = gst_syna_overlay_load_image (image_path);
          if (!image.empty ()) {
            g_mutex_lock (&overlay->external_lock);
            overlay->external_data->add_item (image, x, y);
            overlay->data_changed_flag = TRUE;
            g_mutex_unlock (&overlay->external_lock);
            GST_INFO ("Added overlay from %s at (%d, %d)", image_path, x, y);
          } else {
            GST_WARNING ("Failed to load image: %s", image_path);
          }
        }
        g_free (image_path);
      } else if (g_str_has_prefix (cmd, "add-pointer:")) {
        /* Parse: "add-pointer:x:y:width:height:channels:data-size" */
        const gchar *cmd_data = cmd + 12;  /* Skip "add-pointer:" prefix */

        /* Parse x:y:width:height:channels:data-size */
        gint x = 0, y = 0, width = 0, height = 0, channels = 3;
        guint64 data_size = 0;
        gint parsed = sscanf (cmd_data, "%d:%d:%d:%d:%d:%" G_GUINT64_FORMAT,
            &x, &y, &width, &height, &channels, &data_size);

        guint8 *image_data = NULL;
        guint64 image_size = 0;
        g_mutex_lock (&overlay->external_lock);
        image_data = overlay->pending_image_data;
        image_size = overlay->pending_image_size;
        g_mutex_unlock (&overlay->external_lock);

        if (parsed != 6 || data_size == 0 || image_data == NULL ||
            overlay->external_data == NULL || width <= 0 || height <= 0) {
          GST_WARNING ("add-pointer rejected: malformed command or missing data (x=%d, y=%d, w=%d, h=%d, ch=%d, cmd-size=%" G_GUINT64_FORMAT ", ptr=%p, prop-size=%" G_GUINT64_FORMAT ")",
              x, y, width, height, channels, data_size, image_data, image_size);

          g_mutex_lock (&overlay->external_lock);
          overlay->pending_image_data = NULL;
          overlay->pending_image_size = 0;
          g_mutex_unlock (&overlay->external_lock);
          break;
        }

        /* Validate channels (3 for BGR or 4 for BGRA) */
        if (channels != 3 && channels != 4) {
          GST_WARNING ("Invalid channel count: %d (expected 3 or 4)", channels);
          g_mutex_lock (&overlay->external_lock);
          overlay->pending_image_data = NULL;
          overlay->pending_image_size = 0;
          g_mutex_unlock (&overlay->external_lock);
          break;
        }

        guint64 width_u64 = (guint64) width;
        guint64 height_u64 = (guint64) height;
        guint64 channels_u64 = (guint64) channels;
        if (width_u64 > G_MAXUINT64 / height_u64 ||
            (width_u64 * height_u64) > G_MAXUINT64 / channels_u64) {
          GST_WARNING ("add-pointer rejected: dimension overflow (w=%d, h=%d, ch=%d)",
              width, height, channels);
          g_mutex_lock (&overlay->external_lock);
          overlay->pending_image_data = NULL;
          overlay->pending_image_size = 0;
          g_mutex_unlock (&overlay->external_lock);
          break;
        }

        guint64 expected_size = width_u64 * height_u64 * channels_u64;
        if (image_size < expected_size || data_size < expected_size || data_size != image_size) {
          GST_WARNING ("add-pointer rejected: invalid size (expected>=%" G_GUINT64_FORMAT ", cmd-size=%" G_GUINT64_FORMAT ", prop-size=%" G_GUINT64_FORMAT ")",
              expected_size, data_size, image_size);
          g_mutex_lock (&overlay->external_lock);
          overlay->pending_image_data = NULL;
          overlay->pending_image_size = 0;
          g_mutex_unlock (&overlay->external_lock);
          break;
        }

        /* Wrap with explicit row stride and clone immediately to own safe storage. */
        gint cv_type = (channels == 4) ? CV_8UC4 : CV_8UC3;
        cv::Mat wrapped (height, width, cv_type, image_data, width * channels);
        cv::Mat image = wrapped.clone ();

        if (!image.empty ()) {
          g_mutex_lock (&overlay->external_lock);
          overlay->external_data->add_item (image, x, y);
          overlay->data_changed_flag = TRUE;
          overlay->pending_image_data = NULL;
          overlay->pending_image_size = 0;
          g_mutex_unlock (&overlay->external_lock);

          GST_INFO ("Added overlay from pointer at (%d, %d) size %dx%d with %d channels",
              x, y, width, height, channels);
        } else {
          GST_WARNING ("Failed to create overlay from pointer");
          g_mutex_lock (&overlay->external_lock);
          overlay->pending_image_data = NULL;
          overlay->pending_image_size = 0;
          g_mutex_unlock (&overlay->external_lock);
        }
      } else if (g_strcmp0 (cmd, "clear") == 0) {
        /* Clear all overlays */
        if (overlay->external_data != NULL) {
          g_mutex_lock (&overlay->external_lock);
          overlay->external_data->clear ();
          overlay->data_changed_flag = TRUE;
          g_mutex_unlock (&overlay->external_lock);
          GST_INFO ("Cleared all overlays");
        }
      } else {
        GST_WARNING ("Unknown overlay command: %s", cmd);
      }
    }
    break;

    case PROP_OVERLAY_IMAGE_DATA:
    {
      /* Store pointer to raw image data until validated add-pointer command consumes it. */
      guint8 *image_data = (guint8*)g_value_get_pointer(value);

      g_mutex_lock (&overlay->external_lock);
      overlay->pending_image_data = image_data;
      g_mutex_unlock (&overlay->external_lock);
      GST_DEBUG ("Stored overlay image pointer %p", image_data);
    }
    break;

    case PROP_OVERLAY_IMAGE_SIZE:
    {
      guint64 image_size = g_value_get_uint64 (value);
      g_mutex_lock (&overlay->external_lock);
      overlay->pending_image_size = image_size;
      g_mutex_unlock (&overlay->external_lock);
      GST_DEBUG ("Stored overlay image size %" G_GUINT64_FORMAT, image_size);
    }
    break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_syna_overlay_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstSynaOverlay *overlay = GST_SYNA_OVERLAY (object);

  GST_DEBUG_OBJECT (overlay, "get_property");

  switch (property_id) {
    case PROP_MODE:
    {
      g_mutex_lock (&overlay->static_lock);
      g_value_set_string (value, overlay->mode);
      g_mutex_unlock (&overlay->static_lock);
    }
    break;

    case PROP_IMAGE_PATH:
    {
      g_mutex_lock (&overlay->static_lock);
      g_value_set_string (value, overlay->image_path);
      g_mutex_unlock (&overlay->static_lock);
    }
    break;

    case PROP_IMAGE_X:
    {
      g_mutex_lock (&overlay->static_lock);
      g_value_set_int (value, overlay->image_x);
      g_mutex_unlock (&overlay->static_lock);
    }
    break;

    case PROP_IMAGE_Y:
    {
      g_mutex_lock (&overlay->static_lock);
      g_value_set_int (value, overlay->image_y);
      g_mutex_unlock (&overlay->static_lock);
    }
    break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_syna_overlay_dispose (GObject * object)
{
  GstSynaOverlay *overlay = GST_SYNA_OVERLAY (object);

  GST_DEBUG_OBJECT (overlay, "dispose");

  G_OBJECT_CLASS (gst_syna_overlay_parent_class)->dispose (object);
}

void
gst_syna_overlay_finalize (GObject * object)
{
  GstSynaOverlay *overlay = GST_SYNA_OVERLAY (object);

  GST_DEBUG_OBJECT (overlay, "finalize");

  /* clean up object here */
  g_free (overlay->mode);
  g_free (overlay->image_path);
  overlay->cached_static_image.release ();

  if (overlay->external_data != NULL) {
    delete overlay->external_data;
    overlay->external_data = NULL;
  }

  overlay->cached_external_items.clear ();
  overlay->pending_image_data = NULL;
  overlay->pending_image_size = 0;

  g_mutex_clear (&overlay->static_lock);
  g_mutex_clear (&overlay->external_lock);

  overlay->cached_external_items.~vector<OverlayItem> ();
  overlay->cached_static_image.~Mat ();

  G_OBJECT_CLASS (gst_syna_overlay_parent_class)->finalize (object);
}

static gboolean
gst_syna_overlay_set_caps (GstBaseTransform * trans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstSynaOverlay *overlay = GST_SYNA_OVERLAY (trans);

  GST_DEBUG_OBJECT (overlay, "set_caps");
  if (!gst_video_info_from_caps (&overlay->videoinfo, incaps))
    return FALSE;

  return TRUE;
}

/* states */
static gboolean
gst_syna_overlay_start (GstBaseTransform * trans)
{
  GstSynaOverlay *overlay = GST_SYNA_OVERLAY (trans);

  GST_DEBUG_OBJECT (overlay, "start");

  return TRUE;
}

static gboolean
gst_syna_overlay_stop (GstBaseTransform * trans)
{
  GstSynaOverlay *overlay = GST_SYNA_OVERLAY (trans);

  GST_DEBUG_OBJECT (overlay, "stop");

  g_mutex_lock (&overlay->static_lock);
  overlay->cached_static_image.release ();
  overlay->static_image_loaded = FALSE;
  g_mutex_unlock (&overlay->static_lock);

  g_mutex_lock (&overlay->external_lock);
  if (overlay->external_data != NULL) {
    overlay->external_data->clear ();
  }
  overlay->cached_external_items.clear ();
  overlay->data_changed_flag = FALSE;
  overlay->pending_image_data = NULL;
  overlay->pending_image_size = 0;
  g_mutex_unlock (&overlay->external_lock);

  return TRUE;
}

/**
 * Load image from file path (static mode only).
 * Called only once per path change.
 */
static cv::Mat
gst_syna_overlay_load_image (const gchar * path)
{
  if (path == NULL || path[0] == '\0')
    return cv::Mat ();

  try {
    cv::Mat image = cv::imread (path, cv::IMREAD_UNCHANGED);
    if (image.empty ()) {
      GST_ERROR ("Could not load image from %s", path);
      return cv::Mat ();
    }

    /* Ensure image has alpha channel (convert BGR to BGRA if needed) */
    if (image.channels () == 3) {
      cv::Mat bgra;
      cv::cvtColor (image, bgra, cv::COLOR_BGR2BGRA);
      GST_INFO ("Loaded image: %s, shape: %dx%d", path, image.cols, image.rows);
      return bgra;
    }

    GST_INFO ("Loaded image: %s, shape: %dx%d", path, image.cols, image.rows);
    return image;
  } catch (const std::exception & e) {
    GST_ERROR ("Error loading image: %s", e.what ());
    return cv::Mat ();
  }
}

/**
 * Draw overlay image with alpha blending.
 * Handles clipping to frame bounds.
 */
static void
gst_syna_overlay_draw_overlay (
    cv::Mat & frame, const cv::Mat & overlay_image, gint x, gint y,
    GstVideoFormat frame_format)
{
  if (overlay_image.empty () || frame.empty ())
    return;

  gint frame_h = frame.rows;
  gint frame_w = frame.cols;
  gint h = overlay_image.rows;
  gint w = overlay_image.cols;

  /* Calculate region of interest (clamp to frame bounds) */
  gint x1 = (x < 0) ? 0 : x;
  gint y1 = (y < 0) ? 0 : y;
  gint x2 = (x + w < frame_w) ? (x + w) : frame_w;
  gint y2 = (y + h < frame_h) ? (y + h) : frame_h;

  /* Skip if overlay is completely outside frame */
  if (x2 <= x1 || y2 <= y1)
    return;

  /* Calculate corresponding region in overlay */
  gint ox1 = x1 - x;
  gint oy1 = y1 - y;
  gint ox2 = ox1 + (x2 - x1);
  gint oy2 = oy1 + (y2 - y1);

  /* Clamp overlay region */
  if (ox1 < 0) ox1 = 0;
  if (oy1 < 0) oy1 = 0;
  if (ox2 > w) ox2 = w;
  if (oy2 > h) oy2 = h;

  try {
    cv::Mat frame_region = frame (cv::Rect (x1, y1, x2 - x1, y2 - y1));
    cv::Mat overlay_region = overlay_image (cv::Rect (ox1, oy1, ox2 - ox1, oy2 - oy1));

    /* Apply alpha blending */
    if (overlay_region.channels () == 4) {
      cv::Mat overlay_3c;
      if (frame_format == GST_VIDEO_FORMAT_RGB) {
        cv::cvtColor (overlay_region, overlay_3c, cv::COLOR_BGRA2RGB);
      } else {
        cv::cvtColor (overlay_region, overlay_3c, cv::COLOR_BGRA2BGR);
      }

      cv::Mat alpha;
      cv::extractChannel (overlay_region, alpha, 3);

      cv::Mat alpha_f;
      alpha.convertTo (alpha_f, CV_32FC1, 1.0 / 255.0);

      cv::Mat alpha_3c;
      cv::Mat alpha_channels[] = { alpha_f, alpha_f, alpha_f };
      cv::merge (alpha_channels, 3, alpha_3c);

      cv::Mat frame_f;
      frame_region.convertTo (frame_f, CV_32FC3);
      cv::Mat overlay_f;
      overlay_3c.convertTo (overlay_f, CV_32FC3);

      cv::Mat fg;
      cv::multiply (overlay_f, alpha_3c, fg);

      cv::Mat inv_alpha_3c = cv::Scalar::all (1.0f) - alpha_3c;
      cv::Mat bg;
      cv::multiply (frame_f, inv_alpha_3c, bg);

      cv::Mat blended = fg + bg;
      blended.convertTo (frame_region, CV_8UC3);
    } else if (overlay_region.channels () == 3) {
      if (frame_format == GST_VIDEO_FORMAT_RGB) {
        cv::Mat overlay_rgb;
        cv::cvtColor (overlay_region, overlay_rgb, cv::COLOR_BGR2RGB);
        overlay_rgb.copyTo (frame_region);
      } else {
        /* BGR image without alpha - direct copy */
        overlay_region.copyTo (frame_region);
      }
    }
  } catch (const std::exception & e) {
    GST_ERROR ("Error drawing overlay: %s", e.what ());
  }
}

static GstFlowReturn
gst_syna_overlay_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstSynaOverlay *overlay = GST_SYNA_OVERLAY (trans);

  GST_DEBUG_OBJECT (overlay, "transform_ip");

  GstVideoFrame vframe;
  if (!gst_video_frame_map (&vframe, &overlay->videoinfo, buf,
          (GstMapFlags) GST_MAP_READWRITE)) {
    GST_ERROR ("Failed to map video frame");
    return GST_FLOW_ERROR;
  }

  GstVideoFormat format = GST_VIDEO_INFO_FORMAT (&overlay->videoinfo);
  gint width = GST_VIDEO_FRAME_COMP_WIDTH (&vframe, 0);
  gint height = GST_VIDEO_FRAME_COMP_HEIGHT (&vframe, 0);
  gint stride = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0);

  /* Create OpenCV Mat with explicit stride to honor padded rows/layout. */
  cv::Mat frame;
  if (format == GST_VIDEO_FORMAT_RGB || format == GST_VIDEO_FORMAT_BGR) {
    frame = cv::Mat (height, width, CV_8UC3,
        GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0), stride);
  } else {
    GST_ERROR ("Unsupported video format");
    gst_video_frame_unmap (&vframe);
    return GST_FLOW_ERROR;
  }

  gboolean is_static_mode = TRUE;
  std::vector<OverlayItem> external_items;
  g_mutex_lock (&overlay->static_lock);
  is_static_mode = (g_strcmp0 (overlay->mode, "external") != 0);
  g_mutex_unlock (&overlay->static_lock);

  /* Update external data cache if notification received (external mode only) */
  if (!is_static_mode) {
    g_mutex_lock (&overlay->external_lock);
    if (overlay->data_changed_flag) {
      overlay->data_changed_flag = FALSE;
      if (overlay->external_data != NULL) {
        overlay->cached_external_items = overlay->external_data->get_items ();
        GST_DEBUG_OBJECT (overlay, "External data refreshed: %zu items",
            overlay->cached_external_items.size ());
      }
    }
    external_items = overlay->cached_external_items;
    g_mutex_unlock (&overlay->external_lock);
  }

  /* Draw overlays based on mode */
  if (is_static_mode) {
    g_mutex_lock (&overlay->static_lock);
    if (!overlay->cached_static_image.empty ()) {
      /* Draw the same image at multiple positions for demo */
      gst_syna_overlay_draw_overlay (frame,
          overlay->cached_static_image, overlay->image_x, overlay->image_y,
          format);
    }
    g_mutex_unlock (&overlay->static_lock);
  } else {
    /* Draw cached external overlays */
    for (const auto & item : external_items) {
      gst_syna_overlay_draw_overlay (frame, item.image, item.x, item.y,
          format);
    }
  }

  gst_video_frame_unmap (&vframe);

  return GST_FLOW_OK;
}
