// GST_TEST.cpp : 定义控制台应用程序的入口点。
//

#include <stdio.h>
#include "GST_TEST.h"
#include <gst/sdp/gstsdpmessage.h>

static GMainLoop *loop;

#define GST_TEST_LOCK(s) g_mutex_lock(&(s)->lock)
#define GST_TEST_UNLOCK(s) g_mutex_unlock(&(s)->lock)
#define GST_TEST_WAIT(s) g_cond_wait (&(s)->data_cond, &(s)->lock)
#define GST_TEST_BROADCAST(s) g_cond_broadcast (&(s)->data_cond)

typedef struct {
	GMutex lock;
	GCond data_cond;
} TestLock;
TestLock  lockobj;

GstElement *pipeline;


GstPadProbeReturn handle_myfilter_out(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	GstElement * parent = gst_pad_get_parent_element(pad);
	if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
		GstMapInfo map;
		GstEvent *event = gst_pad_probe_info_get_event(info);
		g_print("Probe received %s event: %" GST_PTR_FORMAT,GST_EVENT_TYPE_NAME(event), event);
		GST_TEST_LOCK(&lockobj);
		GST_TEST_WAIT(&lockobj);
		GstBuffer *buf = gst_pad_probe_info_get_buffer(info);
		gst_buffer_map(buf, &map, GST_MAP_READ);
		if (buf)
		{
			g_print("==============Gstbuffsize:tsize:%u,memorys:%u,offset:%u,offset_end:%u\n", gst_buffer_get_size(buf), gst_buffer_n_memory(buf), buf->offset, buf->offset_end);
			g_print("head 5 byte[%02X,%02X,%02X,%02X,%02X,%02X],nal type:%d,keyframe:%d\n", map.data[0], map.data[1], map.data[2], map.data[3], map.data[4], map.data[5], map.data[4] & 0x1f, !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT));
		}
		GST_TEST_UNLOCK(&lockobj);
	}
	return GST_PAD_PROBE_PASS;
}

static gboolean
resend_sticky(GstPad * pad, GstEvent ** event, GstPad * peer)
{
	if (GST_EVENT_TYPE(*event) == GST_EVENT_EOS)
	{
		return TRUE;
	}
	else
	{
		return gst_pad_send_event(peer, gst_event_ref(*event));
	}
}

static gboolean my_bus_callback(GstBus     *bus,GstMessage *message,gpointer    data)
{
	g_print("Got %s message\n", GST_MESSAGE_TYPE_NAME(message));
	GstFormat format;
	gint64 position;
	static gint64 index = 0;
	GstElement *pipeline,*sink,*filter;
	GstPad *sinkpad, *peersrcpad;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR: {
		GError *err;
		gchar *debug;

		gst_message_parse_error(message, &err, &debug);
		g_print("Error: %s\n", err->message);
		g_error_free(err);
		g_free(debug);

		g_main_loop_quit(loop);
		break;
	}
	case GST_MESSAGE_EOS:
		/* end-of-stream */
		gst_message_unref(message);
		//g_main_loop_quit(loop);
		
		if (GST_IS_ELEMENT(data))
		{
			sink = GST_ELEMENT(data);
			sinkpad = gst_element_get_static_pad(sink, "sink");
			peersrcpad = gst_pad_get_peer(sinkpad);
			filter = gst_pad_get_parent_element(peersrcpad);

			index++;
			gst_element_set_state(sink, GST_STATE_NULL);
			gchar *filename = g_strdup_printf("sink_%02d.wav",index);
			g_object_set(data, "location", filename, NULL);
			pipeline = (GstElement *)gst_element_get_parent(sink);
			if (GST_IS_PIPELINE(pipeline))
			{
			   //gst_element_set_state(pipeline, GST_STATE_PLAYING);
			   //gst_element_set_state(sink, GST_STATE_PLAYING);
			   gst_element_sync_state_with_parent(sink);
			}
			gst_pad_sticky_events_foreach(peersrcpad, (GstPadStickyEventsForeachFunction)(resend_sticky), sinkpad);
		}

		break;
	case GST_MESSAGE_SEGMENT_START:
		gst_message_parse_segment_start(message, &format,&position);
		g_print("**************GST_MESSAGE_SEGMENT_START:%d,%u\n", format, position);
		break;
	default:
		/* unhandled message */
		break;
	}

	/* we want to be notified again the next time there is a message
	* on the bus, so returning TRUE (FALSE means we want to stop watching
	* for messages on the bus and our callback should not be called again)
	*/
	return TRUE;
}

static void cb_new_pad(GstElement *element,GstPad     *pad,gpointer    data)
{
	gchar *name;

	name = gst_pad_get_name(pad);
	g_print("***************A new pad %s was created\n", name);
	g_free(name);

	if (NULL != data)
	{
		GstElement *decoder = (GstElement *)data;
		GstPad *sinkpad;
		sinkpad = gst_element_get_static_pad(decoder, "sink");
		gst_pad_link(pad, sinkpad);
		gst_object_unref(sinkpad);
	}

}

static gboolean print_field(GQuark field, const GValue * value, gpointer pfx) {
	gchar *str = gst_value_serialize(value);

	g_print("%s  %15s: %s\n", (gchar *)pfx, g_quark_to_string(field), str);
	g_free(str);
	return TRUE;
}

