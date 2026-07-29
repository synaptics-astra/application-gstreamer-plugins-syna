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

#ifndef __GST_SYNA_OVERLAY_H__
#define __GST_SYNA_OVERLAY_H__

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include <glib.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <vector>
#include <memory>

G_BEGIN_DECLS

#define GST_TYPE_SYNA_OVERLAY            (gst_syna_overlay_get_type ())
#define GST_SYNA_OVERLAY(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_SYNA_OVERLAY, GstSynaOverlay))
#define GST_SYNA_OVERLAY_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_SYNA_OVERLAY, GstSynaOverlayClass))
#define GST_IS_SYNA_OVERLAY(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_SYNA_OVERLAY))
#define GST_IS_SYNA_OVERLAY_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_SYNA_OVERLAY))
#define GST_SYNA_OVERLAY_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_SYNA_OVERLAY, GstSynaOverlayClass))

typedef struct _GstSynaOverlay GstSynaOverlay;
typedef struct _GstSynaOverlayClass GstSynaOverlayClass;

/**
 * OverlayItem: Single overlay image with position
 */
struct OverlayItem {
  cv::Mat image;  /* OpenCV image (BGR or BGRA) */
  gint x;         /* X coordinate */
  gint y;         /* Y coordinate */
};

/**
 * ExternalOverlayData: Thread-safe container for external overlay data
 */
struct ExternalOverlayData {
  std::vector<OverlayItem> items;

  void clear() {
    items.clear();
  }

  void add_item(const cv::Mat& image, gint x, gint y) {
    items.push_back({image.clone(), x, y});
  }

  std::vector<OverlayItem> get_items() {
    return items;
  }
};

struct _GstSynaOverlay {
  GstBaseTransform element;

  /* Video info */
  GstVideoInfo videoinfo;

  /* Properties for static mode */
  gchar *mode;              /* "static" or "external" */
  gchar *image_path;        /* Path to image file (static mode) */
  gint image_x;             /* X position (static mode) */
  gint image_y;             /* Y position (static mode) */

  /* Static mode caching */
  cv::Mat cached_static_image;
  gboolean static_image_loaded;

  /* External data management */
  ExternalOverlayData *external_data;
  std::vector<OverlayItem> cached_external_items;  /* Persisted cache across frames */

  /* Temporary pointer storage for add-pointer command */
  guint8 *pending_image_data;
  guint64 pending_image_size;

  /* Thread safety */
  GMutex static_lock;
  GMutex external_lock;
  gboolean data_changed_flag;
};

struct _GstSynaOverlayClass {
  GstBaseTransformClass parent_class;
};

GType gst_syna_overlay_get_type (void);

/* Forward declaration for plugin registration */
gboolean gst_element_register_synaoverlay (GstPlugin * plugin);

G_END_DECLS

#endif /* __GST_SYNA_OVERLAY_H__ */
