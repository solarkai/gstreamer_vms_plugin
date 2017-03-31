/* GStreamer Muxer bin that splits output stream by size/time
 * Copyright (C) <2014> Jan Schmidt <jan@centricular.com>
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

/**
 
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "videorecordermux.h"
#include "matroska-mux.h"

GST_DEBUG_CATEGORY_STATIC (videorecorder_debug);
#define GST_CAT_DEFAULT videorecorder_debug

#define GST_VIDEORECORDER_LOCK(s) g_mutex_lock(&(s)->lock)
#define GST_VIDEORECORDER_UNLOCK(s) g_mutex_unlock(&(s)->lock)
#define GST_VIDEORECORDER_WAIT(s) g_cond_wait (&(s)->data_cond, &(s)->lock)
#define GST_VIDEORECORDER_BROADCAST(s) g_cond_broadcast (&(s)->data_cond)

enum
{
  PROP_0,
  PROP_LOCATION,
  PROP_MAX_SIZE_TIME,
  PROP_MAX_SIZE_BYTES,
  PROP_MUXER_OVERHEAD,
  PROP_MUXER,
  PROP_SINK
};

#define DEFAULT_MAX_SIZE_TIME       0
#define DEFAULT_MAX_SIZE_BYTES      0
#define DEFAULT_MUXER_OVERHEAD      0.02
#define DEFAULT_MUXER "mymatroskamux"
#define DEFAULT_SINK "filesink"
#define DEFAULT_FILE_LOCATION "videorecord%09d.mkv"

enum
{
  SIGNAL_FORMAT_LOCATION,
  SIGNAL_RECORD_INFORMATION,
  SIGNAL_LAST
};

static guint signals[SIGNAL_LAST];

static GstStaticPadTemplate video_sink_template =
GST_STATIC_PAD_TEMPLATE ("video",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate audio_sink_template =
GST_STATIC_PAD_TEMPLATE ("audio_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate subtitle_sink_template =
GST_STATIC_PAD_TEMPLATE ("subtitle_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

static GQuark PAD_CONTEXT;

static void
_do_init (void)
{
  PAD_CONTEXT = g_quark_from_static_string ("pad-context");
}

#define gst_videorecorder_mux_parent_class parent_class
G_DEFINE_TYPE_EXTENDED(GstVideoRecorderMux, gst_videorecorder_mux, GST_TYPE_BIN, 0,
    _do_init ());

static gboolean create_elements (GstVideoRecorderMux * videorecordermux);
static gboolean create_sink(GstVideoRecorderMux * videorecordermux);
static void gst_videorecorder_mux_set_property(GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_videorecorder_mux_get_property(GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_videorecorder_mux_dispose(GObject * object);
static void gst_videorecorder_mux_finalize(GObject * object);

static GstPad *gst_videorecorder_mux_request_new_pad(GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_videorecorder_mux_release_pad(GstElement * element, GstPad * pad);

static GstStateChangeReturn gst_videorecorder_mux_change_state(GstElement *
    element, GstStateChange transition);

static void bus_handler (GstBin * bin, GstMessage * msg);
static void set_next_filename(GstVideoRecorderMux * videorecordermux);
static void start_next_fragment(GstVideoRecorderMux * videorecordermux);
static void check_queue_length(GstVideoRecorderMux * videorecordermux, MqStreamCtx * ctx);
static void mq_stream_ctx_unref (MqStreamCtx * ctx);
static void receive_recordinfo(GstElement *element, RecordInfo *ri, gpointer userdata);

static MqStreamBuf *
mq_stream_buf_new (void)
{
  return g_slice_new0 (MqStreamBuf);
}

static void
mq_stream_buf_free (MqStreamBuf * data)
{
  g_slice_free (MqStreamBuf, data);
}

static void
gst_videorecorder_mux_class_init(GstVideoRecorderMuxClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBinClass *gstbin_class = (GstBinClass *) klass;

  gobject_class->set_property = gst_videorecorder_mux_set_property;
  gobject_class->get_property = gst_videorecorder_mux_get_property;
  gobject_class->dispose = gst_videorecorder_mux_dispose;
  gobject_class->finalize = gst_videorecorder_mux_finalize;

  gst_element_class_set_static_metadata (gstelement_class,
      "Video Recorder Bin", "Generic/Bin/Muxer",
      "Convenience bin that muxes incoming streams into multiple time/size limited files",
      "Zhang Kai <zhang.kai@kedacom.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&video_sink_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&audio_sink_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&subtitle_sink_template));

  gstelement_class->change_state =
	  GST_DEBUG_FUNCPTR(gst_videorecorder_mux_change_state);
  gstelement_class->request_new_pad =
	  GST_DEBUG_FUNCPTR(gst_videorecorder_mux_request_new_pad);
  gstelement_class->release_pad =
	  GST_DEBUG_FUNCPTR(gst_videorecorder_mux_release_pad);

  gstbin_class->handle_message = bus_handler;

  g_object_class_install_property (gobject_class, PROP_LOCATION,
      g_param_spec_string ("location", "File Output Pattern",
          "Format string pattern for the location of the files to write (e.g. video%05d.mkv)",
          NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MUXER_OVERHEAD,
      g_param_spec_double ("mux-overhead", "Muxing Overhead",
          "Extra size overhead of muxing (0.02 = 2%)", 0.0, 1.0,
          DEFAULT_MUXER_OVERHEAD,
          G_PARAM_READWRITE | GST_PARAM_CONTROLLABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_SIZE_TIME,
      g_param_spec_uint64 ("max-size-time", "Max. size (ns)",
          "Max. amount of time per file (in ns, 0=disable)", 0, G_MAXUINT64,
          DEFAULT_MAX_SIZE_TIME, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_MAX_SIZE_BYTES,
      g_param_spec_uint64 ("max-size-bytes", "Max. size bytes",
          "Max. amount of data per file (in bytes, 0=disable)", 0, G_MAXUINT64,
          DEFAULT_MAX_SIZE_BYTES, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MUXER,
      g_param_spec_object ("muxer", "Muxer",
          "The muxer element to use (NULL = default mp4mux)",
          GST_TYPE_ELEMENT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_SINK,
      g_param_spec_object ("sink", "Sink",
          "The sink element (or element chain) to use (NULL = default filesink)",
          GST_TYPE_ELEMENT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstSplitMuxSink::format-location:
   * @splitmux: the #GstSplitMuxSink
   * @fragment_id: the sequence number of the file to be created
   *
   * Returns: the location to be used for the next output file
   */
  signals[SIGNAL_FORMAT_LOCATION] =
      g_signal_new ("format-location", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_STRING, 1, G_TYPE_UINT);

  signals[SIGNAL_RECORD_INFORMATION] =
	  g_signal_new("record-information", G_TYPE_FROM_CLASS(klass),
	  G_SIGNAL_RUN_LAST, 0, NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 4, 
	  G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_UINT64, G_TYPE_UINT64);
}

