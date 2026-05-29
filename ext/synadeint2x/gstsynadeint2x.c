/* GStreamer synadeint2x
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
 * gstsynadeint2x.c - V4L2 M2M MPLANE deinterlace element
 *
 * Copy-based (Option A):
 *   - OUTPUT: NV12M (2-plane) interlaced input
 *   - CAPTURE: NV12M (2-plane) progressive output
 *   - Driver returns 2 CAPTURE buffers per 1 OUTPUT buffer
 *   - We copy each CAPTURE buffer into a new GstBuffer and push downstream
 *
 * Default behavior:
 *   - Produces 2 progressive frames per 1 interlaced input frame (2x fps, bob).
 *
 * Optional modes:
 *   - Single-rate output via output-mode property.
 *
 * Example:
 *  gst-launch-1.0 -e -v \
 *    filesrc location=us_flag_720x480_29.97_interlaced_nv12.raw ! \
 *    rawvideoparse format=nv12 width=720 height=480 framerate=30000/1001 ! \
 *    synadeint2x output-mode=0 ! \
 *    filesink location=deint_out.nv12
 */

#include <gst/gst.h>
#include <gst/video/video.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <linux/videodev2.h>


#define GST_TYPE_V4L2DEINT2X (gst_synadeint2x_get_type())
G_DECLARE_FINAL_TYPE (GstSynaDeint2x, gst_synadeint2x, GST, V4L2DEINT2X,
    GstElement)
    GST_DEBUG_CATEGORY_STATIC (gst_synadeint2x_debug);
#define GST_CAT_DEFAULT gst_synadeint2x_debug

#define SYNA_OVPD_VIDEO_DEVICE		"/dev/video-deint"
#define V4L2_OVPD_OUTPUT_BUF_COUNT	4
#define V4L2_OVPD_CAPTURE_BUF_COUNT	8
#define V4L2_OVPD_INIT_OUTPUT_QUEUED	2
#define V4L2_OVPD_INIT_CAPTURE_QUEUED	4
#define V4L2_OVPD_STATS_LOG_INTERVAL	60

/* *INDENT-OFF* */
typedef struct
{
  void *addr[2];
  size_t len[2];
} MPlaneMap;

struct _GstSynaDeint2x
{
  GstElement parent;

  GstPad *sinkpad;
  GstPad *srcpad;

  guint width, height;
  guint fps_n, fps_d;
  gboolean have_caps;

  int fd;
  gboolean streaming;

  guint out_count;
  guint cap_count;

  gint output_mode;
  guint half_field_input_mask;
  guint init_out_q;
  guint init_cap_q;
  gboolean priming;
  guint out_inflight;
  guint prime_done_count;

  MPlaneMap *out_map;
  MPlaneMap *cap_map;

  GQueue *out_free_q;
  GQueue *pts_q;

  guint pair_phase;
  GstClockTime cur_in_pts;

  GstClockTime frame_dur;
  GstClockTime half_dur;

  GThread *thread;
  GMutex lock;
  GCond cond;
  gboolean stop;

  GstFlowReturn last_flow_ret;
  gboolean have_flow_error;

  guint64 input_received_count;
  guint64 output_qbuf_count;
  guint64 output_dqbuf_count;
  guint64 capture_dqbuf_count;
  guint64 capture_qbuf_count;
  guint64 output_generated_count;
  guint64 output_push_ok_count;
  guint64 output_push_fail_count;
  guint64 drop_count;
};

G_DEFINE_TYPE (GstSynaDeint2x, gst_synadeint2x, GST_TYPE_ELEMENT)

enum
{
  PROP_0,
  PROP_OUTPUT_MODE,
  PROP_HALF_FIELD_INPUT_MASK,
};
/* *INDENT-ON* */

     static int xioctl (int fd, unsigned long req, void *arg)
{
  int r;

  do {
    r = ioctl (fd, req, arg);
  } while (r < 0 && errno == EINTR);

  return r;
}

static gboolean
streamon_type (int fd, enum v4l2_buf_type type)
{
  return xioctl (fd, VIDIOC_STREAMON, &type) == 0;
}

static gboolean
streamoff_type (int fd, enum v4l2_buf_type type)
{
  return xioctl (fd, VIDIOC_STREAMOFF, &type) == 0;
}

static int
reqbufs_mmap (int fd, enum v4l2_buf_type type, unsigned count)
{
  struct v4l2_requestbuffers rb;

  memset (&rb, 0, sizeof (rb));
  rb.type = type;
  rb.memory = V4L2_MEMORY_MMAP;
  rb.count = count;

  if (xioctl (fd, VIDIOC_REQBUFS, &rb) < 0)
    return -1;

  return (int) rb.count;
}

static int
mmap_one_index_2plane (int fd, enum v4l2_buf_type type, unsigned index,
    MPlaneMap * m)
{
  struct v4l2_buffer b;
  struct v4l2_plane p[2];
  int i;

  memset (&b, 0, sizeof (b));
  memset (p, 0, sizeof (p));

  b.type = type;
  b.memory = V4L2_MEMORY_MMAP;
  b.index = index;
  b.length = 2;
  b.m.planes = p;

  if (xioctl (fd, VIDIOC_QUERYBUF, &b) < 0)
    return -1;

  for (i = 0; i < 2; i++) {
    m->len[i] = b.m.planes[i].length;
    m->addr[i] = mmap (NULL, m->len[i], PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, b.m.planes[i].m.mem_offset);
    if (m->addr[i] == MAP_FAILED)
      return -1;
  }

  return 0;
}

static int
qbuf_capture_idx (int fd, unsigned idx)
{
  struct v4l2_buffer b;
  struct v4l2_plane p[2];

  memset (&b, 0, sizeof (b));
  memset (p, 0, sizeof (p));

  b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  b.memory = V4L2_MEMORY_MMAP;
  b.index = idx;
  b.length = 2;
  b.m.planes = p;

  p[0].bytesused = 0;
  p[1].bytesused = 0;

  return xioctl (fd, VIDIOC_QBUF, &b);
}

