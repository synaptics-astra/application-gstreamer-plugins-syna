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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstsynacompositor
 *
 * The synacompositor element does OpenGL based composition for video frames.
 *
 * <refsect2>
 * <title>Example launch line for synacompositor </title>
 * |[
 *    gst-launch-1.0 synacompositor name=comp \
      sink_0::xpos=0 sink_0::ypos=0 sink_0::height=540 sink_0::width=960 \
      sink_1::xpos=0 sink_1::ypos=540 sink_1::height=540 sink_1::width=960 \
      sink_2::xpos=960 sink_2::ypos=0 sink_2::height=540 sink_2::width=960  \
      sink_3::xpos=960 sink_3::ypos=540 sink_3::height=540 sink_3::width=960 \
      ! waylandsink sync=false \
      filesrc location=video1.h264 ! h264parse ! v4l2h264dec ! comp.sink_0 \
      filesrc location=video2.h2644 ! h264parse ! v4l2h264dec ! comp.sink_1 \
      filesrc location=video3.h264 ! h264parse ! v4l2h264dec ! comp.sink_2 \
      filesrc location=video4.h264 ! h264parse ! v4l2h264dec ! comp.sink_3
 * ]|
 * Sample pipeline to do NV12 to BGRA conversion
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include "gstsynacompositor.h"

#ifdef DISABLE_ORC
#define orc_memset memset
#else
#include <orc/orcfunctions.h>
#endif

static void gst_synacompositor_child_proxy_init(gpointer g_iface,
                                                gpointer iface_data);

GST_DEBUG_CATEGORY_STATIC(gst_synacompositor_debug);
#define GST_CAT_DEFAULT gst_synacompositor_debug
G_DEFINE_TYPE_WITH_CODE(
    GstSynaCompositor, gst_synacompositor, GST_TYPE_VIDEO_AGGREGATOR,
    G_IMPLEMENT_INTERFACE(GST_TYPE_CHILD_PROXY,
                          gst_synacompositor_child_proxy_init));
GST_ELEMENT_REGISTER_DEFINE(synacompositor, "synacompositor",
                            GST_RANK_PRIMARY + 1, GST_TYPE_SYNACOMPOSITOR);
G_DEFINE_TYPE(GstSynaCompositorPad, gst_synacompositor_pad,
              GST_TYPE_VIDEO_AGGREGATOR_PAD);

#define SRC_COLOR_FORMATS "{ BGRA }"
#define SINK_COLOR_FORMAT "{ NV12, NV12M }"
#define DEFAULT_PAD_XPOS 0.0
#define DEFAULT_PAD_YPOS 0.0
#define DEFAULT_PAD_ZORDER 0.0
#define DEFAULT_PAD_WIDTH 0.0
#define DEFAULT_PAD_HEIGHT 0.0
#define DEFAULT_PAD_ALPHA 1.0

enum {
  PROP_PAD_0,
  PROP_PAD_XPOS,
  PROP_PAD_YPOS,
  PROP_PAD_ZORDER,
  PROP_PAD_WIDTH,
  PROP_PAD_HEIGHT,
  PROP_PAD_ALPHA,
};

enum {
  PROP_0,
  PROP_BACKGROUND,
  PROP_COMP_WIDTH,
  PROP_COMP_HEIGHT,
};

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:DMABuf), "
                    "format=(string)" SRC_COLOR_FORMATS ", "
                    "width=(int)[1,2147483647], height=(int)[1,2147483647], "
                    "framerate=(fraction)[0/1,2147483647/1]; "

                    "video/x-raw, "
                    "format=(string)" SRC_COLOR_FORMATS ", "
                    "width=(int)[1,2147483647], height=(int)[1,2147483647], "
                    "framerate=(fraction)[0/1,2147483647/1]"));

static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE(
    "sink_%u", GST_PAD_SINK, GST_PAD_REQUEST,
    GST_STATIC_CAPS("video/x-raw(memory:DMABuf), "
                    "format=(string)" SINK_COLOR_FORMAT ", "
                    "width=(int)[1,2147483647], height=(int)[1,2147483647], "
                    "framerate=(fraction)[0/1, 2147483647/1]; "

                    "video/x-raw, "
                    "format=(string)" SINK_COLOR_FORMAT ", "
                    "width=(int)[1,2147483647], height=(int)[1,2147483647], "
                    "framerate=(fraction)[0/1, 2147483647/1]"));