//rtspsrc的pad增加后连接sink的pad
static void cb_rtspsrc_new_pad(GstElement *element, GstPad     *pad, gpointer    data)
{
	gchar *name;
	GstPad *rtph264depay_srcpad;

	name = gst_pad_get_name(pad);
	g_print("***************A new pad %s was created\n", name);
	

	if (g_str_has_prefix(name, "recv_rtp_src_0") || g_str_has_prefix(name, "src_")) //rtspsrc,decodebin产生的src pad
	{
		/*GstCaps *ptSrccaps = gst_pad_query_caps(pad, NULL);
		for (int i = 0; i < gst_caps_get_size(ptSrccaps); i++){
			GstStructure *ptStru = gst_caps_get_structure(ptSrccaps, i);
			const gchar *ptStru_Name = gst_structure_get_name(ptStru);

			g_print("***************%s pad the %d cap is: %s \n", name, i,ptStru_Name);
			gst_structure_foreach(ptStru, print_field, "####");
		}*/

		if (NULL != data)
		{
			GstPad *sinkpad = (GstPad *)data;
			gst_pad_link(pad, sinkpad);
			gst_object_unref(sinkpad);
		}

		/*if (rtph264depay){
			rtph264depay_srcpad = gst_element_get_static_pad(rtph264depay, "src");
			if (rtph264depay_srcpad)
			{
				GstCaps *ptSrccaps = gst_pad_query_caps(rtph264depay_srcpad, NULL);
				for (int i = 0; i < gst_caps_get_size(ptSrccaps); i++){
					GstStructure *ptStru = gst_caps_get_structure(ptSrccaps, i);
					const gchar *ptStru_Name = gst_structure_get_name(ptStru);

					g_print("***************rtph264depay_srcpad the %d cap is: %s \n", i, ptStru_Name);
					gst_structure_foreach(ptStru, print_field, "####");
				}
			}
		}*/
	}

	if (g_str_has_prefix(name, "recv_rtp_src_1"))
	{
		/*GstCaps *ptSrccaps = gst_pad_query_caps(pad, NULL);
		for (int i = 0; i < gst_caps_get_size(ptSrccaps); i++){
			GstStructure *ptStru = gst_caps_get_structure(ptSrccaps, i);
			const gchar *ptStru_Name = gst_structure_get_name(ptStru);

			g_print("***************%s pad the %d cap is: %s \n", name, i, ptStru_Name);
			gst_structure_foreach(ptStru, print_field, "####");
		}*/
	}

	g_free(name);
}


static void read_video_props(GstCaps *caps)
{
	gint width, height;
	const GstStructure *str;
	g_return_if_fail(gst_caps_is_fixed(caps));
	str = gst_caps_get_structure(caps, 0);
	if (!gst_structure_get_int(str, "width", &width) ||
		!gst_structure_get_int(str, "height", &height)) {
		g_print("No width/height available\n");
		return;
	}
	g_print("The video size of this set of capabilities is %dx%d\n",
		width, height);
}

/*rtspsrc的on-sdp信号*/
void  on_rtspsrc_onsdp_call(GstElement *element, GstSDPMessage *sdp, gpointer user_data)
{
	g_print("sdp from server get!\n");
	g_print("get attrnum:%d\n", gst_sdp_message_attributes_len(sdp));
	guint dwMediaNum = gst_sdp_message_medias_len(sdp);
	g_print("stream num: %d\n", dwMediaNum);
	for (int i = 0; i < dwMediaNum; i++)
	{
		const GstSDPMedia *media = gst_sdp_message_get_media(sdp, i);
		const gchar *attrval = gst_sdp_media_get_attribute_val(media, "control");
		g_print("stream %d has control val is:%s\n",i,attrval);
	}
}

/*rtspsrc的on-select-stream信号*/
gboolean on_rtspsrc_selstream_call(GstElement *element, guint num, GstCaps   *caps, gpointer user_data)
{
	g_print("rtspsrc select stream %d\n",num);
	GstStructure *ptStru = gst_caps_get_structure(caps,0);
	const gchar *ptStru_Media = gst_structure_get_string(ptStru, "media");
	g_print("rtspsrc select stream  media name is :%s\n", ptStru_Media);
	return true;
}

/*rtspsrc的new manager信号*/
void on_rtspsrc_newmanager_call(GstElement *rtspsrc, GstElement *manager, gpointer    user_data)
{
	g_print("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXrtspsrc new manager:%s \n", GST_ELEMENT_NAME(manager));
}

/*decodebin 的 autoplug-continue和autoplug-select信号*/
void on_decodebin_autoplug_call(GstElement *bin, GstPad  *pad, GstCaps *caps, gpointer  user_data)
{
	gchar *name;

	name = gst_pad_get_name(pad);
	g_print("***************on_decodebin_autoplug_call,pad name is:%s\n", name);

	if (caps)
	{
		for (int i = 0; i < gst_caps_get_size(caps); i++){
			GstStructure *ptStru = gst_caps_get_structure(caps, i);
			const gchar *ptStru_Name = gst_structure_get_name(ptStru);

			g_print("***************%s pad the %d cap is: %s \n", name, i, ptStru_Name);
			gst_structure_foreach(ptStru, print_field, "####");
		}
	}

	g_free(name);
}

