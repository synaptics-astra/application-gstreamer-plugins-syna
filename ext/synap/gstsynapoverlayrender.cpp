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
#include <string.h>
#include <algorithm>
#include <vector>

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

  g_mutex_lock (&synapoverlay->data_lock);
  Classifier::Result * result = (Classifier::Result *) synapoverlay->overlay_data.synapmeta.type.classifier.data;
  g_return_val_if_fail (result != NULL, FALSE);

  // Post-process logic for stable text display
  char new_label[256];
  float new_confidence = result->items[0].confidence;
  strcpy (new_label, synapoverlay->labelinfo->label (result->items[0].class_index).c_str ());
  if (new_confidence >= synapoverlay->overlay_data.synapmeta.type.classifier.confthreshold) {
    // Strong confidence: Update the label and reset persistence timer
    strncpy(synapoverlay->icState.label, new_label, sizeof(synapoverlay->icState.label) - 1);
    synapoverlay->icState.label[sizeof(synapoverlay->icState.label) - 1] = '\0';  // Ensure null termination
    synapoverlay->icState.confidence = new_confidence;
    synapoverlay->icState.persistence_count = synapoverlay->renderctrlPersistenceFrames;
  }
  else if (new_confidence < synapoverlay->renderctrlLowThreshold) {
    // Confidence too low: Reduce persistence count
    if (synapoverlay->icState.persistence_count > 0) {
      synapoverlay->icState.persistence_count--;
    } else {
      // Hide text when timeout expires
      synapoverlay->icState.label[0] = '\0'; // Empty string (no display)
      synapoverlay->icState.confidence = 0.0;
    }
  } else {
    // Between thresholds: Keep last stable state
    if (synapoverlay->icState.persistence_count > 0) {
      synapoverlay->icState.persistence_count--;
    }
  }

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

  cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL,CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size (cr, 20.0);
  cairo_move_to (cr, 10, 35);
  cairo_text_path (cr,synapoverlay->icState.label);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_fill_preserve (cr);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_set_line_width (cr, .3);
  cairo_stroke (cr);
  cairo_fill_preserve (cr);

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
    xs = landmarks[start].x * GST_VIDEO_FRAME_WIDTH (frame) / synapoverlay->overlay_data.synapmeta.type.detector.width;
    ys = landmarks[start].y * GST_VIDEO_FRAME_HEIGHT (frame) / synapoverlay->overlay_data.synapmeta.type.detector.height;

    xe = landmarks[end].x * GST_VIDEO_FRAME_WIDTH (frame) / synapoverlay->overlay_data.synapmeta.type.detector.width;
    ye = landmarks[end].y * GST_VIDEO_FRAME_HEIGHT (frame) / synapoverlay->overlay_data.synapmeta.type.detector.height;

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
  Detector::Result * result = (Detector::Result *) synapoverlay->overlay_data.synapmeta.type.detector.data;
  g_return_val_if_fail (result != NULL, FALSE);

  /* Compute & smooth velocity, then predict to display PTS */
  const GstClockTime t  = GST_BUFFER_PTS (buffer);
  const GstClockTime t0 = synapoverlay->prev_det_pts;
  const GstClockTime t1 = synapoverlay->curr_det_pts;

  Detector::Result predicted = *result; /* start from latest detection (t1) */

  if (synapoverlay->predictive &&
      synapoverlay->prev_det_copy &&
      t0 != GST_CLOCK_TIME_NONE && t1 != GST_CLOCK_TIME_NONE && t1 > t0)
  {
    const double dt01 = (double)(t1 - t0) / 1e9;   /* seconds prev->curr detect */
    double       dt1t = (double)(t  - t1) / 1e9;   /* seconds curr detect->draw */
    if (synapoverlay->extrapolate_max_ms > 0) {
      const double cap = synapoverlay->extrapolate_max_ms / 1000.0;
      if (dt1t > cap) dt1t = cap;
    }

    /* Greedy IoU matching: prev_det_copy (t0) → predicted(items at t1) */
    const auto &prev = *synapoverlay->prev_det_copy;
    const auto &curr = predicted;

    auto iou = [](const Detector::Result::Item &A,
                  const Detector::Result::Item &B) -> double {
      const double ax0=A.bounding_box.origin.x, ay0=A.bounding_box.origin.y;
      const double ax1=ax0+A.bounding_box.size.x, ay1=ay0+A.bounding_box.size.y;
      const double bx0=B.bounding_box.origin.x, by0=B.bounding_box.origin.y;
      const double bx1=bx0+B.bounding_box.size.x, by1=by0+B.bounding_box.size.y;
      const double ix0=std::max(ax0,bx0), iy0=std::max(ay0,by0);
      const double ix1=std::min(ax1,bx1), iy1=std::min(ay1,by1);
      const double iw = std::max(0.0, ix1-ix0), ih = std::max(0.0, iy1-iy0);
      const double inter = iw*ih;
      const double aarea = (ax1-ax0)*(ay1-ay0);
      const double barea = (bx1-bx0)*(by1-by0);
      const double uni = aarea + barea - inter;
      return uni > 0.0 ? inter / uni : 0.0;
    };

    const double IOU_T = 0.20;
    std::vector<int> prev_used(prev.items.size(), 0);

    /* Ensure velocity state matches current items */
    if (synapoverlay->vel_state.size() != curr.items.size())
      synapoverlay->vel_state.assign(curr.items.size(), {0,0,0,0,-1});

    for (size_t i = 0; i < curr.items.size(); ++i) {
      /* Find best match in previous frame by IoU */
      int best = -1; double best_iou = IOU_T;
      for (size_t j = 0; j < prev.items.size(); ++j) {
        if (prev_used[j]) continue;
        const double v = iou(curr.items[i], prev.items[j]);
        if (v > best_iou) { best_iou = v; best = (int)j; }
      }

      /* Measure instantaneous velocity (prev→curr) */
      double vx=0.0, vy=0.0, vw=0.0, vh=0.0;
      const int cls = curr.items[i].class_index;
      if (best >= 0) {
        prev_used[best] = 1;
        const auto &B0 = prev.items[best].bounding_box;
        const auto &B1 = curr.items[i].bounding_box;
        const double c0x = B0.origin.x + 0.5 * B0.size.x;
        const double c0y = B0.origin.y + 0.5 * B0.size.y;
        const double c1x = B1.origin.x + 0.5 * B1.size.x;
        const double c1y = B1.origin.y + 0.5 * B1.size.y;
        vx = (c1x - c0x) / dt01;  vy = (c1y - c0y) / dt01;
        vw = (B1.size.x - B0.size.x) / dt01;
        vh = (B1.size.y - B0.size.y) / dt01;
      }

      /* EMA low-pass on velocity (no-lag smoothing) */
      auto &S = synapoverlay->vel_state[i];
      if (synapoverlay->smooth_velocity) {
        const double a = synapoverlay->vel_alpha; /* 0..1 */
        if (S.cls != cls) {
          S = {vx,vy,vw,vh, cls};
        } else {
          S.vx = (1.0-a)*S.vx + a*vx;
          S.vy = (1.0-a)*S.vy + a*vy;
          S.vw = (1.0-a)*S.vw + a*vw;
          S.vh = (1.0-a)*S.vh + a*vh;
        }
      } else {
        S = {vx,vy,vw,vh, cls};
      }

      /* Predict forward from current detection using *smoothed* velocity */
      auto &B1 = predicted.items[i].bounding_box;
      const double c1x = B1.origin.x + 0.5 * B1.size.x;
      const double c1y = B1.origin.y + 0.5 * B1.size.y;

      double cx = c1x + S.vx * dt1t;
      double cy = c1y + S.vy * dt1t;
      double ww = B1.size.x + S.vw * dt1t;
      double hh = B1.size.y + S.vh * dt1t;

      /* Optional small residual pull toward the latest detection */
      if (synapoverlay->ema_alpha > 0.0 && dt1t >= 0.0) {
        const double a = synapoverlay->ema_alpha;
        cx = (1.0 - a) * c1x + a * cx;
        cy = (1.0 - a) * c1y + a * cy;
        ww = (1.0 - a) * B1.size.x + a * ww;
        hh = (1.0 - a) * B1.size.y + a * hh;
      }

      B1.origin.x = cx - 0.5 * ww;
      B1.origin.y = cy - 0.5 * hh;
      B1.size.x   = ww;
      B1.size.y   = hh;
    }
  }

  /* If you still want a tiny post-predict visual low-pass, keep smooth_draw on.
     Otherwise leave it off to avoid any lag. */
  Detector::Result to_draw = predicted;
  if (synapoverlay->smooth_draw && synapoverlay->last_draw_copy) {
    const double a = synapoverlay->draw_ema_alpha; /* 0..1 */
    const double IOU_THRESH = 0.2;

    auto iou = [](const Detector::Result::Item &A,
                  const Detector::Result::Item &B)->double {
      const double ax0=A.bounding_box.origin.x, ay0=A.bounding_box.origin.y;
      const double ax1=ax0+A.bounding_box.size.x, ay1=ay0+A.bounding_box.size.y;
      const double bx0=B.bounding_box.origin.x, by0=B.bounding_box.origin.y;
      const double bx1=bx0+B.bounding_box.size.x, by1=by0+B.bounding_box.size.y;
      const double ix0=std::max(ax0,bx0), iy0=std::max(ay0,by0);
      const double ix1=std::min(ax1,bx1), iy1=std::min(ay1,by1);
      const double iw=std::max(0.0, ix1-ix0), ih=std::max(0.0, iy1-iy0);
      const double inter=iw*ih;
      const double aarea=(ax1-ax0)*(ay1-ay0), barea=(bx1-bx0)*(by1-by0);
      const double uni=aarea+barea-inter; return uni>0.0? inter/uni : 0.0;
    };

    const auto &prevDraw = *synapoverlay->last_draw_copy;
    std::vector<int> used(prevDraw.items.size(), 0);

    for (size_t i = 0; i < to_draw.items.size(); ++i) {
      int best = -1; double best_iou = IOU_THRESH;
      for (size_t j = 0; j < prevDraw.items.size(); ++j) {
        if (used[j]) continue;
        double v = iou(to_draw.items[i], prevDraw.items[j]);
        if (v > best_iou) { best_iou = v; best = (int)j; }
      }
      if (best >= 0) {
        used[best] = 1;
        auto &D = to_draw.items[i].bounding_box;
        const auto &P = prevDraw.items[best].bounding_box;
        D.origin.x = (1.0 - a) * P.origin.x + a * D.origin.x;
        D.origin.y = (1.0 - a) * P.origin.y + a * D.origin.y;
        D.size.x   = (1.0 - a) * P.size.x   + a * D.size.x;
        D.size.y   = (1.0 - a) * P.size.y   + a * D.size.y;
      }
    }
  }

  /* draw from (predicted and optionally smoothed) result */
  result = &to_draw;

  /* Drawing */
  cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size (cr, 20.0);

  for (size_t i = 0; i < result->items.size (); i++) {
    size_t points = result->items[i].landmarks.size();
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
          gdouble x = result->items[i].landmarks[j].x * GST_VIDEO_FRAME_WIDTH (&frame)  / synapoverlay->overlay_data.synapmeta.type.detector.width;
          gdouble y = result->items[i].landmarks[j].y * GST_VIDEO_FRAME_HEIGHT (&frame) / synapoverlay->overlay_data.synapmeta.type.detector.height;
          cairo_arc (cr, x, y, 3, 0, 2 * M_PI);
          cairo_fill (cr);
        }
      }
    } else {
      gdouble x = result->items[i].bounding_box.origin.x *
                  GST_VIDEO_FRAME_WIDTH (&frame)  / synapoverlay->overlay_data.synapmeta.type.detector.width;
      gdouble y = result->items[i].bounding_box.origin.y *
                  GST_VIDEO_FRAME_HEIGHT (&frame) / synapoverlay->overlay_data.synapmeta.type.detector.height;
      gdouble width  = result->items[i].bounding_box.size.x *
                       GST_VIDEO_FRAME_WIDTH (&frame)  / synapoverlay->overlay_data.synapmeta.type.detector.width;
      gdouble height = result->items[i].bounding_box.size.y *
                       GST_VIDEO_FRAME_HEIGHT (&frame) / synapoverlay->overlay_data.synapmeta.type.detector.height;

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

  /* keep what we actually drew (only used if smooth_draw=true) */
  if (synapoverlay->smooth_draw) {
    if (synapoverlay->last_draw_copy) { delete synapoverlay->last_draw_copy; synapoverlay->last_draw_copy = NULL; }
    synapoverlay->last_draw_copy = new Detector::Result (*result);
    synapoverlay->last_draw_pts = t;
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

  if (synapoverlay->overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_CLASSIFIER) {
    ret = gst_synap_overlay_render_classification (synapoverlay, buffer, format);
  } else if (synapoverlay->overlay_data.synapmeta.mode == GST_SYNAP_POSTPROCESSING_MODE_DETECTOR) {
    ret = gst_synap_overlay_render_detection (synapoverlay, buffer, format);
  }

  return ret;
}