static void
gst_videorecorder_mux_init(GstVideoRecorderMux * videorecordermux)
{
	g_mutex_init(&videorecordermux->lock);
	g_cond_init(&videorecordermux->data_cond);

  videorecordermux->mux_overhead = DEFAULT_MUXER_OVERHEAD;
  videorecordermux->threshold_time = DEFAULT_MAX_SIZE_TIME;
  videorecordermux->threshold_bytes = DEFAULT_MAX_SIZE_BYTES;

  GST_OBJECT_FLAG_SET(videorecordermux, GST_ELEMENT_FLAG_SINK);
}

static void
gst_videorecorder_reset(GstVideoRecorderMux *videorecordermux)
{
	if (videorecordermux->mq)
		gst_bin_remove(GST_BIN(videorecordermux), videorecordermux->mq);
	if (videorecordermux->muxer)
		gst_bin_remove(GST_BIN(videorecordermux), videorecordermux->muxer);
	if (videorecordermux->active_sink)
		gst_bin_remove(GST_BIN(videorecordermux), videorecordermux->active_sink);

	videorecordermux->sink = videorecordermux->active_sink = videorecordermux->muxer = videorecordermux->mq = NULL;
}

static void
gst_videorecorder_mux_dispose(GObject * object)
{
	GstVideoRecorderMux *videorecordermux = GST_VIDEORECORDER_MUX(object);

    G_OBJECT_CLASS (parent_class)->dispose (object);

  /* Calling parent dispose invalidates all child pointers */
  videorecordermux->sink = videorecordermux->active_sink = videorecordermux->muxer = videorecordermux->mq = NULL;
}