/* The appsink has received a buffer */
void new_buffer(GstElement *sink, gpointer *data) {
	GstBuffer *buffer;
	GstMapInfo map;

	/* Retrieve the buffer */
	g_signal_emit_by_name(sink, "pull-preroll", &buffer);
	if (buffer) {
		/* The only thing we do in this example is print a * to indicate a received buffer */
		/* Generate some psychodelic waveforms */
		if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
			gst_util_dump_mem(map.data, map.size);
			g_print("-------------------------------------------\n");

		}
		gst_buffer_unref(buffer);
	}
}

int main (int argc, char *argv[])
{
	
	g_mutex_init(&lockobj.lock);
	g_cond_init(&lockobj.data_cond);
	/*gchar *printBytes = "head [";
	printBytes = g_strconcat(printBytes, "bytes[");
	g_print(printBytes);*/
	
   recordRtspStream(argc, argv);
  //return showVersion(argc,argv);
  //goption(argc, argv);
  //gCreateGElement(argc, argv);
  //linkElement(argc, argv);
  //loopBusMsg(argc, argv);
  //createPad(argc, argv);
  //linkRequestPad(argc, argv);
  //createOggPlayer(argc, argv);
   //getRtspStream(argc, argv);
	//testH264filterMuti(argc, argv);
  //appsrcsinktestMain(argc, argv);
}



//定时器回调函数
static gboolean cb_add_streamoutput(GstElement *ele)
{
	GstElement *pipeline, *h264filter;
	GstElement *rtph264depay;
	GstElement  *queue, *decodebin, *autovideosink;
	GstPad *autovideosink_sinkpad, *h264fsrcpad, *queue_sinkpad;

	g_print("timeout for h264filter output start!\n");
	queue = gst_element_factory_make("queue", "queue");
	decodebin = gst_element_factory_make("decodebin", "decode-bin");
	autovideosink = gst_element_factory_make("autovideosink", "auto-videosink");

	gst_bin_add_many(GST_BIN(pipeline), queue, decodebin, autovideosink, NULL);
	gst_element_link_many(queue, decodebin, NULL);

	//pipeline的输出
	autovideosink_sinkpad = gst_element_get_static_pad(autovideosink, "sink");
	g_signal_connect(decodebin, "pad-added", G_CALLBACK(cb_rtspsrc_new_pad), autovideosink_sinkpad);  //decodebin's src PAD link autovideosink's sink 
	h264fsrcpad = gst_element_get_request_pad(h264filter, "h264src_%u");
	queue_sinkpad = gst_element_get_static_pad(queue, "sink");
	if (h264fsrcpad)
	{
		gst_pad_link(h264fsrcpad, queue_sinkpad);
	}

	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	return TRUE;
}

//定时产生h264filter的流输出
int testH264filterMuti(int argc, char *argv[])
{
	GstElement *pipeline, *h264filter;
	GstElement *rtph264depay;
	GstElement  *rtspclient, *fakesink, *queue;
	GstBus *bus;
	guint bus_watch_id,padnum;
	GstPad *rtph264depay_sinkpad, *h264fsrcpad, *queue_sinkpad;
    gchar* rtspurl = "rtsp://172.16.64.191/id=1";
	
	
	/* init */
	gst_init(&argc, &argv);
	loop = g_main_loop_new(NULL, FALSE);
	padnum = 300;

	/* Create gstreamer elements */
	pipeline = gst_pipeline_new("rtsp-client");
	rtspclient = gst_element_factory_make("rtspsrc", "rtsp-src");
	rtph264depay = gst_element_factory_make("rtph264depay", "rtp-h264-depay");
	h264filter = gst_element_factory_make("h264filter", "h264-filter");

	//设置rtspclient元素属性
	g_object_set(G_OBJECT(rtspclient), "location", rtspurl, NULL);
	g_object_set(G_OBJECT(rtspclient), "user-id", "admin", NULL);
	g_object_set(G_OBJECT(rtspclient), "user-pw", "admin", NULL);
	g_object_set(G_OBJECT(rtspclient), "debug", TRUE, NULL);

	gst_bin_add_many(GST_BIN(pipeline), rtspclient, rtph264depay, h264filter, NULL);
	gst_element_link_many(rtph264depay,h264filter, NULL);

	/* we add a message handler */
	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	bus_watch_id = gst_bus_add_watch(bus, my_bus_callback, NULL);
	//gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_EOS);
	gst_object_unref(bus);

	/*处理rtspclient的pad-adder信号*/
	rtph264depay_sinkpad = gst_element_get_static_pad(rtph264depay, "sink");
	g_signal_connect(rtspclient, "pad-added", G_CALLBACK(cb_rtspsrc_new_pad), rtph264depay_sinkpad);  //rtspsrc's src PAD link decodebin's sink pad  
	g_signal_connect(rtspclient, "on-sdp", G_CALLBACK(on_rtspsrc_onsdp_call), NULL);
	g_signal_connect(rtspclient, "select-stream", G_CALLBACK(on_rtspsrc_selstream_call), NULL);
	g_signal_connect(rtspclient, "new-manager", G_CALLBACK(on_rtspsrc_newmanager_call), NULL);

	//g_timeout_add_seconds(5, (GSourceFunc)cb_add_streamoutput,NULL);
	//cb_add_streamoutput(NULL);
	for (guint i = 0; i < padnum; i++)
	{
		gchar *sinkName = g_strdup_printf("fakesink_%u", i);
		gchar *queueName = g_strdup_printf("queue_%u", i);
		fakesink = gst_element_factory_make("fakesink", sinkName);
		queue = gst_element_factory_make("queue", queueName);
		gst_bin_add_many(GST_BIN(pipeline), fakesink,queue,NULL);
		gst_element_link(queue,fakesink);

		h264fsrcpad = gst_element_get_request_pad(h264filter, "h264src_%u");
		queue_sinkpad = gst_element_get_static_pad(queue, "sink");
		if (h264fsrcpad)
		{
			gst_pad_link(h264fsrcpad, queue_sinkpad);
		}
	}

	/* Set the pipeline to "playing" state*/
	g_print("Now playing:\n");
	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	/* Iterate */
	g_print("Running...\n");
	g_main_loop_run(loop);

	/* Out of the main loop, clean up nicely */
	g_print("Returned, stopping playback\n");
	gst_element_set_state(pipeline, GST_STATE_NULL);

	g_print("Deleting pipeline\n");
	gst_object_unref(GST_OBJECT(pipeline));
	g_source_remove(bus_watch_id);
	g_main_loop_unref(loop);
	return 0;
}