static gboolean
calculate_nv12_plane_sizes (guint width, guint height, guint * ysz,
    guint * uvsz)
{
  guint64 y_size;
  guint64 uv_size;

  if (width == 0 || height == 0)
    return FALSE;

  y_size = (guint64) width *height;
  uv_size = y_size / 2;

  if (y_size > G_MAXUINT || uv_size > G_MAXUINT)
    return FALSE;

  if (ysz)
    *ysz = (guint) y_size;
  if (uvsz)
    *uvsz = (guint) uv_size;

  return TRUE;
}

static gboolean
set_format_nv12m (GstSynaDeint2x * self,
    enum v4l2_buf_type type, gboolean interlaced)
{
  struct v4l2_format f;
  guint ysz;
  guint uvsz;

  if (!calculate_nv12_plane_sizes (self->width, self->height, &ysz, &uvsz))
    return FALSE;

  memset (&f, 0, sizeof (f));

  f.type = type;
  f.fmt.pix_mp.width = self->width;
  f.fmt.pix_mp.height = self->height;
  f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
  f.fmt.pix_mp.num_planes = 2;
  f.fmt.pix_mp.field = interlaced ? V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;

  f.fmt.pix_mp.plane_fmt[0].bytesperline = self->width;
  f.fmt.pix_mp.plane_fmt[0].sizeimage = ysz;
  f.fmt.pix_mp.plane_fmt[1].bytesperline = self->width;
  f.fmt.pix_mp.plane_fmt[1].sizeimage = uvsz;

  if (xioctl (self->fd, VIDIOC_S_FMT, &f) < 0)
    return FALSE;

  if (f.fmt.pix_mp.num_planes != 2)
    return FALSE;

  if (f.fmt.pix_mp.width != self->width || f.fmt.pix_mp.height != self->height)
    return FALSE;

  if (f.fmt.pix_mp.plane_fmt[0].sizeimage < ysz
      || f.fmt.pix_mp.plane_fmt[1].sizeimage < uvsz)
    return FALSE;

  return TRUE;
}

static GstCaps *
make_src_caps (GstSynaDeint2x * self)
{
  guint out_n = self->fps_n;
  guint out_d = self->fps_d ? self->fps_d : 1;

  /* Output caps:
   * output-mode=0   - framerate doubled (2x)
   * output-mode=1/2 - same framerate as input
   */
  if (self->output_mode == 0)
    out_n = self->fps_n * 2;

  return gst_caps_new_simple ("video/x-raw",
      "format", G_TYPE_STRING, "NV12",
      "width", G_TYPE_INT, (int) self->width,
      "height", G_TYPE_INT, (int) self->height,
      "framerate", GST_TYPE_FRACTION, (int) out_n, (int) out_d,
      "interlace-mode", G_TYPE_STRING, "progressive", NULL);
}

static void v4l2_teardown (GstSynaDeint2x * self);

static void
maybe_log_stats (GstSynaDeint2x * self, const gchar * tag)
{
  if ((self->output_generated_count % V4L2_OVPD_STATS_LOG_INTERVAL) == 0) {
    GST_INFO_OBJECT (self,
        "%s stats: in=%" G_GUINT64_FORMAT
        " out_q=%" G_GUINT64_FORMAT
        " out_dq=%" G_GUINT64_FORMAT
        " cap_dq=%" G_GUINT64_FORMAT
        " cap_q=%" G_GUINT64_FORMAT
        " generated=%" G_GUINT64_FORMAT
        " push_ok=%" G_GUINT64_FORMAT
        " push_fail=%" G_GUINT64_FORMAT
        " dropped=%" G_GUINT64_FORMAT
        " inflight=%u priming=%d prime_done=%u",
        tag,
        self->input_received_count,
        self->output_qbuf_count,
        self->output_dqbuf_count,
        self->capture_dqbuf_count,
        self->capture_qbuf_count,
        self->output_generated_count,
        self->output_push_ok_count,
        self->output_push_fail_count,
        self->drop_count,
        self->out_inflight, self->priming, self->prime_done_count);
  }
}

static void
drain_output (GstSynaDeint2x * self)
{
  while (1) {
    struct v4l2_buffer b;
    struct v4l2_plane p[2];

    memset (&b, 0, sizeof (b));
    memset (p, 0, sizeof (p));

    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.length = 2;
    b.m.planes = p;

    if (xioctl (self->fd, VIDIOC_DQBUF, &b) < 0) {
      if (errno == EAGAIN)
        break;

      GST_ERROR_OBJECT (self, "OUTPUT DQBUF failed: %s", g_strerror (errno));
      g_mutex_lock (&self->lock);
      self->stop = TRUE;
      self->have_flow_error = TRUE;
      self->last_flow_ret = GST_FLOW_ERROR;
      g_cond_broadcast (&self->cond);
      g_mutex_unlock (&self->lock);
      break;
    }

    g_mutex_lock (&self->lock);

    self->output_dqbuf_count++;
    if (self->out_inflight > 0)
      self->out_inflight--;

    g_queue_push_tail (self->out_free_q, GUINT_TO_POINTER (b.index));

    if (self->priming) {
      self->prime_done_count++;
      if (self->prime_done_count >= self->init_out_q) {
        guint i;

        self->priming = FALSE;
        for (i = self->init_cap_q; i < self->cap_count; i++) {
          if (qbuf_capture_idx (self->fd, i) < 0) {
            GST_WARNING_OBJECT (self,
                "Failed to QBUF remaining CAPTURE idx %u during priming: %s",
                i, g_strerror (errno));
          } else {
            self->capture_qbuf_count++;
          }
        }

        GST_INFO_OBJECT (self,
            "Priming finished: out_done=%u capture_queued=%u",
            self->prime_done_count, self->cap_count);
      }
    }

    g_cond_signal (&self->cond);
    g_mutex_unlock (&self->lock);
  }
}