static void
gst_videorecorder_mux_finalize(GObject * object)
{
	GstVideoRecorderMux *videorecordermux = GST_VIDEORECORDER_MUX(object);
	g_cond_clear(&videorecordermux->data_cond);
	g_mutex_clear(&videorecordermux->lock);
	if (videorecordermux->provided_sink)
		gst_object_unref(videorecordermux->provided_sink);
	if (videorecordermux->provided_muxer)
		gst_object_unref(videorecordermux->provided_muxer);

	g_free(videorecordermux->location);

  /* Make sure to free any un-released contexts */
	g_list_foreach(videorecordermux->contexts, (GFunc)mq_stream_ctx_unref, NULL);
	g_list_free(videorecordermux->contexts);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_videorecorder_mux_set_property(GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
	GstVideoRecorderMux *videorecordermux = GST_VIDEORECORDER_MUX(object);

  switch (prop_id) {
    case PROP_LOCATION:{
		GST_OBJECT_LOCK(videorecordermux);
		g_free(videorecordermux->location);
		videorecordermux->location = g_value_dup_string(value);
		GST_OBJECT_UNLOCK(videorecordermux);
      break;
    }
    case PROP_MAX_SIZE_BYTES:
		GST_OBJECT_LOCK(videorecordermux);
		videorecordermux->threshold_bytes = g_value_get_uint64(value);
		GST_OBJECT_UNLOCK(videorecordermux);
      break;
    case PROP_MAX_SIZE_TIME:
		GST_OBJECT_LOCK(videorecordermux);
		videorecordermux->threshold_time = g_value_get_uint64(value);
		GST_OBJECT_UNLOCK(videorecordermux);
      break;
    case PROP_MUXER_OVERHEAD:
		GST_OBJECT_LOCK(videorecordermux);
		videorecordermux->mux_overhead = g_value_get_double(value);
		GST_OBJECT_UNLOCK(videorecordermux);
      break;
    case PROP_SINK:
		GST_OBJECT_LOCK(videorecordermux);
		if (videorecordermux->provided_sink)
			gst_object_unref(videorecordermux->provided_sink);
		videorecordermux->provided_sink = g_value_dup_object(value);
		GST_OBJECT_UNLOCK(videorecordermux);
      break;
    case PROP_MUXER:
		GST_OBJECT_LOCK(videorecordermux);
		if (videorecordermux->provided_muxer)
			gst_object_unref(videorecordermux->provided_muxer);
		videorecordermux->provided_muxer = g_value_dup_object(value);
		GST_OBJECT_UNLOCK(videorecordermux);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_videorecorder_mux_get_property(GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
	GstVideoRecorderMux *videorecordermux = GST_VIDEORECORDER_MUX(object);

  switch (prop_id) {
    case PROP_LOCATION:
      GST_OBJECT_LOCK (videorecordermux);
      g_value_set_string (value, videorecordermux->location);
      GST_OBJECT_UNLOCK (videorecordermux);
      break;
    case PROP_MAX_SIZE_BYTES:
      GST_OBJECT_LOCK (videorecordermux);
      g_value_set_uint64 (value, videorecordermux->threshold_bytes);
      GST_OBJECT_UNLOCK (videorecordermux);
      break;
    case PROP_MAX_SIZE_TIME:
      GST_OBJECT_LOCK (videorecordermux);
      g_value_set_uint64 (value, videorecordermux->threshold_time);
      GST_OBJECT_UNLOCK (videorecordermux);
      break;
    case PROP_MUXER_OVERHEAD:
      GST_OBJECT_LOCK (videorecordermux);
      g_value_set_double (value, videorecordermux->mux_overhead);
      GST_OBJECT_UNLOCK (videorecordermux);
      break;
    case PROP_SINK:
      GST_OBJECT_LOCK (videorecordermux);
      g_value_set_object (value, videorecordermux->provided_sink);
      GST_OBJECT_UNLOCK (videorecordermux);
      break;
    case PROP_MUXER:
      GST_OBJECT_LOCK (videorecordermux);
      g_value_set_object (value, videorecordermux->provided_muxer);
      GST_OBJECT_UNLOCK (videorecordermux);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstPad *
mq_sink_to_src (GstElement * mq, GstPad * sink_pad)
{
  gchar *tmp, *sinkname, *srcname;
  GstPad *mq_src;

  sinkname = gst_pad_get_name (sink_pad);
  tmp = sinkname + 5;
  srcname = g_strdup_printf ("src_%s", tmp);

  mq_src = gst_element_get_static_pad (mq, srcname);

  g_free (sinkname);
  g_free (srcname);

  return mq_src;
}

static gboolean
get_pads_from_mq(GstVideoRecorderMux *videorecordermux, GstPad ** sink_pad,
    GstPad ** src_pad)
{
  GstPad *mq_sink;
  GstPad *mq_src;

  /* Request a pad from multiqueue, then connect this one, then
   * discover the corresponding output pad and return both */
  mq_sink = gst_element_get_request_pad(videorecordermux->mq, "sink_%u");
  if (mq_sink == NULL)
    return FALSE;

  mq_src = mq_sink_to_src(videorecordermux->mq, mq_sink);
  if (mq_src == NULL)
    goto fail;

  *sink_pad = mq_sink;
  *src_pad = mq_src;

  return TRUE;

fail:
  gst_element_release_request_pad(videorecordermux->mq, mq_sink);
  return FALSE;
}

static MqStreamCtx *
mq_stream_ctx_new(GstVideoRecorderMux *videorecordermux)
{
  MqStreamCtx *ctx;

  ctx = g_new0 (MqStreamCtx, 1);
  g_atomic_int_set (&ctx->refcount, 1);
  ctx->videorecordermux = videorecordermux;
  gst_segment_init (&ctx->in_segment, GST_FORMAT_UNDEFINED);
  gst_segment_init (&ctx->out_segment, GST_FORMAT_UNDEFINED);
  ctx->in_running_time = ctx->out_running_time = 0;
  g_queue_init (&ctx->queued_bufs);
  return ctx;
}

static void
mq_stream_ctx_free (MqStreamCtx * ctx)
{
  g_queue_foreach (&ctx->queued_bufs, (GFunc) mq_stream_buf_free, NULL);
  g_queue_clear (&ctx->queued_bufs);
  g_free (ctx);
}

static void
mq_stream_ctx_unref (MqStreamCtx * ctx)
{
  if (g_atomic_int_dec_and_test (&ctx->refcount))
    mq_stream_ctx_free (ctx);
}

static void
mq_stream_ctx_ref (MqStreamCtx * ctx)
{
  g_atomic_int_inc (&ctx->refcount);
}

static void
_pad_block_destroy_sink_notify (MqStreamCtx * ctx)
{
  ctx->sink_pad_block_id = 0;
  mq_stream_ctx_unref (ctx);
}

static void
_pad_block_destroy_src_notify (MqStreamCtx * ctx)
{
  ctx->src_pad_block_id = 0;
  mq_stream_ctx_unref (ctx);
}

/* Called with lock held, drops the lock to send EOS to the
 * pad
 */
static void
send_eos(GstVideoRecorderMux *videorecordermux, MqStreamCtx * ctx)
{
  GstEvent *eos;
  GstPad *pad;

  eos = gst_event_new_eos ();
  pad = gst_pad_get_peer (ctx->srcpad);

  //ctx->out_eos = TRUE;

  GST_DEBUG_OBJECT(videorecordermux, "Sending EOS on %" GST_PTR_FORMAT, pad);
  gst_pad_send_event (pad, eos);

  gst_object_unref (pad);
}


/*pad是multiqueue的srcpad,使用探针根据buf的pts计算分隔时间*/
static GstPadProbeReturn
handle_mq_output (GstPad * pad, GstPadProbeInfo * info, MqStreamCtx * ctx)
{
  GstMapInfo map;
  GstVideoRecorderMux *videorecordermux = ctx->videorecordermux;
  GstClockTime ts,outtime;
  gsize outsize;

  //GST_DEBUG_OBJECT(videorecordermux, "Fired probe type 0x%x\n", info->type);

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    g_warning ("Buffer list handling not implemented");
    return GST_PAD_PROBE_DROP;
  }

  //监听的下行时间
  if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *event = gst_pad_probe_info_get_event (info);

	GST_DEBUG_OBJECT(videorecordermux, "mq output get Event %" GST_PTR_FORMAT, event);

    switch (GST_EVENT_TYPE (event)) {
      case GST_EVENT_SEGMENT:
        gst_event_copy_segment (event, &ctx->out_segment);
		GST_DEBUG_OBJECT(videorecordermux, "get segment event,base:%u,duration:%u,start:%u,time:%u", ctx->out_segment.base, ctx->out_segment.duration, ctx->out_segment.start, ctx->out_segment.time);
        break;
      case GST_EVENT_EOS:
        ctx->out_eos = TRUE;
		GST_DEBUG_OBJECT(videorecordermux, "get out eos from outside!");
        break;
      default:
        break;
    }
    return GST_PAD_PROBE_PASS;
  }

  
  //监听的下行buf
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER)
  {
	  GstBuffer *buf = gst_pad_probe_info_get_buffer(info);
	  if (buf)
	  {   

		  if (GST_BUFFER_PTS_IS_VALID(buf))
			  ts = GST_BUFFER_PTS(buf);
		  else
			  ts = GST_BUFFER_DTS(buf);
		  if (GST_CLOCK_TIME_IS_VALID(ts))
		  {
			  ts = gst_segment_to_running_time(&ctx->out_segment, GST_FORMAT_TIME, GST_BUFFER_TIMESTAMP(buf));
		  }
		  else
		  {
			  GST_ERROR_OBJECT(videorecordermux, "get buf runtime error %u", ts);
			  return GST_PAD_PROBE_PASS;
		  }
		 
		  GST_DEBUG_OBJECT(videorecordermux, "get buf runtime is %I64u,%" GST_TIME_FORMAT, ts,GST_TIME_ARGS(ts));
		  //对于关键帧需要判断运行时间是否已经达到文件分隔门限
		  if ((ctx->is_video && !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)) || (!ctx->is_video))
		  {
				  outtime = ts - videorecordermux->mux_start_time;
				  outsize = videorecordermux->muxed_out_bytes + gst_buffer_get_size(buf) - videorecordermux->mux_start_bytes;
				  if ((videorecordermux->threshold_bytes > 0 && outsize >= videorecordermux->threshold_bytes) ||
					  (videorecordermux->threshold_time > 0 && outtime >= videorecordermux->threshold_time))
				  {
					  videorecordermux->max_out_running_time = ts;
					  GST_DEBUG_OBJECT(videorecordermux, "split file for time:%" GST_TIME_FORMAT",outbytes is :%"G_GSIZE_FORMAT, GST_TIME_ARGS(ts), videorecordermux->muxed_out_bytes);
					  send_eos(videorecordermux, ctx);
					  start_next_fragment(videorecordermux);
					  videorecordermux->mux_start_time = ts;
					  videorecordermux->mux_start_bytes = videorecordermux->muxed_out_bytes;
				  }
		  }

		  //记录下该buf的运行时间和已输出字节数  
		  ctx->out_running_time = ts;
		  videorecordermux->muxed_out_bytes += gst_buffer_get_size(buf);
	  }
  }
  return GST_PAD_PROBE_PASS;
}

static gboolean
resend_sticky (GstPad * pad, GstEvent ** event, GstPad * peer)
{
  return gst_pad_send_event (peer, gst_event_ref (*event));
}

static void
restart_context(MqStreamCtx * ctx, GstVideoRecorderMux *videorecordermux)
{
  GstPad *peer = gst_pad_get_peer (ctx->srcpad);

  gst_pad_sticky_events_foreach (ctx->srcpad,
      (GstPadStickyEventsForeachFunction) (resend_sticky), peer);

  /* Clear EOS flag */
  ctx->out_eos = FALSE;

  gst_object_unref (peer);
}

/* Called with lock held when a fragment
 * reaches EOS and it is time to restart
 * a new fragment
 */
static void
start_next_fragment(GstVideoRecorderMux *videorecordermux)
{
  /* 1 change to new file */
	gst_element_set_state(videorecordermux->muxer, GST_STATE_NULL);
	gst_element_set_state(videorecordermux->active_sink, GST_STATE_NULL);

	set_next_filename(videorecordermux);

	gst_element_sync_state_with_parent(videorecordermux->active_sink);
	gst_element_sync_state_with_parent(videorecordermux->muxer);

	g_list_foreach(videorecordermux->contexts, (GFunc)restart_context, videorecordermux);

  
	GST_DEBUG_OBJECT(videorecordermux,
      "Restarting flow for new fragment. New running time %" GST_TIME_FORMAT,
	  GST_TIME_ARGS(videorecordermux->max_out_running_time));

}

static void
bus_handler (GstBin * bin, GstMessage * message)
{
	GstVideoRecorderMux *videorecordermux = GST_VIDEORECORDER_MUX(bin);

	GST_DEBUG_OBJECT(videorecordermux,"videorecordermux Got %s message\n", GST_MESSAGE_TYPE_NAME(message));
   
  switch (GST_MESSAGE_TYPE (message)) {
  case GST_MESSAGE_EOS:
	  /* If the state is draining out the current file, drop this EOS */
  {
	  gst_message_unref(message);
  }
      break;
    default:
      break;
  }

  //GST_BIN_CLASS (parent_class)->handle_message (bin, message);
}

static GstPad *
gst_videorecorder_mux_request_new_pad(GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps)
{
  GstVideoRecorderMux *videorecordermux = (GstVideoRecorderMux *)element;
  GstPadTemplate *mux_template = NULL;
  GstPad *res = NULL;
  GstPad *mq_sink, *mq_src;
  gchar *gname;
  gboolean is_video = FALSE;
  MqStreamCtx *ctx;

  GST_DEBUG_OBJECT(videorecordermux, "templ:%s, name:%s", templ->name_template, name);

  if (!create_elements(videorecordermux))
    goto fail;

  if (templ->name_template) {
    if (g_str_equal (templ->name_template, "video")) {
      /* FIXME: Look for a pad template with matching caps, rather than by name */
      mux_template =
          gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS
		  (videorecordermux->muxer), "video_%u");
      is_video = TRUE;
      name = NULL;
    } else {
      mux_template =
          gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS
		  (videorecordermux->muxer), templ->name_template);
    }
    if (mux_template == NULL) {
      /* Fallback to find sink pad templates named 'sink_%d' (mpegtsmux) */
      mux_template =
          gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS
		  (videorecordermux->muxer), "sink_%d");
    }
  }

  res = gst_element_request_pad(videorecordermux->muxer, mux_template, name, caps);
  if (res == NULL)
    goto fail;

  if (is_video)
    gname = g_strdup ("video");
  else if (name == NULL)
    gname = gst_pad_get_name (res);
  else
    gname = g_strdup (name);

  if (!get_pads_from_mq(videorecordermux, &mq_sink, &mq_src)) {
	  gst_element_release_request_pad(videorecordermux->muxer, res);
    gst_object_unref (GST_OBJECT (res));
    goto fail;
  }

  if (gst_pad_link (mq_src, res) != GST_PAD_LINK_OK) {
	  gst_element_release_request_pad(videorecordermux->muxer, res);
    gst_object_unref (GST_OBJECT (res));
	gst_element_release_request_pad(videorecordermux->mq, mq_sink);
    gst_object_unref (GST_OBJECT (mq_sink));
    goto fail;
  }

  gst_object_unref (GST_OBJECT (res));

  ctx = mq_stream_ctx_new(videorecordermux);
  ctx->is_video = is_video;
  ctx->srcpad = mq_src;
  ctx->sinkpad = mq_sink;

  mq_stream_ctx_ref (ctx);
  ctx->src_pad_block_id =
      gst_pad_add_probe (mq_src, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM,
      (GstPadProbeCallback) handle_mq_output, ctx, (GDestroyNotify)
      _pad_block_destroy_src_notify);
  if (is_video)
	  videorecordermux->video_ctx = ctx;

  res = gst_ghost_pad_new (gname, mq_sink);
  g_object_set_qdata ((GObject *) (res), PAD_CONTEXT, ctx);


  GST_DEBUG_OBJECT(videorecordermux, "Request pad %" GST_PTR_FORMAT" is mq pad %" GST_PTR_FORMAT, res, mq_sink);

  videorecordermux->contexts = g_list_prepend(videorecordermux->contexts, ctx);

  g_free (gname);

  gst_object_unref (mq_sink);
  gst_object_unref (mq_src);

  gst_pad_set_active (res, TRUE);
  gst_element_add_pad (element, res);

  return res;