static void receive_record_infomation(GstElement *element, gchar *fname, GTimeVal* rectime, GstClockTime start, GstClockTime end, gpointer  userdata)
{
	if (fname && rectime)
	{
	  g_print("***filelocation:%s,recsec:%lu,recusec:%lu,startts:%I64u,endts:%I64u\n",
			fname, rectime->tv_sec, rectime->tv_usec, start, end);
	}
}

static gchar* change_record_filename(GstElement *element, guint frgid, gpointer userdata)
{
	gchar *newfilename = g_strdup_printf("test_%09d.mkv", frgid);
	return newfilename;
}

/*定时录像控制函数*/
static gboolean cb_justprint(GstPad *pad)
{
	static gint num;
	g_print("this is the %d time for print\n",++num);
	if((num == 9) && (pad))
	{
		if (GST_IS_PAD(pad))
		{
			GstEvent *eos;
			eos = gst_event_new_eos();
			gst_pad_send_event(pad, eos);

			//g_main_loop_quit(loop); 
		}
	}
	return TRUE;
}

/*使用videorecordermux进行文件录像分隔*/
int recordRtspStream(int argc, char *argv[])
{
	GstElement  *rtspclient, *rtph264depay, *h264parse,*videorecorder;
	GstBus *bus;
	guint bus_watch_id;
	GstPad  *rtph264depay_sinkpad, *h264parse_srcpad, *videorecorder_sinkpad;

	gchar* rtspurl = "rtsp://172.16.64.200/id=1"; /*"rtsp://172.16.64.59/test10.264";*/
	gchar* filelocation = "video_record/vr%09d.mkv";

	/* init */
	gst_init(&argc, &argv);
	loop = g_main_loop_new(NULL, FALSE);

	/* Create gstreamer elements */
	pipeline = gst_pipeline_new("rtsp-recorder");
	rtspclient = gst_element_factory_make("rtspsrc", "rtsp-src");
	rtph264depay = gst_element_factory_make("rtph264depay", "rtp-h264-depay");
	h264parse = gst_element_factory_make("h264parse", "h264-parse");
	videorecorder = gst_element_factory_make("videorecordermux", "video-recorder");

	if (!pipeline || !rtspclient || !rtph264depay || !h264parse || !videorecorder ) {
		g_printerr("One element could not be created. Exiting.\n");
		return -1;
	}

	//设置元素属性
	g_object_set(G_OBJECT(rtspclient), "location", rtspurl, NULL);
	g_object_set(G_OBJECT(rtspclient), "user-id", "admin", NULL);
	g_object_set(G_OBJECT(rtspclient), "user-pw", "admin", NULL);
	g_object_set(G_OBJECT(rtspclient), "debug", TRUE, NULL);
	g_object_set(G_OBJECT(videorecorder), "max-size-time", 10000000000, NULL);
	g_object_set(G_OBJECT(videorecorder), "location", filelocation, NULL);

	/*添加元件到管道*/
	gst_bin_add_many(GST_BIN(pipeline), rtspclient, rtph264depay, h264parse, videorecorder, NULL);

	//链接元素
	gst_element_link_many(rtph264depay, h264parse, NULL);

	/*连接pad*/
	rtph264depay_sinkpad = gst_element_get_static_pad(rtph264depay, "sink");
	h264parse_srcpad = gst_element_get_static_pad(h264parse, "src");
	g_signal_connect(rtspclient, "pad-added", G_CALLBACK(cb_rtspsrc_new_pad), rtph264depay_sinkpad);

	videorecorder_sinkpad = gst_element_get_request_pad(videorecorder, "video");
	gst_pad_link(h264parse_srcpad, videorecorder_sinkpad);

	/*管道消息监听 */
	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	bus_watch_id = gst_bus_add_watch(bus, my_bus_callback, NULL);
	//gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_EOS);
	gst_object_unref(bus);
	g_signal_connect(videorecorder, "record-information", G_CALLBACK(receive_record_infomation), NULL);
	g_signal_connect(videorecorder, "format-location", G_CALLBACK(change_record_filename), NULL);

	g_timeout_add_seconds(5, (GSourceFunc)cb_justprint, videorecorder_sinkpad);


	/* Set the pipeline to "playing" state*/
	g_print("Now playing:\n");
	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	/* Iterate */
	g_print("Running...\n");
	g_main_loop_run(loop);

	/* Out of the main loop, clean up nicely */
	g_print("Returned, stopping playback\n");
	gst_element_set_state(pipeline, GST_STATE_NULL);

	g_print("Deleting pipeline\n");
	gst_object_unref(GST_OBJECT(pipeline));
	g_source_remove(bus_watch_id);
	g_main_loop_unref(loop);
	return 0;



}