static GstClockTime
next_pts_for_capture (GstSynaDeint2x * self, gboolean * is_second)
{
  GstClockTime pts;

  g_mutex_lock (&self->lock);
  if (self->pair_phase == 0) {
    if (!g_queue_is_empty (self->pts_q))
      self->cur_in_pts =
          (GstClockTime) (guintptr) g_queue_pop_head (self->pts_q);
    else
      self->cur_in_pts = GST_CLOCK_TIME_NONE;

    self->pair_phase = 1;
    if (is_second)
      *is_second = FALSE;
    pts = self->cur_in_pts;
  } else {
    self->pair_phase = 0;
    if (is_second)
      *is_second = TRUE;
    if (self->cur_in_pts != GST_CLOCK_TIME_NONE
        && self->half_dur != GST_CLOCK_TIME_NONE)
      pts = self->cur_in_pts + self->half_dur;
    else
      pts = GST_CLOCK_TIME_NONE;
  }
  g_mutex_unlock (&self->lock);

  return pts;
}

static gint
get_poll_timeout_ms (GstSynaDeint2x * self)
{
  GstClockTime frame_dur;
  guint64 timeout_ms;

  if (self->fps_n == 0 || self->fps_d == 0)
    return 200;                 /* safe fallback */

  frame_dur = gst_util_uint64_scale_int (GST_SECOND, self->fps_d, self->fps_n);

  /* 5 input frame durations */
  timeout_ms = gst_util_uint64_scale (frame_dur, 5, GST_MSECOND);

  /* Clamp to avoid too-small or too-large values */
  if (timeout_ms < 50)
    timeout_ms = 50;
  if (timeout_ms > 500)
    timeout_ms = 500;

  return (gint) timeout_ms;
}