fail:
  return NULL;
}

static void
gst_videorecorder_mux_release_pad(GstElement * element, GstPad * pad)
{
  GstVideoRecorderMux *videorecordermux = (GstVideoRecorderMux *)element;
  GstPad *mqsink, *mqsrc, *muxpad;
  MqStreamCtx *ctx =(MqStreamCtx *) (g_object_get_qdata ((GObject *) (pad), PAD_CONTEXT));


  if (videorecordermux->muxer == NULL || videorecordermux->mq == NULL)
    return;                  

  GST_DEBUG_OBJECT(videorecordermux, "releasing request pad");

  mqsink = gst_ghost_pad_get_target (GST_GHOST_PAD (pad));
  mqsrc = mq_sink_to_src(videorecordermux->mq, mqsink);
  muxpad = gst_pad_get_peer (mqsrc);

  /* Remove the context from our consideration */
  videorecordermux->contexts = g_list_remove(videorecordermux->contexts, ctx);

  if (ctx->sink_pad_block_id)
    gst_pad_remove_probe (ctx->sinkpad, ctx->sink_pad_block_id);

  if (ctx->src_pad_block_id)
    gst_pad_remove_probe (ctx->srcpad, ctx->src_pad_block_id);

  /* Can release the context now */
  mq_stream_ctx_unref (ctx);

  /* Release and free the mq input */
  gst_element_release_request_pad(videorecordermux->mq, mqsink);

  /* Release and free the muxer input */
  gst_element_release_request_pad(videorecordermux->muxer, muxpad);

  gst_object_unref (mqsink);
  gst_object_unref (mqsrc);
  gst_object_unref (muxpad);

  gst_element_remove_pad (element, pad);

}