/* optimized memcpy using ARM NEON */
void memcpy_neon(void *dst, const void *src, size_t len) {
  asm volatile("1:\n\t"
               "MOV x0,%0\n\t"
               "MOV x1,%1\n\t"
               "2:\n\t"
               "LDP q0,q1,[x0],#0x20\n\t"
               "STP q0,q1,[x1],#0x20\n\t"
               "SUBS %2,%2,#0x20\n\t"
               "BGT 2b\n\t"
               : "=&r"(src), "=&r"(dst), "=&r"(len)
               : "0"(src), "1"(dst), "2"(len)
               : "q0", "q1", "x0", "x1");
}

static void gst_synacompositor_pad_get_property(GObject *object, guint prop_id,
                                                GValue *value,
                                                GParamSpec *pspec) {
  GstSynaCompositorPad *pad = GST_SYNACOMPOSITOR_PAD(object);
  switch (prop_id) {
    case PROP_PAD_XPOS:
      g_value_set_double(value, pad->layoutItem.xPos);
      break;
    case PROP_PAD_YPOS:
      g_value_set_double(value, pad->layoutItem.yPos);
      break;
    case PROP_PAD_ZORDER:
      g_value_set_double(value, pad->layoutItem.zPos);
      break;
    case PROP_PAD_WIDTH:
      g_value_set_double(value, pad->layoutItem.width);
      break;
    case PROP_PAD_HEIGHT:
      g_value_set_double(value, pad->layoutItem.height);
      break;
    case PROP_PAD_ALPHA:
      g_value_set_double(value, pad->layoutItem.alpha);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_synacompositor_pad_set_property(GObject *object, guint prop_id,
                                                const GValue *value,
                                                GParamSpec *pspec) {
  GstSynaCompositorPad *pad = GST_SYNACOMPOSITOR_PAD(object);
  switch (prop_id) {
    case PROP_PAD_XPOS:
      pad->layoutItem.xPos = g_value_get_double(value);
      break;
    case PROP_PAD_YPOS:
      pad->layoutItem.yPos = g_value_get_double(value);
      break;
    case PROP_PAD_ZORDER:
      pad->layoutItem.zPos = g_value_get_double(value);
      break;
    case PROP_PAD_WIDTH:
      pad->layoutItem.width = g_value_get_double(value);
      break;
    case PROP_PAD_HEIGHT:
      pad->layoutItem.height = g_value_get_double(value);
      break;
    case PROP_PAD_ALPHA:
      pad->layoutItem.alpha = g_value_get_double(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void
gst_synacompositor_pad_class_init(GstSynaCompositorPadClass *klass) {
  GObjectClass *gobject_class = (GObjectClass *)klass;

  gobject_class->set_property = gst_synacompositor_pad_set_property;
  gobject_class->get_property = gst_synacompositor_pad_get_property;

  g_object_class_install_property(
      gobject_class, PROP_PAD_XPOS,
      g_param_spec_double(
          "xpos", "X Position", "X Position of the picture", 0.0, 3840.0, 0.0,
          (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
                        G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_PAD_YPOS,
      g_param_spec_double(
          "ypos", "Y Position", "Y Position of the picture", 0.0, 2160.0, 0.0,
          (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
                        G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_PAD_ZORDER,
      g_param_spec_double(
          "zorder", "Z Order", "Z Order of the picture", -1.0, 1.0, 0.0,
          (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
                        G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_PAD_WIDTH,
      g_param_spec_double(
          "width", "Width", "Width of the picture", 0.0, 3840.0, 0.0,
          (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
                        G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_PAD_HEIGHT,
      g_param_spec_double(
          "height", "Height", "Height of the picture", 0.0, 2160.0, 0.0,
          (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
                        G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_PAD_ALPHA,
      g_param_spec_double(
          "alpha", "Alpha", "Alpha of the picture", 0.0, 1.0, DEFAULT_PAD_ALPHA,
          (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
                        G_PARAM_STATIC_STRINGS)));
}

static void gst_synacompositor_pad_init(GstSynaCompositorPad *compo_pad) {
  static int padIndex = 0;
  compo_pad->layoutItem.index = padIndex;
  compo_pad->layoutItem.xPos = DEFAULT_PAD_XPOS;
  compo_pad->layoutItem.yPos = DEFAULT_PAD_YPOS;
  compo_pad->layoutItem.zPos = DEFAULT_PAD_ZORDER;
  compo_pad->layoutItem.width = DEFAULT_PAD_WIDTH;
  compo_pad->layoutItem.height = DEFAULT_PAD_HEIGHT;
  compo_pad->layoutItem.alpha = DEFAULT_PAD_ALPHA;
  padIndex++;
}

static void gst_synacompositor_get_property(GObject *object, guint prop_id,
                                            GValue *value, GParamSpec *pspec) {
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(object);
  switch (prop_id) {
    case PROP_BACKGROUND:
      g_value_set_uint(value, self->background);
      break;
    case PROP_COMP_WIDTH:
      g_value_set_double(value, self->width);
      break;
    case PROP_COMP_HEIGHT:
      g_value_set_double(value, self->height);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_synacompositor_set_property(GObject *object, guint prop_id,
                                            const GValue *value,
                                            GParamSpec *pspec) {
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(object);
  switch (prop_id) {
    case PROP_BACKGROUND:
      self->background = g_value_get_uint(value);
      break;
    case PROP_COMP_WIDTH:
      self->width = g_value_get_double(value);
      break;
    case PROP_COMP_HEIGHT:
      self->height = g_value_get_double(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static GstCaps *gst_synacompositor_fixate_caps(GstAggregator *agg,
                                               GstCaps *caps) {
  GstVideoAggregator *vagg = GST_VIDEO_AGGREGATOR(agg);
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(agg);
  GList *l;
  gint best_width = -1, best_height = -1;
  gint best_fps_n = -1, best_fps_d = -1;
  gint par_n, par_d;
  gdouble best_fps = 0.;
  GstCaps *ret = NULL;
  GstStructure *s;

  ret = gst_caps_make_writable(caps);

  /* we need this to calculate how large to make the output frame */
  s = gst_caps_get_structure(ret, 0);
  if (gst_structure_has_field(s, "pixel-aspect-ratio")) {
    gst_structure_fixate_field_nearest_fraction(s, "pixel-aspect-ratio", 1, 1);
    gst_structure_get_fraction(s, "pixel-aspect-ratio", &par_n, &par_d);
  } else {
    par_n = par_d = 1;
  }

  best_width = self->width;
  best_height = self->height;

  GST_OBJECT_LOCK(vagg);
  for (l = GST_ELEMENT(vagg)->sinkpads; l; l = l->next) {
    GstVideoAggregatorPad *vaggpad = (GstVideoAggregatorPad *)l->data;
    GstSynaCompositorPad *synacompositor_pad = GST_SYNACOMPOSITOR_PAD(vaggpad);
    gint this_width, this_height;
    gint width, height;
    gint fps_n, fps_d;
    gdouble cur_fps;
    gint x_offset;
    gint y_offset;

    if (gst_aggregator_pad_is_inactive(GST_AGGREGATOR_PAD(vaggpad)))
      continue;

    fps_n = GST_VIDEO_INFO_FPS_N(&vaggpad->info);
    fps_d = GST_VIDEO_INFO_FPS_D(&vaggpad->info);

    if (fps_d == 0)
      cur_fps = 0.0;
    else
      gst_util_fraction_to_double(fps_n, fps_d, &cur_fps);

    if (best_fps < cur_fps) {
      best_fps = cur_fps;
      best_fps_n = fps_n;
      best_fps_d = fps_d;
    }
  }
  GST_OBJECT_UNLOCK(vagg);

  if (best_fps_n <= 0 || best_fps_d <= 0 || best_fps == 0.0) {
    best_fps_n = 25;
    best_fps_d = 1;
    best_fps = 25.0;
  }

  gst_structure_fixate_field_nearest_int(s, "width", 1920);
  gst_structure_fixate_field_nearest_int(s, "height", 1080);
  gst_structure_fixate_field_nearest_fraction(s, "framerate", best_fps_n,
                                              best_fps_d);
  ret = gst_caps_fixate(ret);

  return ret;
}

static gboolean gst_synacompositor_start(GstAggregator *agg) {
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(agg);
  GList *l;
  int layoutIndex = 0;

  self->eglManager.setPixelFormat(ARGB);
  self->eglManager.initializeEGLDisplay("/dev/dri/card0");
  self->synaCompositor.setScreenDimensions(self->width, self->height);

  /* Register Composition Layout */
  for (l = GST_ELEMENT(self)->sinkpads; l; l = l->next) {
    GstSynaCompositorPad *pad = (GstSynaCompositorPad *)l->data;
    pad->layoutItem.index = layoutIndex;
    self->synaCompositor.registerCompositorLayoutItems({pad->layoutItem});
    layoutIndex++;
  }

  /* Initialize Compositor */
  glViewport(0, 0, self->width, self->height);
  self->synaCompositor.initializeCompositor();
  self->outputBuffer.createBuffer(self->eglManager.getGBMDevice(), self->width,
                                  self->height, GBM_FORMAT_ARGB8888);

  /* Free EGL Context from thread */
  if (!eglMakeCurrent(self->eglManager.getDisplay(), EGL_NO_SURFACE,
                      EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
    EGLint error = eglGetError();
    GST_ERROR_OBJECT(self, "eglMakeCurrent failed! EGL error: 0x%04x\n", error);
  }

  return GST_AGGREGATOR_CLASS(gst_synacompositor_parent_class)->start(agg);
}

static gboolean gst_synacompositor_stop(GstAggregator *agg) {
  /* TBD */
  return GST_AGGREGATOR_CLASS(gst_synacompositor_parent_class)->stop(agg);
}

static GstPad *gst_synacompositor_request_new_pad(GstElement *element,
                                                  GstPadTemplate *templ,
                                                  const gchar *req_name,
                                                  const GstCaps *caps) {
  GstPad *newpad;

  newpad = (GstPad *)GST_ELEMENT_CLASS(gst_synacompositor_parent_class)
               ->request_new_pad(element, templ, req_name, caps);

  if (newpad == NULL)
    goto could_not_create;

  gst_child_proxy_child_added(GST_CHILD_PROXY(element), G_OBJECT(newpad),
                              GST_OBJECT_NAME(newpad));

  return newpad;

could_not_create : {
  GST_DEBUG_OBJECT(element, "could not create/add pad");
  return NULL;
}
}

static void gst_synacompositor_release_pad(GstElement *element, GstPad *pad) {
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(element);

  GST_DEBUG_OBJECT(self, "release pad %s:%s", GST_DEBUG_PAD_NAME(pad));

  gst_child_proxy_child_removed(GST_CHILD_PROXY(self), G_OBJECT(pad),
                                GST_OBJECT_NAME(pad));

  GST_ELEMENT_CLASS(gst_synacompositor_parent_class)->release_pad(element, pad);
}

static gboolean gst_synacompositor_sink_query(GstAggregator *agg,
                                              GstAggregatorPad *bpad,
                                              GstQuery *query) {
  GstVideoAggregator *vagg = GST_VIDEO_AGGREGATOR(agg);
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(agg);
  GstVideoAggregatorPad *pad = GST_VIDEO_AGGREGATOR_PAD(bpad);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE(query)) {
    case GST_QUERY_CAPS: {
      GstCaps *filter = NULL;
      GstCaps *caps = NULL;
      GstCaps *pad_template_caps = gst_pad_get_pad_template_caps(GST_PAD(pad));

      gst_query_parse_caps(query, &filter);

      if (filter) {
        GstCaps *intersect = gst_caps_intersect(filter, pad_template_caps);
        if (!intersect) {
          GST_ERROR_OBJECT(self, "Error: Intersection of caps failed!\n");
          break;
        }

        if (gst_caps_can_intersect(
                intersect, gst_caps_from_string(
                              "video/x-raw(memory:DMABuf), width=(int)[1,1920], "
                              "height=(int)[1,1080]"))) {
          caps = gst_caps_intersect(
              intersect,
              gst_caps_from_string("video/x-raw(memory:DMABuf), "
                                  "width=(int)[1,1920], height=(int)[1,1080]"));
        } else {
          caps = gst_caps_ref(intersect);
        }

        gst_caps_unref(intersect);
      } else {
        GST_ERROR_OBJECT(self, "No filter specified, using pad template caps.\n");
        caps = gst_caps_ref(pad_template_caps);
      }

      if (!caps || !GST_IS_CAPS(caps)) {
        GST_ERROR_OBJECT(self, "Error: Failed to obtain valid caps.\n");
        break;
      }

      gst_query_set_caps_result(query, caps);

      gst_caps_unref(pad_template_caps);
      gst_caps_unref(caps);

      ret = TRUE;
      break;
    }

    case GST_QUERY_ACCEPT_CAPS: {
      GstCaps *caps = NULL;
      gst_query_parse_accept_caps(query, &caps);

      if (!caps) {
        GST_ERROR_OBJECT(self, "Error: No caps found in ACCEPT_CAPS query.\n");
        break;
      }

      GstCaps *pad_template_caps = gst_pad_get_pad_template_caps(GST_PAD(pad));
      gboolean is_accepted = gst_caps_can_intersect(pad_template_caps, caps);

      gst_query_set_accept_caps_result(query, is_accepted);
      gst_caps_unref(pad_template_caps);
      gst_caps_unref(caps);

      ret = TRUE;
      break;
    }

    default:
      ret = GST_AGGREGATOR_CLASS(gst_synacompositor_parent_class)
                ->sink_query(agg, bpad, query);
      break;
    }

  return ret;
}

static void gst_synacompositor_finalize(GObject *object) { /* TBD */
}

static GstFlowReturn
gst_synacompositor_aggregate_frames(GstVideoAggregator *vagg,
                                    GstBuffer *outbuf) {
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(vagg);
  GList *l;
  guint i, n_pads = 0;
  EGLImageKHR outputImage;
  guint renderingEnable = 0;
  int index = 0;
  EGLImageKHR inputImage;

  if (eglGetCurrentContext() == EGL_NO_CONTEXT) {
    if (!eglMakeCurrent(self->eglManager.getDisplay(), EGL_NO_SURFACE,
                        EGL_NO_SURFACE, self->eglManager.getContext())) {
      EGLint error = eglGetError();
      GST_ERROR_OBJECT(self, "eglMakeCurrent failed! EGL error: 0x%04x\n",
                       error);
    }
  }

  GST_OBJECT_LOCK(vagg);

  for (l = GST_ELEMENT(vagg)->sinkpads; l; l = l->next) {
    GstVideoAggregatorPad *pad = (GstVideoAggregatorPad *)l->data;
    GstVideoFrame *prepared_frame =
        gst_video_aggregator_pad_get_prepared_frame(pad);
    if (prepared_frame) {
      GstVideoMeta *vmeta = gst_buffer_get_video_meta(prepared_frame->buffer);
      GstMemory *mem = gst_buffer_peek_memory(prepared_frame->buffer, 0);
      if (gst_is_dmabuf_memory(mem)) {
        int fd1 = gst_dmabuf_memory_get_fd(mem);
        mem = gst_buffer_peek_memory(prepared_frame->buffer, 1);
        int fd2 = gst_dmabuf_memory_get_fd(mem);
        self->eglManager.createImageKHR(fd1, fd2, inputImage, vmeta->width,
                                        vmeta->height, NV12);
        self->synaCompositor.setTextureImage(index, inputImage);
        renderingEnable = 1;
      } else
        GST_DEBUG_OBJECT(self, "Buffer Format is not supported.");
    }
    index++;
  }

  if (renderingEnable) {
    guint n_mem = gst_buffer_n_memory(outbuf);
    GstMemory *mem = gst_buffer_peek_memory(outbuf, 0);
    if (n_mem > 0) {
      if (gst_is_dmabuf_memory(mem)) {
        int fd = gst_dmabuf_memory_get_fd(mem);
        self->eglManager.createImageKHR(fd, -1, outputImage, 1920, 1080, ARGB);
        self->synaCompositor.setOutputTextureImage(outputImage);
        self->synaCompositor.startRendering();
      } else {
        self->eglManager.createImageKHR(self->outputBuffer.getSharedFD(), -1,
                                        outputImage, 1920, 1080, ARGB);
        self->synaCompositor.setOutputTextureImage(outputImage);
        self->synaCompositor.startRendering();

        GstMapInfo map;
        if (!gst_buffer_map(outbuf, &map, GST_MAP_WRITE)) {
          GST_ERROR_OBJECT(self, "Failed to map output buffer");
        }

        uint8_t *base = map.data;
        memcpy_neon((void *)base, self->outputBuffer.getVirtualAddress(),
                    map.size);
        gst_buffer_unmap(outbuf, &map);
      }
    }
  }

  GST_OBJECT_UNLOCK(vagg);

  return GST_FLOW_OK;
}

/* GObject boilerplate */
static void gst_synacompositor_class_init(GstSynaCompositorClass *klass) {
  GObjectClass *gobject_class = (GObjectClass *)klass;
  GstElementClass *gstelement_class = (GstElementClass *)klass;
  GstVideoAggregatorClass *videoaggregator_class =
      (GstVideoAggregatorClass *)klass;
  GstAggregatorClass *agg_class = (GstAggregatorClass *)klass;

  gobject_class->get_property = gst_synacompositor_get_property;
  gobject_class->set_property = gst_synacompositor_set_property;
  gobject_class->finalize = gst_synacompositor_finalize;

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR(gst_synacompositor_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR(gst_synacompositor_release_pad);
  agg_class->sink_query = gst_synacompositor_sink_query;
  agg_class->fixate_src_caps = gst_synacompositor_fixate_caps;
  agg_class->start = GST_DEBUG_FUNCPTR(gst_synacompositor_start);
  agg_class->stop = GST_DEBUG_FUNCPTR(gst_synacompositor_stop);
  videoaggregator_class->aggregate_frames = gst_synacompositor_aggregate_frames;

  gst_element_class_add_static_pad_template_with_gtype(
      gstelement_class, &src_factory, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype(
      gstelement_class, &sink_factory, GST_TYPE_SYNACOMPOSITOR_PAD);

  gst_element_class_set_static_metadata(
      gstelement_class, "Syna Compositor", "Filter/Editor/Video/SynaCompositor",
      "Composite multiple video streams", "Synaptics Inc");

  g_object_class_install_property(
      gobject_class, PROP_BACKGROUND,
      g_param_spec_uint("background", "Background",
                        "Background Color of Compositor (in ARGB hex value)",
                        0x00000000, 0xFFFFFFFF, 0x00000000,
                        (GParamFlags)(G_PARAM_READWRITE |
                                      GST_PARAM_CONTROLLABLE |
                                      G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_COMP_WIDTH,
      g_param_spec_double(
          "width", "Width", "Width of the Compositor", 0.0, 3840.0, 0.0,
          (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
                        G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property(
      gobject_class, PROP_COMP_HEIGHT,
      g_param_spec_double(
          "height", "Height", "Height of the Compositor", 0.0, 2160.0, 0.0,
          (GParamFlags)(G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE |
                        G_PARAM_STATIC_STRINGS)));

  gst_type_mark_as_plugin_api(GST_TYPE_SYNACOMPOSITOR_PAD,
                              (GstPluginAPIFlags)0);
}

static void gst_synacompositor_init(GstSynaCompositor *self) {
  /* initialize variables to default values */
  self->background = 0x00000000;
  self->height = 1080.0;
  self->width = 1920.0;
}

/* GstChildProxy implementation */
static GObject *
gst_synacompositor_child_proxy_get_child_by_index(GstChildProxy *child_proxy,
                                                  guint index) {
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(child_proxy);
  GObject *obj = nullptr;

  GST_OBJECT_LOCK(self);
  obj = (GObject *)(g_list_nth_data(GST_ELEMENT_CAST(self)->sinkpads, index));
  if (obj)
    gst_object_ref(obj);
  GST_OBJECT_UNLOCK(self);

  return obj;
}

static guint
gst_synacompositor_child_proxy_get_children_count(GstChildProxy *child_proxy) {
  guint count = 0;
  GstSynaCompositor *self = GST_SYNACOMPOSITOR(child_proxy);

  GST_OBJECT_LOCK(self);
  count = GST_ELEMENT_CAST(self)->numsinkpads;
  GST_OBJECT_UNLOCK(self);
  GST_INFO_OBJECT(self, "Children Count: %d", count);

  return count;
}

static void gst_synacompositor_child_proxy_init(gpointer g_iface,
                                                gpointer iface_data) {
  GstChildProxyInterface *iface = (GstChildProxyInterface *)g_iface;

  iface->get_child_by_index = gst_synacompositor_child_proxy_get_child_by_index;
  iface->get_children_count = gst_synacompositor_child_proxy_get_children_count;
}

/* Element registration */
static gboolean plugin_init(GstPlugin *plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_synacompositor_debug, "synacompositor", 0,
                          "synacompositor");

  return GST_ELEMENT_REGISTER(synacompositor, plugin);
}

#ifndef PACKAGE
#define PACKAGE "gstreamer-plugins-syna"
#endif
#ifndef GST_PACKAGE_NAME
#define GST_PACKAGE_NAME "gstreamer-plugins-syna"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://www.synaptics.com/"
#endif

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, synacompositor,
                  "synacompositor", plugin_init, VERSION, "LGPL",
                  GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
