/* GStreamer Synap Overlay Render
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include "gstsynapoverlay.h"
#include "synapmeta.h"
#include <math.h>

#include <cairo.h>
#include <cairo-gobject.h>

#define GST_SYNAP_POSE_SIZE                     (17)
#define GST_SYNAP_POSE_VISIBILITY_THRESHOLD     (0.3)

static gboolean
gst_synap_overlay_render_classification (GstSynapOverlay * synapoverlay,
    GstBuffer * buffer, cairo_format_t format)
{
  cairo_surface_t *surface;
  cairo_t *cr;
  GstVideoFrame frame;

  GST_DEBUG ("SynapOverlay: render classification");
  if (!gst_video_frame_map (&frame, &synapoverlay->videoinfo, buffer,
          GST_MAP_READWRITE)) {
    GST_ERROR ("Frame map error");
    return FALSE;
  }

  surface =
      cairo_image_surface_create_for_data ((unsigned char*)GST_VIDEO_FRAME_PLANE_DATA (&frame,0), format, GST_VIDEO_FRAME_WIDTH (&frame),
                        GST_VIDEO_FRAME_HEIGHT (&frame), GST_VIDEO_FRAME_PLANE_STRIDE (&frame,0));
  if (G_UNLIKELY (!surface)) {
    GST_ERROR ("Cairo surface not created");
    return FALSE;
  }

  cr = cairo_create (surface);
  if (G_UNLIKELY (!cr)) {
    GST_ERROR ("Cairo create failed\n");
    cairo_surface_destroy (surface);
    return FALSE;
  }

  g_mutex_lock (&synapoverlay->data_lock);
  Classifier::Result * result = (Classifier::Result *) synapoverlay->overlaydata;
  g_return_val_if_fail (result != NULL, FALSE);

  if (result->items[0].confidence > synapoverlay->confthreshold) {
    if (synapoverlay->label != NULL) {
      cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size (cr, 35.0);
      cairo_move_to (cr, 10, 35);
      cairo_text_path (cr,
          synapoverlay->labelinfo->label (result->items[0].class_index).c_str ());
      cairo_set_source_rgb (cr, 1, 1, 1);
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 1, 1, 1);
      cairo_set_line_width (cr, .3);
      cairo_stroke (cr);
      cairo_fill_preserve (cr);
    }
  }
  g_mutex_unlock (&synapoverlay->data_lock);
  cairo_destroy (cr);
  cairo_surface_destroy (surface);
  if (frame.buffer) {
    gst_video_frame_unmap (&frame);
  }

  return TRUE;
}

static gboolean
gst_synap_overlay_render_poseline (GstSynapOverlay *synapoverlay, GstVideoFrame *frame,
                                   cairo_t *cr, std::vector<Landmark> &landmarks,
                                   guint start, guint end)
{
  gdouble xs, ys, xe, ye;

  if ((landmarks[start].visibility > GST_SYNAP_POSE_VISIBILITY_THRESHOLD) &&
       (landmarks[end].visibility > GST_SYNAP_POSE_VISIBILITY_THRESHOLD)) {
    xs = landmarks[start].x * GST_VIDEO_FRAME_WIDTH (frame) / synapoverlay->width;
    ys = landmarks[start].y * GST_VIDEO_FRAME_HEIGHT (frame) / synapoverlay->height;

    xe = landmarks[end].x * GST_VIDEO_FRAME_WIDTH (frame) / synapoverlay->width;
    ye = landmarks[end].y * GST_VIDEO_FRAME_HEIGHT (frame) / synapoverlay->height;

    cairo_move_to (cr, xs, ys);
    cairo_line_to (cr, xe, ye);
    cairo_stroke (cr);
  }
  return TRUE;
}

static gboolean
gst_synap_overlay_render_detection (GstSynapOverlay * synapoverlay,
    GstBuffer * buffer, cairo_format_t format)
{
  cairo_surface_t *surface;
  cairo_t *cr;
  GstVideoFrame frame;

  GST_DEBUG ("SynapOverlay: render detection");
  if (!gst_video_frame_map (&frame, &synapoverlay->videoinfo, buffer,
          GST_MAP_READWRITE)) {
    GST_ERROR ("Frame map error");
    return FALSE;
  }

  surface =
      cairo_image_surface_create_for_data ((unsigned char*)GST_VIDEO_FRAME_PLANE_DATA (&frame, 0), format, GST_VIDEO_FRAME_WIDTH (&frame),
                                            GST_VIDEO_FRAME_HEIGHT (&frame), GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0));
  if (G_UNLIKELY (!surface)) {
    GST_ERROR ("Cairo surface not created");
    return FALSE;
  }

  cr = cairo_create (surface);
  if (G_UNLIKELY (!cr)) {
    GST_ERROR ("Cairo create failed\n");
    cairo_surface_destroy (surface);
    return FALSE;
  }

  g_mutex_lock (&synapoverlay->data_lock);
  Detector::Result * result = (Detector::Result *) synapoverlay->overlaydata;
  g_return_val_if_fail (result != NULL, FALSE);

  gdouble x, y, width, height;
  cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size (cr, 20.0);

  size_t points;
  for (size_t i = 0; i < result->items.size (); i++) {
    points = result->items[i].landmarks.size();
    if (points == GST_SYNAP_POSE_SIZE) {
      cairo_set_source_rgb (cr, 1.0, 1.0, 0);
      cairo_set_line_width (cr, 1.5);

      /* Nose - Left eye */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 0, 1);
      /* Left eye - Left ear */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 1, 3);
      /* Left ear - Left shoulder */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 3, 5);
      /* Left shoulder - Left elbow */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 5, 7);
      /* Left elbow - Left wrist */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 7, 9);
      /* Left shoulder - Left hip */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 5, 11);
      /* Left hip - Left Knee */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 11, 13);
      /* Left Knee - Left ankle */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 13, 15);

      /* Nose - Right eye */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 0, 2);
      /* Right eye - Right ear */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 2, 4);
      /* Right ear - Right shoulder */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 4, 6);
      /* Right shoulder - Right elbow */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 6, 8);
      /* Right elbow - Right wrist */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 8, 10);
      /* Right shoulder - Right hip */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 6, 12);
      /* Right hip - Right Knee */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 12, 14);
      /* Right Knee - Right ankle */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 14, 16);

      /* Left shoulder - Right shoulder */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 5, 6);
      /* Left hip - Right hip */
      gst_synap_overlay_render_poseline (synapoverlay, &frame, cr, result->items[i].landmarks, 11, 12);

      cairo_set_source_rgb (cr, 0.0, 1.0, 0);

      /* dot */
      for (size_t j = 0; j < GST_SYNAP_POSE_SIZE; ++j) {
        if (result->items[i].landmarks[j].visibility > GST_SYNAP_POSE_VISIBILITY_THRESHOLD) {
          x = result->items[i].landmarks[j].x * GST_VIDEO_FRAME_WIDTH (&frame) / synapoverlay->width;
          y = result->items[i].landmarks[j].y * GST_VIDEO_FRAME_HEIGHT (&frame) / synapoverlay->height;
          cairo_arc (cr, x, y, 3, 0, 2 * M_PI);
          cairo_fill (cr);
        }
      }
    }
    else {
    x = result->items[i].bounding_box.origin.x *
        GST_VIDEO_FRAME_WIDTH (&frame) / synapoverlay->width;
    y = result->items[i].bounding_box.origin.y *
        GST_VIDEO_FRAME_HEIGHT (&frame) / synapoverlay->height;
    width =
        result->items[i].bounding_box.size.x * GST_VIDEO_FRAME_WIDTH (&frame) / synapoverlay->width;
    height =
        result->items[i].bounding_box.size.y * GST_VIDEO_FRAME_HEIGHT (&frame) /synapoverlay->height;

    /* draw rectangle */
    cairo_rectangle (cr, x, y, width, height);
    cairo_set_source_rgb (cr, 1, 0, 0);
    cairo_set_line_width (cr, 1.5);
    cairo_stroke (cr);
    cairo_fill_preserve (cr);

    if (synapoverlay->label != NULL) {
      cairo_move_to (cr, x + 5, y + 25);
      cairo_text_path (cr,
          synapoverlay->labelinfo->label (result->items[i].class_index).c_str ());
      cairo_set_source_rgb (cr, 1, 0, 0);
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 1, 1, 1);
      cairo_set_line_width (cr, .3);
      cairo_stroke (cr);
      cairo_fill_preserve (cr);
    }
    }
  }
  g_mutex_unlock (&synapoverlay->data_lock);
  cairo_destroy (cr);
  cairo_surface_destroy (surface);
  if (frame.buffer) {
    gst_video_frame_unmap (&frame);
  }

  return TRUE;
}