/*使用rtspsrc 元素从rtsp服务器获取流*/
int getRtspStream(int argc, char *argv[])
{
	GstElement *pipeline;
	GstElement *rtph264depay;
	GstElement  *rtspclient, *decodebin,  *autovideosink,  *h264filter,*filesink,*tee/*,*queue1,*queue2*/;
	GstBus *bus;
	guint bus_watch_id;
	GstPad *decodebin_sinkpad, *decodebin_sinkpad1, *autovideosink_sinkpad, *h264parser_sinkpad, *autovideosink_sinkpad1, *rtph264depay_sinkpad, *h264fsrcpad, *h264fsrcpad1,*filesink_sinkpad;
	

	gchar* rtspurl = "rtsp://172.16.64.191/id=1"; /*"rtsp://172.16.64.59/test10.264";*/

	/* init */
	gst_init(&argc, &argv);
	loop = g_main_loop_new(NULL, FALSE);

	/* Create gstreamer elements */
	pipeline = gst_pipeline_new("rtsp-client");
	rtspclient = gst_element_factory_make("rtspsrc", "rtsp-src");
	rtph264depay = gst_element_factory_make("rtph264depay", "rtp-h264-depay");
	h264filter = gst_element_factory_make("h264filter", "h264-filter");
	
	decodebin = gst_element_factory_make("decodebin", "decode-bin");
	autovideosink = gst_element_factory_make("autovideosink", "auto-videosink");

	tee = gst_element_factory_make("tee", "tee");

	/*decodebin1 = gst_element_factory_make("decodebin", "decode-bin1");
	autovideosink1 = gst_element_factory_make("autovideosink", "auto-videosink1");*/
	filesink = gst_element_factory_make("filesink", "file-sink");

	if (!pipeline || !rtspclient || !decodebin || !autovideosink || !rtph264depay || !h264filter /*|| !autovideosink1 || !decodebin1*/) {
		g_printerr("One element could not be created. Exiting.\n");
		return -1;
	}

	//设置rtspclient元素属性
	g_object_set(G_OBJECT(rtspclient), "location", rtspurl, NULL);
	g_object_set(G_OBJECT(rtspclient), "user-id", "admin", NULL);
	g_object_set(G_OBJECT(rtspclient), "user-pw", "admin", NULL);
	g_object_set(G_OBJECT(rtspclient), "debug", TRUE, NULL);
	g_object_set(G_OBJECT(filesink),"location" ,"rtsp191.264", NULL);

	/* we add all elements into the pipeline */
	gst_bin_add_many(GST_BIN(pipeline), rtspclient, rtph264depay, h264filter, /*tee,*//* filesink,*/ decodebin, autovideosink, NULL);
	//链接元素
	gst_element_link_many(rtph264depay, /*tee*/h264filter, NULL);

	/* we add a message handler */
	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	bus_watch_id = gst_bus_add_watch(bus, my_bus_callback, NULL);
	//gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_EOS);
	gst_object_unref(bus);

	
	/*处理rtspclient的pad-adder信号*/
	rtph264depay_sinkpad = gst_element_get_static_pad(rtph264depay, "sink");
	g_signal_connect(rtspclient, "pad-added", G_CALLBACK(cb_rtspsrc_new_pad), rtph264depay_sinkpad);  //rtspsrc's src PAD link decodebin's sink pad  
	g_signal_connect(rtspclient, "on-sdp", G_CALLBACK(on_rtspsrc_onsdp_call), NULL);
	g_signal_connect(rtspclient, "select-stream", G_CALLBACK(on_rtspsrc_selstream_call), NULL);
	g_signal_connect(rtspclient, "new-manager", G_CALLBACK(on_rtspsrc_newmanager_call), NULL);

	

	//pipeline的第一个输出
	autovideosink_sinkpad = gst_element_get_static_pad(autovideosink, "sink");
	g_signal_connect(decodebin, "pad-added", G_CALLBACK(cb_rtspsrc_new_pad), autovideosink_sinkpad);  //decodebin's src PAD link autovideosink's sink 
	h264fsrcpad = gst_element_get_request_pad(/*tee,"src_%u"*/h264filter,"h264src_%u");
	decodebin_sinkpad = gst_element_get_static_pad(decodebin, "sink");
	if (h264fsrcpad)
	{
		gst_pad_link(h264fsrcpad, decodebin_sinkpad);
	}

	//pipeline的第二个输出
	h264fsrcpad1 = gst_element_get_request_pad(/*tee, "src_%u"*/h264filter, "h264src_%u");
	filesink_sinkpad = gst_element_get_static_pad(filesink, "sink");
	if (h264fsrcpad1)
	{
		gst_pad_link(h264fsrcpad1, filesink_sinkpad);
	}

	/* Set the pipeline to "playing" state*/
	g_print("Now playing:\n");
	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	/* Iterate */
	g_print("Running...\n");
	g_main_loop_run(loop);

	/* Out of the main loop, clean up nicely */
	g_print("Returned, stopping playback\n");
	gst_element_set_state(pipeline, GST_STATE_NULL);

	g_print("Deleting pipeline\n");
	gst_object_unref(GST_OBJECT(pipeline));
	g_source_remove(bus_watch_id);
	g_main_loop_unref(loop);
	return 0;
}

