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

#ifndef _GST_SYNAP_OVERLAY_RENDER_H_
#define _GST_SYNAP_OVERLAY_RENDER_H_

#include <gst/gst.h>
#include <gst/video/video.h>
#include "synap/classifier.hpp"
#include "synap/detector.hpp"
#include "synap/label_info.hpp"


using namespace std;
using namespace synaptics::synap;

gboolean
gst_synap_overlay_render (GstBaseTransform * trans, GstBuffer * buffer);

#endif