static GstElement *
create_element(GstVideoRecorderMux *videorecordermux,const gchar * factory, const gchar * name)
{
  GstElement *ret = gst_element_factory_make (factory, name);
  if (ret == NULL) {
    g_warning ("Failed to create %s - splitmuxsink will not work", name);
    return NULL;
  }

  if (!gst_bin_add(GST_BIN(videorecordermux), ret)) {
    g_warning ("Could not add %s element - splitmuxsink will not work", name);
    gst_object_unref (ret);
    return NULL;
  }

  return ret;
}

static gboolean
create_elements(GstVideoRecorderMux *videorecordermux)
{
  /* Create internal elements */
	if (videorecordermux->mq == NULL) {
		if ((videorecordermux->mq = create_element(videorecordermux, "multiqueue", "multiqueue")) == NULL)
      goto fail;

	videorecordermux->mq_max_buffers = 5;
    /* No bytes or time limit, we limit buffers manually */
	g_object_set(videorecordermux->mq, "max-size-bytes", 0, "max-size-time",
		(guint64)0, "max-size-buffers", videorecordermux->mq_max_buffers, NULL);
  }

	if (videorecordermux->muxer == NULL) {
    GstElement *provided_muxer = NULL;

	if (videorecordermux->provided_muxer != NULL)
		provided_muxer = gst_object_ref(videorecordermux->provided_muxer);
 

    if (provided_muxer == NULL) {
		if ((videorecordermux->muxer = create_element(videorecordermux, /*"matroskamux"*/DEFAULT_MUXER, "muxer")) == NULL)
            goto fail;
    } else {
			if (!gst_bin_add(GST_BIN(videorecordermux), provided_muxer)) {
			g_warning ("Could not add muxer element - videorecordermux will not work");
			gst_object_unref (provided_muxer);
			goto fail;
            }

	        videorecordermux->muxer = provided_muxer;
            gst_object_unref (provided_muxer);
    }
	/*监听mux的record-info信号*/
	g_signal_connect(videorecordermux->muxer, "record-info",(GCallback)receive_recordinfo, NULL);
  }

  return TRUE;
fail:
  return FALSE;
}