static GstPadProbeReturn  handle_myfilter_output(GstPad * pad, GstPadProbeInfo * info,GstElement *sink)
{
	static gint64 index = 0;
	GstPad *sinkpad = gst_element_get_static_pad(sink, "sink");

	if (info->type & GST_PAD_PROBE_TYPE_BUFFER)
	{
		GstBuffer *buf = gst_pad_probe_info_get_buffer(info);
		if (buf)
		{
			index++;
			g_print("push buffer index:%d\n", index);
			if ((0 == (index % 5)) && (sinkpad))
			{
				g_print("split file for index: %d\n", index);
				GstEvent *eos;
				eos = gst_event_new_eos();
				gst_pad_send_event(sinkpad, eos);

				//重置对端状态
				gst_element_set_state(sink, GST_STATE_NULL);
				gchar *filename = g_strdup_printf("sink_%02d.wav", index);
				g_object_set(sink, "location", filename, NULL);
				
				pipeline = GST_ELEMENT(gst_element_get_parent(sink));
				if (GST_IS_PIPELINE(pipeline))
				{
				 //gst_element_set_state(pipeline, GST_STATE_PLAYING);
				 gst_element_sync_state_with_parent(sink);
				}
				//gst_pad_sticky_events_foreach(pad, (GstPadStickyEventsForeachFunction)(resend_sticky), sinkpad);
			}
		}
		//gst_buffer_unref(buf);
		return GST_PAD_PROBE_PASS;
	}
	else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)
	{
		GstEvent *event = gst_pad_probe_info_get_event(info);
		//g_print("probe Event %s \n",  GST_EVENT_TYPE_NAME(event));

		switch (GST_EVENT_TYPE(event)) {
		case GST_EVENT_EOS:
		{   //从上游来的EOS消息，结束进程。
			g_print("probe eos event from upstream \n");
			g_main_loop_quit(loop);
			break;
		}
		default:
			break;
		}
		return GST_PAD_PROBE_PASS;
	}
	else
	{
		g_warning("probe not implemented\n");
		return GST_PAD_PROBE_DROP;
	}

}

/*ogg 播放组件*/
int createOggPlayer(int argc, char *argv[])
{
	GstElement *pipeline, *source, *demuxer, *decoder, *conv, *sink,*myfilter;
	GstBus *bus;
	guint bus_watch_id;
	GstPad *myfilter_srcpad,*filesinkpad;

	/* init */
	gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

	/* Create gstreamer elements */
	pipeline = gst_pipeline_new("audio-player");
	source = gst_element_factory_make("filesrc", "file-source");
	demuxer = gst_element_factory_make("oggdemux", "ogg-demuxer");
	myfilter = gst_element_factory_make("myfilter", "my-filter");
	decoder = gst_element_factory_make("vorbisdec", "vorbis-decoder");
	conv = gst_element_factory_make("audioconvert", "converter");
	//sink = gst_element_factory_make("autoaudiosink", "audio-output");
	sink = gst_element_factory_make("filesink", "file-sink");

	myfilter_srcpad = gst_element_get_static_pad(myfilter,"src");
	filesinkpad = gst_element_get_static_pad(sink, "sink");
	gst_pad_add_probe(/*filesinkpad*/myfilter_srcpad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, (GstPadProbeCallback)handle_myfilter_output, sink, NULL);

	if (!pipeline || !source || !demuxer || !decoder || !conv || !sink || !myfilter) {
		g_printerr("One element could not be created. Exiting.\n");
		return -1;
	}

	g_object_set(G_OBJECT(sink), "location", "sink_00.wav", NULL);
	g_object_set(G_OBJECT(source), "location", /*"D:\\stream.txt"*/"D:\\Alarm_Rooster_02.ogg", NULL);

	/* we add a message handler */
	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	bus_watch_id = gst_bus_add_watch(bus, my_bus_callback,NULL/*sink*/);
	gst_object_unref(bus);
	 

	/* we add all elements into the pipeline */
	/* file-source | ogg-demuxer | vorbis-decoder | converter | alsa-output */
	gst_bin_add_many(GST_BIN(pipeline),source, myfilter,demuxer, decoder, conv, sink, NULL);

	/* we link the elements together */
	/* file-source -> ogg-demuxer ~> vorbis-decoder -> converter -> alsa-output */
	gst_element_link(source, demuxer);
	gst_element_link_many(decoder, conv, myfilter, sink, NULL);

	g_signal_connect(demuxer, "pad-added", G_CALLBACK(cb_new_pad), decoder);
	//gst_pad_add_probe(myfilter_srcpad, GST_PAD_PROBE_TYPE_DATA_DOWNSTREAM, (GstPadProbeCallback)handle_myfilter_out, NULL, NULL);

	/* Set the pipeline to "playing" state*/
	g_print("Now playing:\n");
	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	/* Iterate */
	g_print("Running...\n");
	g_main_loop_run(loop);

	/* Out of the main loop, clean up nicely */
	g_print("Returned, stopping playback\n");
	gst_element_set_state(pipeline, GST_STATE_NULL);

	g_print("Deleting pipeline\n");
	gst_object_unref(GST_OBJECT(pipeline));
	g_source_remove(bus_watch_id);
	g_main_loop_unref(loop);
	return 0;
 
}

