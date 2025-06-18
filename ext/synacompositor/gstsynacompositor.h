/* GStreamer SynaCompositor
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

#ifndef __GST_SYNACOMPOSITOR_H__
#define __GST_SYNACOMPOSITOR_H__

#include "SynaCompositor.h"
#include <gst/base/base.h>
#include <gst/gst.h>
#include <gst/video/gstvideoaggregator.h>
#include <gst/video/video.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglmemory.h>
#include <gst/gstallocator.h>
#include <gst/allocators/allocators.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>

G_BEGIN_DECLS

#define GST_TYPE_SYNACOMPOSITOR (gst_synacompositor_get_type())
G_DECLARE_FINAL_TYPE(GstSynaCompositor, gst_synacompositor, GST, SYNACOMPOSITOR,
                     GstVideoAggregator)

#define GST_TYPE_SYNACOMPOSITOR_PAD (gst_synacompositor_pad_get_type())
G_DECLARE_FINAL_TYPE(GstSynaCompositorPad, gst_synacompositor_pad, GST,
                     SYNACOMPOSITOR_PAD, GstVideoAggregatorPad)

/**
 * GstSynaCompositor:
 *
 * The opaque #GstSynaCompositor structure.
 */
struct _GstSynaCompositor {
  GstVideoAggregator videoaggregator;
  SynaCompositor synaCompositor;
  EGLManager eglManager;
  DRMBuffer outputBuffer;
  gdouble width, height;
  guint background;
  guint numPads;
};

/**
 * GstSynaCompositorPad:
 *
 * The opaque #GstSynaCompositorPad structure.
 */
struct _GstSynaCompositorPad {
  GstVideoAggregatorPad parent;

  /* properties */
  LayoutItem layoutItem;
};

GST_ELEMENT_REGISTER_DECLARE(synacompositor);

G_END_DECLS
#endif /* __GST_SYNACOMPOSITOR_H__ */