static GstElement *
find_sink (GstElement * e)
{
  GstElement *res = NULL;
  GstIterator *iter;
  gboolean done = FALSE;
  GValue data = { 0, };

  if (!GST_IS_BIN (e))
    return e;

  iter = gst_bin_iterate_sinks (GST_BIN (e));
  while (!done) {
    switch (gst_iterator_next (iter, &data)) {
      case GST_ITERATOR_OK:
      {
        GstElement *child = g_value_get_object (&data);
        if (g_object_class_find_property (G_OBJECT_GET_CLASS (child),"location") != NULL) {
          res = child;
          done = TRUE;
        }
        g_value_reset (&data);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
      case GST_ITERATOR_ERROR:
        g_assert_not_reached ();
        break;
    }
  }
  g_value_unset (&data);
  gst_iterator_free (iter);

  return res;
}

static gboolean
create_sink(GstVideoRecorderMux *videorecordermux)
{
  GstElement *provided_sink = NULL;

  g_return_val_if_fail(videorecordermux->active_sink == NULL, TRUE);

  if (videorecordermux->provided_sink != NULL)
	  provided_sink = gst_object_ref(videorecordermux->provided_sink);

  if (provided_sink == NULL) {
	  if ((videorecordermux->sink =
		  create_element(videorecordermux, DEFAULT_SINK, "sink")) == NULL)
      goto fail;
	  videorecordermux->active_sink = videorecordermux->sink;
  } else {
	  if (!gst_bin_add(GST_BIN(videorecordermux), provided_sink)) {
      g_warning ("Could not add sink elements - videorecordermux will not work");
      gst_object_unref (provided_sink);
      goto fail;
    }

	  videorecordermux->active_sink = provided_sink;

    /* The bin holds a ref now, we can drop our tmp ref */
    gst_object_unref (provided_sink);

    /* Find the sink element */
	videorecordermux->sink = find_sink(videorecordermux->active_sink);
	if (videorecordermux->sink == NULL) {
      g_warning
          ("Could not locate sink element in provided sink - videorecordermux will not work");
      goto fail;
    }
  }

  if (!gst_element_link(videorecordermux->muxer, videorecordermux->active_sink)) {
    g_warning ("Failed to link muxer and sink- videorecordermux will not work");
    goto fail;
  }

  return TRUE;
fail:
  return FALSE;
}

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static void
set_next_filename(GstVideoRecorderMux *videorecordermux)
{
  gchar *fname = NULL;

  g_signal_emit(videorecordermux, signals[SIGNAL_FORMAT_LOCATION], 0,
	  videorecordermux->fragment_id, &fname);

  if (!fname)
	  fname = videorecordermux->location ?
	  g_strdup_printf(videorecordermux->location, videorecordermux->fragment_id) : 
	  g_strdup_printf(DEFAULT_FILE_LOCATION, videorecordermux->fragment_id);

  if (fname) {
	  GST_DEBUG_OBJECT(videorecordermux, "Setting file to %s", fname);
	  g_object_set(videorecordermux->sink, "location", fname, NULL);
    g_free (fname);

	videorecordermux->fragment_id++;
  }
}

static GstStateChangeReturn
gst_videorecorder_mux_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstVideoRecorderMux *videorecordermux = (GstVideoRecorderMux *)element;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:{
	  if (!create_elements(videorecordermux) || !create_sink(videorecordermux)) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto beach;
      }
	  videorecordermux->fragment_id = 0;
	  set_next_filename(videorecordermux);
      break;
    }
    case GST_STATE_CHANGE_READY_TO_PAUSED:{
      /* Start by collecting one input on each pad */
	  videorecordermux->max_in_running_time = 0;
	  videorecordermux->muxed_out_time = videorecordermux->mux_start_time = 0;
	  videorecordermux->muxed_out_bytes = videorecordermux->mux_start_bytes = 0;
      break;
    }
    case GST_STATE_CHANGE_PAUSED_TO_READY:
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    goto beach;

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
		videorecordermux->fragment_id = 0;
		gst_videorecorder_reset(videorecordermux);
      break;
    default:
      break;
  }