gboolean
gst_synap_overlay_render (GstBaseTransform * trans, GstBuffer * buffer)
{
  GstSynapOverlay *synapoverlay = GST_SYNAP_OVERLAY (trans);
  GST_DEBUG ("SynapOverlay: render");
  g_return_val_if_fail (buffer != NULL, FALSE);

  gboolean ret = TRUE;
  cairo_format_t format;

  switch (GST_VIDEO_INFO_FORMAT (&synapoverlay->videoinfo)) {
    case GST_VIDEO_FORMAT_ARGB:
    case GST_VIDEO_FORMAT_BGRA:
      format = CAIRO_FORMAT_ARGB32;
      break;
    case GST_VIDEO_FORMAT_xRGB:
    case GST_VIDEO_FORMAT_BGRx:
      format = CAIRO_FORMAT_RGB24;
      break;
    case GST_VIDEO_FORMAT_RGB16:
      format = CAIRO_FORMAT_RGB16_565;
      break;
    default:
    {
      GST_WARNING ("No matching cairo format for %s",
          gst_video_format_to_string (GST_VIDEO_INFO_FORMAT
              (&synapoverlay->videoinfo)));
      return FALSE;
    }
  }

  if (synapoverlay->mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
    ret = gst_synap_overlay_render_classification (synapoverlay, buffer, format);
  } else if (synapoverlay->mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
    ret = gst_synap_overlay_render_detection (synapoverlay, buffer, format);
  }

  return ret;
}
