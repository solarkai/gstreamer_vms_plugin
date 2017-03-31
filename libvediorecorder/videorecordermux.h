/* GStreamer split muxer bin
 * Copyright (C) 2014 Jan Schmidt <jan@centricular.com>
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

#ifndef __GST_VIDEORECORDERMUX_H__
#define __GST_VIDEORECORDERMUX_H__

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

G_BEGIN_DECLS

#define GST_TYPE_VIDEORECORDER_MUX               (gst_videorecorder_mux_get_type())
#define GST_VIDEORECORDER_MUX(obj)               (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_VIDEORECORDER_MUX,GstVideoRecorderMux))
#define GST_VIDEORECORDER_MUX_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_VIDEORECORDER_MUX,GstVideoRecorderMuxClass))
#define GST_IS_VIDEORECORDER_MUX(obj)            (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_VIDEORECORDER_MUX))
#define GST_IS_VIDEORECORDER_MUX_CLASS(klass)    (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_VIDEORECORDER_MUX))

typedef struct _GstVideoRecorderMux GstVideoRecorderMux;
typedef struct _GstVideoRecorderMuxClass GstVideoRecorderMuxClass;

GType gst_splitmux_sink_get_type(void);
gboolean register_VideoRecorderMux (GstPlugin * plugin);

//typedef enum _VideoRecorderState {
//  VIDEORECORDER_STATE_STOPPED,
//  VIDEORECORDER_STATE_COLLECTING_GOP_START,
//  VIDEORECORDER_STATE_WAITING_GOP_COMPLETE,
//  VIDEORECORDER_STATE_ENDING_FILE,
//  VIDEORECORDER_STATE_START_NEXT_FRAGMENT,
//} VideoRecorderState;

typedef struct _MqStreamBuf
{
  gboolean keyframe;
  GstClockTime run_ts;
  gsize buf_size;
} MqStreamBuf;

typedef struct _MqStreamCtx
{
  gint refcount;

  GstVideoRecorderMux *videorecordermux;

  guint sink_pad_block_id;
  guint src_pad_block_id;

  gboolean is_video;

  gboolean flushing;
  gboolean in_eos;
  gboolean out_eos;

  GstSegment in_segment;
  GstSegment out_segment;

  GstClockTime in_running_time;
  GstClockTime out_running_time;

  gsize in_bytes;

  GQueue queued_bufs;

  GstPad *sinkpad;
  GstPad *srcpad;

  gboolean out_blocked;
} MqStreamCtx;

struct _GstVideoRecorderMux {
  GstBin parent;

  GMutex lock;
  GCond data_cond;

  /*VideoRecorderState state;*/
  gdouble mux_overhead;

  GstClockTime threshold_time;
  guint64 threshold_bytes;

  guint mq_max_buffers;

  GstElement *mq;
  GstElement *muxer;
  GstElement *sink;

  GstElement *provided_muxer;

  GstElement *provided_sink;
  GstElement *active_sink;

  gchar *location;
  guint fragment_id;

  GList *contexts;

  MqStreamCtx *video_ctx;
  guint queued_gops;
  GstClockTime max_in_running_time;
  GstClockTime max_out_running_time;

  GstClockTime muxed_out_time;
  gsize muxed_out_bytes;
  gboolean have_muxed_something;

  GstClockTime mux_start_time;
  gsize mux_start_bytes;
};

struct _GstVideoRecorderMuxClass {
  GstBinClass parent_class;
};

G_END_DECLS

#endif /* __GST_VIDEORECORDERMUX_H__ */