static gpointer
capture_thread (gpointer data)
{
  GstSynaDeint2x *self = GST_V4L2DEINT2X (data);
  struct pollfd pfd;
  guint ysz;
  guint uvsz;
  gint poll_timeout_ms = get_poll_timeout_ms (self);

  if (!calculate_nv12_plane_sizes (self->width, self->height, &ysz, &uvsz)) {
    GST_ERROR_OBJECT (self, "Invalid frame dimensions %ux%u",
        self->width, self->height);
    g_mutex_lock (&self->lock);
    self->stop = TRUE;
    self->have_flow_error = TRUE;
    self->last_flow_ret = GST_FLOW_ERROR;
    g_cond_broadcast (&self->cond);
    g_mutex_unlock (&self->lock);
    return NULL;
  }

  memset (&pfd, 0, sizeof (pfd));
  pfd.fd = self->fd;
  pfd.events = POLLIN | POLLPRI | POLLOUT;

  while (1) {
    gboolean stop;
    int r;

    g_mutex_lock (&self->lock);
    stop = self->stop;
    g_mutex_unlock (&self->lock);

    if (stop)
      break;

    r = poll (&pfd, 1, poll_timeout_ms);
    if (r < 0) {
      if (errno == EINTR)
        continue;

      GST_ERROR_OBJECT (self, "poll failed: %s", g_strerror (errno));
      g_mutex_lock (&self->lock);
      self->stop = TRUE;
      self->have_flow_error = TRUE;
      self->last_flow_ret = GST_FLOW_ERROR;
      g_cond_broadcast (&self->cond);
      g_mutex_unlock (&self->lock);
      break;
    }

    if (r == 0)
      continue;

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      GST_ERROR_OBJECT (self, "poll revents error: 0x%x", pfd.revents);
      g_mutex_lock (&self->lock);
      self->stop = TRUE;
      self->have_flow_error = TRUE;
      self->last_flow_ret = GST_FLOW_ERROR;
      g_cond_broadcast (&self->cond);
      g_mutex_unlock (&self->lock);
      break;
    }

    drain_output (self);

    while (1) {
      struct v4l2_buffer b;
      struct v4l2_plane p[2];
      guint idx, y_used, uv_used, total;
      GstClockTime pts;
      gboolean is_second;
      gboolean drop_output;
      GstBuffer *out;
      GstMapInfo mi;
      GstFlowReturn fr;

      memset (&b, 0, sizeof (b));
      memset (p, 0, sizeof (p));

      b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      b.memory = V4L2_MEMORY_MMAP;
      b.length = 2;
      b.m.planes = p;

      if (xioctl (self->fd, VIDIOC_DQBUF, &b) < 0) {
        int err = errno;

        if (err == EAGAIN)
          break;

        if (err == ENODEV)
          GST_ERROR_OBJECT (self, "Device removed (ENODEV) during DQBUF");
        else if (err == EIO || err == EPIPE)
          GST_ERROR_OBJECT (self, "Streaming error during DQBUF: %s",
              g_strerror (err));
        else
          GST_ERROR_OBJECT (self, "CAPTURE DQBUF failed: %s", g_strerror (err));

        g_mutex_lock (&self->lock);
        self->stop = TRUE;
        self->have_flow_error = TRUE;
        self->last_flow_ret = GST_FLOW_ERROR;
        g_cond_broadcast (&self->cond);
        g_mutex_unlock (&self->lock);
        break;
      }

      g_mutex_lock (&self->lock);
      self->capture_dqbuf_count++;
      g_mutex_unlock (&self->lock);

      idx = b.index;
      y_used = p[0].bytesused ? p[0].bytesused : ysz;
      uv_used = p[1].bytesused ? p[1].bytesused : uvsz;
      total = y_used + uv_used;

      pts = next_pts_for_capture (self, &is_second);
      drop_output = (self->output_mode == 1 && !is_second) ||
          (self->output_mode == 2 && is_second);

      g_mutex_lock (&self->lock);
      self->output_generated_count++;
      g_mutex_unlock (&self->lock);

      if (drop_output) {
        GST_DEBUG_OBJECT (self,
            "Dropping %s generated output due to output-mode=%d",
            is_second ? "second" : "first", self->output_mode);
        g_mutex_lock (&self->lock);
        self->drop_count++;
        maybe_log_stats (self, "capture");
        g_mutex_unlock (&self->lock);
        goto requeue_capture;
      }

      if (y_used > self->cap_map[idx].len[0] ||
          uv_used > self->cap_map[idx].len[1]) {
        GST_ERROR_OBJECT (self,
            "Driver bytesused exceeds mapped size: y_used=%u y_len=%zu uv_used=%u uv_len=%zu",
            y_used, self->cap_map[idx].len[0],
            uv_used, self->cap_map[idx].len[1]);

        g_mutex_lock (&self->lock);
        self->drop_count++;
        g_mutex_unlock (&self->lock);

        goto requeue_capture;
      }

      out = gst_buffer_new_allocate (NULL, total, NULL);
      if (G_UNLIKELY (out == NULL)) {
        GST_ERROR_OBJECT (self, "Failed to allocate GstBuffer");
        g_mutex_lock (&self->lock);
        self->drop_count++;
        g_mutex_unlock (&self->lock);
        goto requeue_capture;
      }

      if (gst_buffer_map (out, &mi, GST_MAP_WRITE)) {
        if (mi.size < total) {
          GST_ERROR_OBJECT (self,
              "Output GstBuffer too small: mapped=%zu required=%u y_used=%u uv_used=%u",
              mi.size, total, y_used, uv_used);

          g_mutex_lock (&self->lock);
          self->drop_count++;
          g_mutex_unlock (&self->lock);

          gst_buffer_unmap (out, &mi);
          gst_buffer_unref (out);
          goto requeue_capture;
        }

        memcpy (mi.data, self->cap_map[idx].addr[0], y_used);
        memcpy (mi.data + y_used, self->cap_map[idx].addr[1], uv_used);
        gst_buffer_unmap (out, &mi);
      } else {
        GST_ERROR_OBJECT (self, "Failed to map output GstBuffer");
        g_mutex_lock (&self->lock);
        self->drop_count++;
        g_mutex_unlock (&self->lock);
        gst_buffer_unref (out);
        goto requeue_capture;
      }

      if (self->output_mode != 0 && self->cur_in_pts != GST_CLOCK_TIME_NONE)
        pts = self->cur_in_pts;

      if (pts != GST_CLOCK_TIME_NONE)
        GST_BUFFER_PTS (out) = pts;
      if (self->output_mode == 0 && self->half_dur != GST_CLOCK_TIME_NONE)
        GST_BUFFER_DURATION (out) = self->half_dur;
      else if (self->output_mode != 0 && self->frame_dur != GST_CLOCK_TIME_NONE)
        GST_BUFFER_DURATION (out) = self->frame_dur;

      g_mutex_lock (&self->lock);
      maybe_log_stats (self, "capture");
      g_mutex_unlock (&self->lock);

      fr = gst_pad_push (self->srcpad, out);
      if (fr != GST_FLOW_OK) {
        GST_WARNING_OBJECT (self, "Push returned %s; stopping",
            gst_flow_get_name (fr));
        g_mutex_lock (&self->lock);
        self->last_flow_ret = fr;
        self->have_flow_error = TRUE;
        self->stop = TRUE;
        self->output_push_fail_count++;
        self->drop_count++;
        g_cond_broadcast (&self->cond);
        g_mutex_unlock (&self->lock);
        break;
      }

      g_mutex_lock (&self->lock);
      self->output_push_ok_count++;
      g_mutex_unlock (&self->lock);

    requeue_capture:
      if (qbuf_capture_idx (self->fd, idx) < 0) {
        GST_ERROR_OBJECT (self, "CAPTURE QBUF failed: %s", g_strerror (errno));
        g_mutex_lock (&self->lock);
        self->stop = TRUE;
        self->have_flow_error = TRUE;
        self->last_flow_ret = GST_FLOW_ERROR;
        g_cond_broadcast (&self->cond);
        g_mutex_unlock (&self->lock);
        break;
      }

      g_mutex_lock (&self->lock);
      self->capture_qbuf_count++;
      g_mutex_unlock (&self->lock);
    }
  }

  return NULL;
}