int createPad(int argc, char *argv[])
{
	GstElement *pipeline, *source, *demux;
	GMainLoop *loop;
	/* init */
	gst_init(&argc, &argv);
	/* create elements */
	pipeline = gst_pipeline_new("my_pipeline");
	source = gst_element_factory_make("filesrc", "source");
	g_object_set(source, "location", "D:\\Alarm_Rooster_02.ogg", NULL);
	demux = gst_element_factory_make("oggdemux", "demuxer");
	/* you would normally check that the elements were created properly */
	/* put together a pipeline */
	gst_bin_add_many(GST_BIN(pipeline), source, demux, NULL);
	gst_element_link_pads(source, "src", demux, "sink");
	/* listen for newly created pads */
	g_signal_connect(demux, "pad-added", G_CALLBACK(cb_new_pad), NULL);
	/* start the pipeline */
	gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);
	return 0;
}

/*使用tee和queqe进行流的复制和分发*/
int linkRequestPad(int argc, char *argv[])
{
	GstElement *pipeline, *audio_source, *tee, *audio_queue, *audio_convert, *audio_resample, *audio_sink;
	GstElement *video_queue, *visual, *video_convert, *video_sink;
	GstBus *bus;
	GstMessage *msg;
	GstPadTemplate *tee_src_pad_template;
	GstPad *tee_audio_pad, *tee_video_pad;
	GstPad *queue_audio_pad, *queue_video_pad;
	guint bus_watch_id;

	/* init */
	gst_init(&argc, &argv);
	loop = g_main_loop_new(NULL, FALSE);

	/* Create the elements */
	audio_source = gst_element_factory_make("audiotestsrc", "audio_source");
	tee = gst_element_factory_make("tee", "tee");
	audio_queue = gst_element_factory_make("queue", "audio_queue");
	audio_convert = gst_element_factory_make("audioconvert", "audio_convert");
	audio_resample = gst_element_factory_make("audioresample", "audio_resample");
	audio_sink = gst_element_factory_make("autoaudiosink", "audio_sink");
	video_queue = gst_element_factory_make("queue", "video_queue");
	visual = gst_element_factory_make("wavescope", "visual");
	video_convert = gst_element_factory_make("videoconvert", "csp");
	video_sink = gst_element_factory_make("autovideosink", "video_sink");

	/* Create the empty pipeline */
	pipeline = gst_pipeline_new("test-pipeline");

	if (!pipeline || !audio_source || !tee || !audio_queue || !audio_convert || !audio_resample || !audio_sink ||
		!video_queue || !visual || !video_convert || !video_sink) {
		g_printerr("Not all elements could be created.\n");
		return -1;
	}

	/* Configure elements */
	g_object_set(audio_source, "freq", 215.0f, NULL);
	g_object_set(visual, "shader", 0, "style", 3, NULL);

	/* Link all elements that can be automatically linked because they have "Always" pads */
	gst_bin_add_many(GST_BIN(pipeline), audio_source, tee, audio_queue, audio_convert, audio_resample, audio_sink,
		video_queue, visual, video_convert, video_sink, NULL);
	if (gst_element_link_many(audio_source, tee, NULL) != TRUE ||
		gst_element_link_many(audio_queue, audio_convert, audio_resample, audio_sink, NULL) != TRUE ||
		gst_element_link_many(video_queue, visual, video_convert, video_sink, NULL) != TRUE) {
		g_printerr("Elements could not be linked.\n");
		gst_object_unref(pipeline);
		return -1;
	}

	/* Manually link the Tee, which has "Request" pads */
	tee_src_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(tee), "src_%u");
	tee_audio_pad = gst_element_request_pad(tee, tee_src_pad_template, NULL, NULL);
	g_print("Obtained request pad %s for audio branch.\n", gst_pad_get_name(tee_audio_pad));
	queue_audio_pad = gst_element_get_static_pad(audio_queue, "sink");
	tee_video_pad = gst_element_request_pad(tee, tee_src_pad_template, NULL, NULL);
	g_print("Obtained request pad %s for video branch.\n", gst_pad_get_name(tee_video_pad));
	queue_video_pad = gst_element_get_static_pad(video_queue, "sink");
	if (gst_pad_link(tee_audio_pad, queue_audio_pad) != GST_PAD_LINK_OK ||
		gst_pad_link(tee_video_pad, queue_video_pad) != GST_PAD_LINK_OK) {
		g_printerr("Tee could not be linked.\n");
		gst_object_unref(pipeline);
		return -1;
	}
	gst_object_unref(queue_audio_pad);
	gst_object_unref(queue_video_pad);

	/* we add a message handler */
	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	bus_watch_id = gst_bus_add_watch(bus, my_bus_callback, NULL);
	gst_object_unref(bus);
	

	/* Set the pipeline to "playing" state*/
	g_print("Now playing:\n");
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
	msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,/*GST_MESSAGE_EOS | */GST_MESSAGE_ERROR);
	

	/* Iterate */
	g_print("Running...\n");
	/*g_main_loop_run(loop);*/

	/* Out of the main loop, clean up nicely */
	g_print("Returned, stopping playback\n");
	gst_element_set_state(pipeline, GST_STATE_NULL);

	/* Release the request pads from the Tee, and unref them */
	gst_element_release_request_pad(tee, tee_audio_pad);
	gst_element_release_request_pad(tee, tee_video_pad);
	gst_object_unref(tee_audio_pad);
	gst_object_unref(tee_video_pad);

	g_print("Deleting pipeline\n");
	gst_object_unref(GST_OBJECT(pipeline));
	g_source_remove(bus_watch_id);
	g_main_loop_unref(loop);
	return 0;

}