beach:

  if (transition == GST_STATE_CHANGE_NULL_TO_READY &&
      ret == GST_STATE_CHANGE_FAILURE) {
    /* Cleanup elements on failed transition out of NULL */
	  gst_videorecorder_reset(videorecordermux);
  }
  return ret;
}

/* 接收来自MUXER的recordinfo信号 */
void receive_recordinfo(GstElement *element, RecordInfo *ri, gpointer  userdata)
{
	gchar *fname = NULL;
	GstVideoRecorderMux *videorecordermux = GST_VIDEORECORDER_MUX(gst_element_get_parent(element));
	if ((videorecordermux) && (videorecordermux->sink))
	{
		g_object_get(G_OBJECT(videorecordermux->sink), "location", &fname, NULL);
		/*g_print(*/GST_DEBUG_OBJECT(videorecordermux,"***filelocation:%s,recsec:%lu,recusec:%lu,startts:%I64u,endts:%I64u\n",
			fname, ri->recordtime.tv_sec, ri->recordtime.tv_usec, ri->startframets,ri->endframets);
		/* 发送信号 */
		g_signal_emit(videorecordermux, signals[SIGNAL_RECORD_INFORMATION], 0, fname, &(ri->recordtime), ri->startframets, ri->endframets);
	}
}

gboolean
register_videorecordermux(GstPlugin * plugin)
{
	GST_DEBUG_CATEGORY_INIT(videorecorder_debug, "videorecordermux", 0,"recorder media File Muxing Sink");

  return gst_element_register (plugin, "videorecordermux", GST_RANK_NONE,GST_TYPE_VIDEORECORDER_MUX);
}