static gboolean
v4l2_setup (GstSynaDeint2x * self)
{
  struct v4l2_capability cap;
  int oc, cc;
  guint i;
  guint ncap_q;

  if (self->streaming)
    return TRUE;

  self->fd = open (SYNA_OVPD_VIDEO_DEVICE, O_RDWR | O_NONBLOCK);
  if (self->fd < 0) {
    GST_ERROR_OBJECT (self, "Failed to open %s: %s",
        SYNA_OVPD_VIDEO_DEVICE, g_strerror (errno));
    return FALSE;
  }

  memset (&cap, 0, sizeof (cap));

  if (xioctl (self->fd, VIDIOC_QUERYCAP, &cap) < 0) {
    GST_ERROR_OBJECT (self, "VIDIOC_QUERYCAP failed: %s", g_strerror (errno));
    v4l2_teardown (self);
    return FALSE;
  }

  if (!(cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)) {
    GST_ERROR_OBJECT (self, "Device is not V4L2 M2M MPLANE");
    v4l2_teardown (self);
    return FALSE;
  }

  if (!set_format_nv12m (self, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, TRUE)) {
    GST_ERROR_OBJECT (self, "Failed to set OUTPUT NV12M format");
    v4l2_teardown (self);
    return FALSE;
  }

  if (!set_format_nv12m (self, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, FALSE)) {
    GST_ERROR_OBJECT (self, "Failed to set CAPTURE NV12M format");
    v4l2_teardown (self);
    return FALSE;
  }

  oc = reqbufs_mmap (self->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
      self->out_count);
  cc = reqbufs_mmap (self->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
      self->cap_count);
  if (oc <= 0 || cc <= 0) {
    GST_ERROR_OBJECT (self, "REQBUFS failed (out=%d cap=%d): %s", oc, cc,
        g_strerror (errno));
    v4l2_teardown (self);
    return FALSE;
  }
  self->out_count = (guint) oc;
  self->cap_count = (guint) cc;

  self->out_map = g_new0 (MPlaneMap, self->out_count);
  self->cap_map = g_new0 (MPlaneMap, self->cap_count);

  for (i = 0; i < self->out_count; i++) {
    if (mmap_one_index_2plane (self->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            i, &self->out_map[i]) < 0) {
      GST_ERROR_OBJECT (self, "MMAP OUTPUT idx %u failed: %s", i,
          g_strerror (errno));
      v4l2_teardown (self);
      return FALSE;
    }
  }

  for (i = 0; i < self->cap_count; i++) {
    if (mmap_one_index_2plane (self->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            i, &self->cap_map[i]) < 0) {
      GST_ERROR_OBJECT (self, "MMAP CAPTURE idx %u failed: %s", i,
          g_strerror (errno));
      v4l2_teardown (self);
      return FALSE;
    }
  }

  g_mutex_lock (&self->lock);
  g_queue_clear (self->out_free_q);
  for (i = 0; i < self->out_count; i++)
    g_queue_push_tail (self->out_free_q, GUINT_TO_POINTER (i));
  g_queue_clear (self->pts_q);

  self->pair_phase = 0;
  self->cur_in_pts = GST_CLOCK_TIME_NONE;
  self->stop = FALSE;
  self->have_flow_error = FALSE;
  self->last_flow_ret = GST_FLOW_OK;
  self->priming = TRUE;
  self->out_inflight = 0;
  self->prime_done_count = 0;

  self->input_received_count = 0;
  self->output_qbuf_count = 0;
  self->output_dqbuf_count = 0;
  self->capture_dqbuf_count = 0;
  self->capture_qbuf_count = 0;
  self->output_generated_count = 0;
  self->output_push_ok_count = 0;
  self->output_push_fail_count = 0;
  self->drop_count = 0;
  g_mutex_unlock (&self->lock);

  ncap_q = MIN (self->init_cap_q, self->cap_count);
  for (i = 0; i < ncap_q; i++) {
    if (qbuf_capture_idx (self->fd, i) < 0) {
      GST_ERROR_OBJECT (self, "CAPTURE QBUF (priming) idx %u failed: %s",
          i, g_strerror (errno));
      v4l2_teardown (self);
      return FALSE;
    }

    g_mutex_lock (&self->lock);
    self->capture_qbuf_count++;
    g_mutex_unlock (&self->lock);
  }

  if (!streamon_type (self->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ||
      !streamon_type (self->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)) {
    GST_ERROR_OBJECT (self, "STREAMON failed: %s", g_strerror (errno));
    v4l2_teardown (self);
    return FALSE;
  }

  self->thread = g_thread_new ("synadeint2x-cap", capture_thread, self);
  self->streaming = TRUE;

  GST_INFO_OBJECT (self,
      "V4L2 started on %s (out=%u cap=%u, priming out=%u cap=%u)",
      SYNA_OVPD_VIDEO_DEVICE, self->out_count, self->cap_count,
      self->init_out_q, self->init_cap_q);
  return TRUE;
}

static void
v4l2_teardown (GstSynaDeint2x * self)
{
  if (self->thread) {
    g_mutex_lock (&self->lock);
    self->stop = TRUE;
    g_cond_broadcast (&self->cond);
    g_mutex_unlock (&self->lock);
    g_thread_join (self->thread);
    self->thread = NULL;
  }

  if (self->fd >= 0) {
    if (!streamoff_type (self->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE))
      GST_WARNING_OBJECT (self, "STREAMOFF CAPTURE failed: %s",
          g_strerror (errno));

    if (!streamoff_type (self->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE))
      GST_WARNING_OBJECT (self, "STREAMOFF OUTPUT failed: %s",
          g_strerror (errno));
  }

  GST_INFO_OBJECT (self,
      "final stats: in=%" G_GUINT64_FORMAT
      " out_q=%" G_GUINT64_FORMAT
      " out_dq=%" G_GUINT64_FORMAT
      " cap_dq=%" G_GUINT64_FORMAT
      " cap_q=%" G_GUINT64_FORMAT
      " generated=%" G_GUINT64_FORMAT
      " push_ok=%" G_GUINT64_FORMAT
      " push_fail=%" G_GUINT64_FORMAT
      " dropped=%" G_GUINT64_FORMAT,
      self->input_received_count,
      self->output_qbuf_count,
      self->output_dqbuf_count,
      self->capture_dqbuf_count,
      self->capture_qbuf_count,
      self->output_generated_count,
      self->output_push_ok_count,
      self->output_push_fail_count, self->drop_count);

  if (self->out_map) {
    guint i;
    for (i = 0; i < self->out_count; i++) {
      int p;
      for (p = 0; p < 2; p++) {
        if (self->out_map[i].addr[p] && self->out_map[i].addr[p] != MAP_FAILED)
          munmap (self->out_map[i].addr[p], self->out_map[i].len[p]);
      }
    }
    g_free (self->out_map);
    self->out_map = NULL;
  }

  if (self->cap_map) {
    guint i;
    for (i = 0; i < self->cap_count; i++) {
      int p;
      for (p = 0; p < 2; p++) {
        if (self->cap_map[i].addr[p] && self->cap_map[i].addr[p] != MAP_FAILED)
          munmap (self->cap_map[i].addr[p], self->cap_map[i].len[p]);
      }
    }
    g_free (self->cap_map);
    self->cap_map = NULL;
  }

  if (self->fd >= 0) {
    close (self->fd);
    self->fd = -1;
  }

  self->streaming = FALSE;
}

static gboolean
sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstSynaDeint2x *self = GST_V4L2DEINT2X (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps = NULL;
      GstVideoInfo vinfo;
      GstCaps *scaps;

      gst_event_parse_caps (event, &caps);
      if (!caps)
        return gst_pad_event_default (pad, parent, event);

      if (!gst_video_info_from_caps (&vinfo, caps)) {
        GST_ERROR_OBJECT (self, "Failed to parse caps");
        return FALSE;
      }

      if (GST_VIDEO_INFO_FORMAT (&vinfo) != GST_VIDEO_FORMAT_NV12) {
        GST_ERROR_OBJECT (self, "Only NV12 supported (input)");
        return FALSE;
      }

      if (!calculate_nv12_plane_sizes (GST_VIDEO_INFO_WIDTH (&vinfo),
              GST_VIDEO_INFO_HEIGHT (&vinfo), NULL, NULL)) {
        GST_ERROR_OBJECT (self, "Unsupported frame dimensions %dx%d",
            GST_VIDEO_INFO_WIDTH (&vinfo), GST_VIDEO_INFO_HEIGHT (&vinfo));
        return FALSE;
      }

      if (self->streaming) {
        GST_INFO_OBJECT (self, "Reconfiguring active V4L2 stream for new caps");
        v4l2_teardown (self);

        g_mutex_lock (&self->lock);
        self->stop = FALSE;
        self->have_flow_error = FALSE;
        self->last_flow_ret = GST_FLOW_OK;
        self->pair_phase = 0;
        self->cur_in_pts = GST_CLOCK_TIME_NONE;
        self->out_inflight = 0;
        self->prime_done_count = 0;
        self->priming = TRUE;

        if (self->pts_q)
          g_queue_clear (self->pts_q);
        if (self->out_free_q)
          g_queue_clear (self->out_free_q);
        g_mutex_unlock (&self->lock);
      }

      self->width = GST_VIDEO_INFO_WIDTH (&vinfo);
      self->height = GST_VIDEO_INFO_HEIGHT (&vinfo);
      self->fps_n = GST_VIDEO_INFO_FPS_N (&vinfo);
      self->fps_d = GST_VIDEO_INFO_FPS_D (&vinfo);
      self->have_caps = TRUE;

      if (self->fps_n != 0) {
        self->frame_dur =
            gst_util_uint64_scale_int (GST_SECOND, self->fps_d, self->fps_n);
        self->half_dur = self->frame_dur / 2;
      } else {
        self->frame_dur = GST_CLOCK_TIME_NONE;
        self->half_dur = GST_CLOCK_TIME_NONE;
      }

      scaps = make_src_caps (self);

      GST_INFO_OBJECT (self, "Negotiated caps: sink=%" GST_PTR_FORMAT
          " src=%" GST_PTR_FORMAT, caps, scaps);

      if (!gst_pad_set_caps (self->srcpad, scaps)) {
        GST_ERROR_OBJECT (self, "Failed to set src caps: %" GST_PTR_FORMAT,
            scaps);
        gst_caps_unref (scaps);
        return FALSE;
      }

      if (!gst_pad_push_event (self->srcpad, gst_event_new_caps (scaps))) {
        GST_ERROR_OBJECT (self, "Failed to push src caps: %" GST_PTR_FORMAT,
            scaps);
        gst_caps_unref (scaps);
        return FALSE;
      }

      gst_caps_unref (scaps);

      return TRUE;
    }

    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (self, "Received FLUSH_START");

      g_mutex_lock (&self->lock);
      self->stop = TRUE;
      self->have_flow_error = TRUE;
      self->last_flow_ret = GST_FLOW_FLUSHING;
      g_cond_broadcast (&self->cond);
      g_mutex_unlock (&self->lock);

      return gst_pad_event_default (pad, parent, event);

    case GST_EVENT_FLUSH_STOP:
      GST_DEBUG_OBJECT (self, "Received FLUSH_STOP");

      v4l2_teardown (self);

      g_mutex_lock (&self->lock);
      self->stop = FALSE;
      self->have_flow_error = FALSE;
      self->last_flow_ret = GST_FLOW_OK;
      self->pair_phase = 0;
      self->cur_in_pts = GST_CLOCK_TIME_NONE;
      self->out_inflight = 0;
      self->prime_done_count = 0;
      self->priming = TRUE;

      if (self->pts_q)
        g_queue_clear (self->pts_q);
      if (self->out_free_q)
        g_queue_clear (self->out_free_q);

      g_cond_broadcast (&self->cond);
      g_mutex_unlock (&self->lock);

      return gst_pad_event_default (pad, parent, event);

    case GST_EVENT_EOS:
      GST_DEBUG_OBJECT (self, "Received EOS");

      g_mutex_lock (&self->lock);
      self->stop = TRUE;
      self->last_flow_ret = GST_FLOW_EOS;
      self->have_flow_error = TRUE;
      g_cond_broadcast (&self->cond);
      g_mutex_unlock (&self->lock);

      v4l2_teardown (self);

      return gst_pad_push_event (self->srcpad, event);

    default:
      return gst_pad_event_default (pad, parent, event);
  }
}