int loopBusMsg(int argc, char *argv[])
{
	GstElement *pipeline;
	GstBus *bus;
	guint bus_watch_id;

	/* init */
	gst_init(&argc, &argv);

	/* create pipeline, add handler */
	pipeline = gst_pipeline_new("my_pipeline");

	/* adds a watch for new message on our pipeline's message bus to
	* the default GLib main context, which is the main context that our
	* GLib main loop is attached to below
	*/
	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	bus_watch_id = gst_bus_add_watch(bus, my_bus_callback, NULL);
	gst_object_unref(bus);

	/* create a mainloop that runs/iterates the default GLib main context
	* (context NULL), in other words: makes the context check if anything
	* it watches for has happened. When a message has been posted on the
	* bus, the default main context will automatically call our
	* my_bus_callback() function to notify us of that message.
	* The main loop will be run until someone calls g_main_loop_quit()
	*/
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);

	/* clean up */
	printf("loop end!\n");
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(pipeline); 
	g_source_remove(bus_watch_id);
	g_main_loop_unref(loop);

	return 0;


}

int showVersion(int argc,char *argv[])
{
	const gchar *nano_str;
	guint major, minor, micro, nano;

	gst_init(&argc, &argv);

	gst_version(&major, &minor, &micro, &nano);

	if (nano == 1)
		nano_str = "(CVS)";
	else if (nano == 2)
		nano_str = "(Prerelease)";
	else
		nano_str = "";

	printf("This program is linked against GStreamer %d.%d.%d %s\n",
		major, minor, micro, nano_str);
	return 0;

}

int goption(int argc, char *argv[])
{
	gboolean silent = FALSE;
	gchar *savefile = NULL;
	GOptionContext *ctx;
	GError *err = NULL;
	GOptionEntry entries[] = {
		{ "silent", 's', 0, G_OPTION_ARG_NONE, &silent,
		"do not output status information", NULL },
		{ "output", 'o', 0, G_OPTION_ARG_STRING, &savefile,
		"save xml representation of pipeline to FILE and exit", "FILE" },
		{ NULL }
	};
	ctx = g_option_context_new("- Your application");
	g_option_context_add_main_entries(ctx, entries, NULL);
	g_option_context_add_group(ctx, gst_init_get_option_group());
	if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
		g_print("Failed to initialize: %s\n", err->message);
		g_error_free(err);
		return 1;
	}
	printf("Run me with --help to see the Application options appended.\n");
	return 0;
}

int gCreateGElement(int argc, char *argv[])
{
	GstElementFactory *factory;
	GstElement *element;
	gchar *name;

	/* init GStreamer */
	gst_init(&argc, &argv);

	/* create element, method #2 */
	factory = gst_element_factory_find("fakesrc");
	if (!factory) {
		g_print("Failed to find factory of type 'fakesrc'\n");
		return -1;
	}

	/* display information */
	g_print("The '%s' element is a member of the category %s.\n"
		"Description: %s\n",
		gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)),
		gst_element_factory_get_klass(factory),
		gst_element_factory_get_description(factory));


	element = gst_element_factory_create(factory, "source");
	if (!element) {
		g_print("Failed to create element, even though its factory exists!\n");
		return -1;
	}

	g_object_get(G_OBJECT(element), "name", &name, NULL);
	g_print("The name of the element is '%s'.\n", name);
	g_free(name);


	gst_object_unref(GST_OBJECT(element));

	return 0;
}

int linkElement(int argc, char *argv[])
{
	GstElement *pipeline;
	GstElement *source, *filter, *sink;

	/* init */
	gst_init(&argc, &argv);

	/* create pipeline */
	pipeline = gst_pipeline_new("my-pipeline");

	/* create elements */
	source = gst_element_factory_make("fakesrc", "source");
	filter = gst_element_factory_make("identity", "filter");
	sink = gst_element_factory_make("fakesink", "sink");

	/* must add elements to pipeline before linking them */
	gst_bin_add_many(GST_BIN(pipeline), source, filter, sink, NULL);

	/* link */
	if (!gst_element_link_many(source, filter, sink, NULL)) {
		g_warning("Failed to link elements!");
	}

	return 0;
}