static GstFlowReturn
sink_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstSynaDeint2x *self = GST_V4L2DEINT2X (parent);
  GstClockTime in_pts;
  guint ysz;
  guint uvsz;
  guint idx;
  GstMapInfo mi;
  struct v4l2_buffer b;
  struct v4l2_plane p[2];

  (void) pad;

  if (!self->have_caps) {
    GST_ERROR_OBJECT (self,
        "No caps yet: put rawvideoparse before synadeint2x");
    gst_buffer_unref (buf);
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (!calculate_nv12_plane_sizes (self->width, self->height, &ysz, &uvsz)) {
    GST_ERROR_OBJECT (self, "Invalid frame dimensions %ux%u",
        self->width, self->height);
    gst_buffer_unref (buf);
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (!self->streaming) {
    if (!v4l2_setup (self)) {
      gst_buffer_unref (buf);
      return GST_FLOW_ERROR;
    }
  }

  g_mutex_lock (&self->lock);

  while (!self->stop && !self->have_flow_error) {
    guint out_limit = self->priming ? MIN (self->init_out_q,
        self->out_count) : self->out_count;
    if (self->out_inflight < out_limit && !g_queue_is_empty (self->out_free_q))
      break;
    g_cond_wait (&self->cond, &self->lock);
  }

  if (self->have_flow_error) {
    GstFlowReturn ret = self->last_flow_ret;
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return ret;
  }

  if (self->stop) {
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return GST_FLOW_FLUSHING;
  }

  idx = (guint) (guintptr) g_queue_pop_head (self->out_free_q);
  self->out_inflight++;
  self->input_received_count++;
  g_mutex_unlock (&self->lock);

  if (!gst_buffer_map (buf, &mi, GST_MAP_READ)) {
    GST_ERROR_OBJECT (self, "Failed to map input buffer");
    g_mutex_lock (&self->lock);
    self->drop_count++;
    if (self->out_inflight > 0)
      self->out_inflight--;
    g_queue_push_tail (self->out_free_q, GUINT_TO_POINTER (idx));
    g_cond_signal (&self->cond);
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }

  if (mi.size < (gsize) (ysz + uvsz)) {
    GST_ERROR_OBJECT (self, "Input buffer too small (%zu), expected >= %u",
        mi.size, ysz + uvsz);
    gst_buffer_unmap (buf, &mi);
    g_mutex_lock (&self->lock);
    self->drop_count++;
    if (self->out_inflight > 0)
      self->out_inflight--;
    g_queue_push_tail (self->out_free_q, GUINT_TO_POINTER (idx));
    g_cond_signal (&self->cond);
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }

  if (self->out_map[idx].len[0] < ysz || self->out_map[idx].len[1] < uvsz) {
    GST_ERROR_OBJECT (self,
        "Mapped OUTPUT plane too small: y_len=%zu required=%u uv_len=%zu required=%u",
        self->out_map[idx].len[0], ysz, self->out_map[idx].len[1], uvsz);
    gst_buffer_unmap (buf, &mi);
    g_mutex_lock (&self->lock);
    self->drop_count++;
    if (self->out_inflight > 0)
      self->out_inflight--;
    g_queue_push_tail (self->out_free_q, GUINT_TO_POINTER (idx));
    g_cond_signal (&self->cond);
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }

  memcpy (self->out_map[idx].addr[0], mi.data, ysz);
  memcpy (self->out_map[idx].addr[1], mi.data + ysz, uvsz);
  gst_buffer_unmap (buf, &mi);

  in_pts =
      GST_BUFFER_PTS_IS_VALID (buf) ? GST_BUFFER_PTS (buf) :
      GST_CLOCK_TIME_NONE;

  memset (&b, 0, sizeof (b));
  memset (p, 0, sizeof (p));
  b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
  b.memory = V4L2_MEMORY_MMAP;
  b.index = idx;
  b.length = 2;
  b.m.planes = p;
  p[0].bytesused = ysz;
  p[1].bytesused = uvsz;

  if (xioctl (self->fd, VIDIOC_QBUF, &b) < 0) {
    GST_ERROR_OBJECT (self, "OUTPUT QBUF failed: %s", g_strerror (errno));
    g_mutex_lock (&self->lock);
    self->drop_count++;
    if (self->out_inflight > 0)
      self->out_inflight--;
    g_queue_push_tail (self->out_free_q, GUINT_TO_POINTER (idx));
    g_cond_signal (&self->cond);
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return GST_FLOW_ERROR;
  }

  g_mutex_lock (&self->lock);
  self->output_qbuf_count++;
  g_queue_push_tail (self->pts_q, (gpointer) (guintptr) in_pts);
  g_mutex_unlock (&self->lock);

  gst_buffer_unref (buf);
  return GST_FLOW_OK;
}

static GstStateChangeReturn
change_state (GstElement * element, GstStateChange transition)
{
  GstSynaDeint2x *self = GST_V4L2DEINT2X (element);
  GstStateChangeReturn ret;

  ret =
      GST_ELEMENT_CLASS (gst_synadeint2x_parent_class)->change_state (element,
      transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      v4l2_teardown (self);
      break;
    default:
      break;
  }

  return ret;
}


static void
gst_synadeint2x_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSynaDeint2x *self = GST_V4L2DEINT2X (object);

  switch (prop_id) {
    case PROP_OUTPUT_MODE:
      g_mutex_lock (&self->lock);
      self->output_mode = g_value_get_int (value);
      g_mutex_unlock (&self->lock);
      GST_INFO_OBJECT (self, "output-mode set to %d", self->output_mode);
      break;

    case PROP_HALF_FIELD_INPUT_MASK:
    {
      guint mask = g_value_get_uint (value);

      if (mask == 3) {
        GST_WARNING_OBJECT (self,
            "half-field-input-mask=3 is ambiguous; expected 0, 1, or 2");
      }

      g_mutex_lock (&self->lock);
      self->half_field_input_mask = mask;
      g_mutex_unlock (&self->lock);
      GST_INFO_OBJECT (self, "half-field-input-mask set to 0x%x", mask);
      break;
    }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_synadeint2x_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSynaDeint2x *self = GST_V4L2DEINT2X (object);

  switch (prop_id) {
    case PROP_OUTPUT_MODE:
      g_value_set_int (value, self->output_mode);
      break;

    case PROP_HALF_FIELD_INPUT_MASK:
      g_value_set_uint (value, self->half_field_input_mask);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_synadeint2x_dispose (GObject * obj)
{
  GstSynaDeint2x *self = GST_V4L2DEINT2X (obj);
  v4l2_teardown (self);
  G_OBJECT_CLASS (gst_synadeint2x_parent_class)->dispose (obj);
}

static void
gst_synadeint2x_finalize (GObject * obj)
{
  GstSynaDeint2x *self = GST_V4L2DEINT2X (obj);

  if (self->out_free_q)
    g_queue_free (self->out_free_q);
  if (self->pts_q)
    g_queue_free (self->pts_q);

  g_mutex_clear (&self->lock);
  g_cond_clear (&self->cond);

  G_OBJECT_CLASS (gst_synadeint2x_parent_class)->finalize (obj);
}

static void
gst_synadeint2x_class_init (GstSynaDeint2xClass * klass)
{
  GObjectClass *gobj = G_OBJECT_CLASS (klass);
  GstElementClass *ec = GST_ELEMENT_CLASS (klass);
  GstCaps *sink_caps;
  GstCaps *src_caps;
  GstPadTemplate *sink_t;
  GstPadTemplate *src_t;

  gobj->set_property = gst_synadeint2x_set_property;
  gobj->get_property = gst_synadeint2x_get_property;
  gobj->dispose = gst_synadeint2x_dispose;
  gobj->finalize = gst_synadeint2x_finalize;

  ec->change_state = change_state;

  g_object_class_install_property (gobj, PROP_OUTPUT_MODE,
      g_param_spec_int ("output-mode",
          "Output mode",
          "0=double rate (default, 2x fps), "
          "1=single rate (drop first output), "
          "2=single rate (drop second output)",
          0, 2, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobj, PROP_HALF_FIELD_INPUT_MASK,
      g_param_spec_uint ("half-field-input-mask", "Half field input mask",
          "0=both fields present, bit0=top field only present, bit1=bottom field only present",
          0, 3, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (ec,
      "V4L2 Deinterlace 2x (bob)",
      "Filter/Video",
      "Deinterlaces interlaced NV12 input. "
      "Default output is double frame rate (2x, bob). "
      "Single-rate modes available via output-mode property.", "Synaptics Inc");

  GST_DEBUG_CATEGORY_INIT (gst_synadeint2x_debug,
      "synadeint2x", 0, "Synaptics V4L2 deinterlace element");

  sink_caps = gst_caps_from_string ("video/x-raw, "
      "format=(string)NV12, "
      "width=(int)[1,MAX], "
      "height=(int)[1,MAX], " "framerate=(fraction)[0/1,MAX]");

  src_caps = gst_caps_from_string ("video/x-raw, "
      "format=(string)NV12, "
      "width=(int)[1,MAX], "
      "height=(int)[1,MAX], "
      "framerate=(fraction)[0/1,MAX], " "interlace-mode=(string)progressive");

  sink_t =
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink_caps);
  src_t = gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, src_caps);

  gst_element_class_add_pad_template (ec, sink_t);
  gst_element_class_add_pad_template (ec, src_t);

  gst_caps_unref (sink_caps);
  gst_caps_unref (src_caps);
}

static void
gst_synadeint2x_init (GstSynaDeint2x * self)
{
  GstPadTemplate *sink_t;
  GstPadTemplate *src_t;

  self->fd = -1;
  self->streaming = FALSE;
  self->have_caps = FALSE;
  self->output_mode = 0;
  self->half_field_input_mask = 0;

  self->out_count = V4L2_OVPD_OUTPUT_BUF_COUNT;
  self->cap_count = V4L2_OVPD_CAPTURE_BUF_COUNT;
  self->init_out_q = V4L2_OVPD_INIT_OUTPUT_QUEUED;
  self->init_cap_q = V4L2_OVPD_INIT_CAPTURE_QUEUED;
  self->priming = TRUE;
  self->out_inflight = 0;
  self->prime_done_count = 0;

  self->last_flow_ret = GST_FLOW_OK;
  self->have_flow_error = FALSE;

  self->input_received_count = 0;
  self->output_qbuf_count = 0;
  self->output_dqbuf_count = 0;
  self->capture_dqbuf_count = 0;
  self->capture_qbuf_count = 0;
  self->output_generated_count = 0;
  self->output_push_ok_count = 0;
  self->output_push_fail_count = 0;
  self->drop_count = 0;

  self->out_free_q = g_queue_new ();
  self->pts_q = g_queue_new ();

  g_mutex_init (&self->lock);
  g_cond_init (&self->cond);

  sink_t =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (G_OBJECT_GET_CLASS
          (self)), "sink");
  src_t =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (G_OBJECT_GET_CLASS
          (self)), "src");

  self->sinkpad = gst_pad_new_from_template (sink_t, "sink");
  self->srcpad = gst_pad_new_from_template (src_t, "src");

  gst_pad_set_chain_function (self->sinkpad, GST_DEBUG_FUNCPTR (sink_chain));
  gst_pad_set_event_function (self->sinkpad, GST_DEBUG_FUNCPTR (sink_event));

  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "synadeint2x", GST_RANK_NONE,
      GST_TYPE_V4L2DEINT2X);
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

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    synadeint2x,
    "V4L2 deinterlace producing 2 outputs per input (copy-based)",
    plugin_init, "1.0", "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
