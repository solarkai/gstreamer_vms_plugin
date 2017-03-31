/*
 * GStreamer
 * Copyright (C) 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright (C) 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * Copyright (C) 2016 ubuntu <<user@hostname.org>>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-h264filter
 *
 * FIXME:Describe h264filter here.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v -m fakesrc ! h264filter ! fakesink silent=TRUE
 * ]|
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>

#include "gsth264filter.h"

GST_DEBUG_CATEGORY_STATIC (gst_h264filter_debug);
#define GST_CAT_DEFAULT gst_h264filter_debug

#define SRC_PAD_INFO_LEN sizeof(SrcPadInfo)
#define NAL_TYPE_IS_KEY(nt) (((nt) == 5) /*|| ((nt) == 7) || ((nt) == 8)*/)

/* Filter signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_SILENT
};

/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
	GST_STATIC_CAPS /*("ANY")*/(
	"video/x-h264, "
	"stream-format = (string) avc, alignment = (string) au; "
	)
);


static GstStaticPadTemplate h264src_factory =GST_STATIC_PAD_TEMPLATE("h264src_%u",
	GST_PAD_SRC,
	GST_PAD_REQUEST,
	GST_STATIC_CAPS("ANY")
);

#define gst_h264filter_parent_class parent_class
G_DEFINE_TYPE (GstH264Filter, gst_h264filter, GST_TYPE_ELEMENT);

static void gst_h264filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_h264filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_h264filter_sink_event (GstPad * pad, GstObject * parent, GstEvent * event);
static GstFlowReturn gst_h264filter_chain (GstPad * pad, GstObject * parent, GstBuffer * buf);

static GstPad * gst_h264filter_request_new_pad(GstElement *element, GstPadTemplate *templ, const gchar *name, const GstCaps *caps);
static void gst_h264filter_release_pad(GstElement *element, GstPad *pad);

/* GObject vmethod implementations */

/* initialize the h264filter's class */
static void
gst_h264filter_class_init (GstH264FilterClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_h264filter_set_property;
  gobject_class->get_property = gst_h264filter_get_property;
  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
          FALSE, G_PARAM_READWRITE));

  gst_element_class_set_details_simple(gstelement_class,
    "H264Filter",
    "H264 stream filter",
    "H264 stream filter,flust the GOP.",
    "kedacom <<zhang.kai@kedacom.com>>");

  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&sink_factory));
  gst_element_class_add_pad_template(gstelement_class,gst_static_pad_template_get(&h264src_factory));
  gstelement_class->request_new_pad = GST_DEBUG_FUNCPTR(gst_h264filter_request_new_pad);
  gstelement_class->release_pad = GST_DEBUG_FUNCPTR(gst_h264filter_release_pad);

  
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
static void
gst_h264filter_init (GstH264Filter * filter)
{
  filter->sinkpad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_event_function (filter->sinkpad,GST_DEBUG_FUNCPTR(gst_h264filter_sink_event));
  gst_pad_set_chain_function (filter->sinkpad,GST_DEBUG_FUNCPTR(gst_h264filter_chain));
  GST_PAD_SET_PROXY_CAPS (filter->sinkpad);
  gst_element_add_pad (GST_ELEMENT (filter), filter->sinkpad);

  filter->silent = FALSE;
  filter->num_streams = 0;
  filter->padset = gst_structure_new("SrcPadSet", NULL);
  filter->keyframebuflist = gst_buffer_list_new();
  
}

static void
gst_h264filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstH264Filter *filter = GST_H264FILTER (object);

  switch (prop_id) {
    case PROP_SILENT:
      filter->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_h264filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstH264Filter *filter = GST_H264FILTER (object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, filter->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstElement vmethod implementations */

static gboolean print_field(GQuark field, const GValue * value, gpointer filter) {
	gchar *str = gst_value_serialize(value);
    GST_DEBUG_OBJECT(filter,"%15s: %s\n", g_quark_to_string(field), str);
	g_free(str);
	return TRUE;
}

/*调试函数，打印pad的caps*/
static void printCaps(GstH264Filter *filter, GstCaps *caps)
{
	for (int i = 0; i < gst_caps_get_size(caps); i++){
		GstStructure *ptStru = gst_caps_get_structure(caps, i);
		const gchar *ptStru_Name = gst_structure_get_name(ptStru);

		GST_DEBUG_OBJECT(filter,"***************%s pad the %dth cap is: %s \n", ptStru_Name, i, ptStru_Name);
		gst_structure_foreach(ptStru, print_field, filter);
	}
}

static gboolean
gst_h264filter_src_query(GstPad * pad, GstObject * parent, GstQuery * query)
{
	GstH264Filter *h264filter ;
	gboolean res;
	GstPad *sinkpad;

	h264filter = GST_H264FILTER(parent);

	res = gst_pad_query_default(pad, parent, query);
	return res;

}

/*请求一个新的h264src pad*/
static GstPad * gst_h264filter_request_new_pad(GstElement *element, GstPadTemplate *templ, const gchar *name, const GstCaps *caps)
{
	GstPad *pad;
	GstElementClass *klass = GST_ELEMENT_GET_CLASS(element);
	GstH264Filter *h264filter = GST_H264FILTER(element);
	GValue value;
	GstH264FilterInputContext *context;

	GST_OBJECT_LOCK(h264filter);
	/*这里忽略了输入的name参数，应该要判断一下是否请求的pad已经存在*/
	SrcPadInfo *padinfo = g_new0(SrcPadInfo, 1);
	gchar *padName = g_strdup_printf("h264src_%u", h264filter->num_streams++);

	context = g_new0(GstH264FilterInputContext, 1);
	context->isrecieved = FALSE;
	pad = gst_pad_new_from_template(templ, padName);
	gst_pad_set_element_private(pad, context);

	/*将生成的pad放入结构体padinfo中并放入集合结构中*/
	padinfo->isrecieved = FALSE;
	padinfo->padid = h264filter->num_streams;
	padinfo->srcpad = pad;
	gst_structure_set(h264filter->padset, padName, G_TYPE_POINTER, (gpointer)padinfo, NULL);
	GST_OBJECT_UNLOCK(h264filter);

	gst_pad_activate_mode(pad, GST_PAD_MODE_PUSH, TRUE);
	gst_pad_set_query_function(pad, GST_DEBUG_FUNCPTR(gst_h264filter_src_query));
	GST_OBJECT_FLAG_SET(pad, GST_PAD_FLAG_PROXY_CAPS);

	if (!gst_element_add_pad(element, GST_PAD(pad)))
	{
		GST_ERROR_OBJECT(h264filter, "gst_h264filter_request_new_pad fail!");
		return NULL;
	}
	else
	{
		return pad;
	}
	
}


static void gst_h264filter_release_pad(GstElement *element, GstPad *pad)
{
	SrcPadInfo *padinfo;
	gchar *padname = gst_pad_get_name(pad);
	GstH264Filter *h264filter = GST_H264FILTER(element);

	if (gst_structure_has_field(h264filter->padset, padname))
	{
		gst_structure_remove_field(h264filter->padset, padname);
	}
	g_free(gst_pad_get_element_private(pad)); //free context
	gst_element_remove_pad(element,pad);

}


/* 与上游的caps进行协商 */
static gboolean
h264filter_negotiate_caps(GstH264Filter *filter,GstStructure *ptStru)
{
	gboolean ret =TRUE;
	const gchar *str = NULL;

	if ((ptStru) && (0 == strcmp("video/x-h264", gst_structure_get_name(ptStru))))
	{
		
		if (gst_structure_has_field(ptStru, "stream-format"))
		{
			str = gst_structure_get_string(ptStru, "stream-format");
			if (0 != strcmp(str, "avc"))
			{
				ret = FALSE;
			}
		}
		else
		{
			ret = FALSE;
		}
		if (gst_structure_has_field(ptStru, "alignment"))
		{
			str = gst_structure_get_string(ptStru, "alignment");
			if (0 != strcmp(str, "au"))
			{
				ret = FALSE;
			}
		}
		else
		{
			ret = FALSE;
		}
	}
	else
	{
		ret = FALSE;
	}

	return ret;
  
}

/* sink pad 的接收事件 */
static gboolean
gst_h264filter_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstH264Filter *filter;
  gboolean ret;
  GstSegment gsegment;
  GstCaps *caps;

  filter = GST_H264FILTER (parent);

  GST_DEBUG_OBJECT (filter, "####h264filter_sink_event Received %s event: %" GST_PTR_FORMAT,
      GST_EVENT_TYPE_NAME (event), event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
	  gst_event_parse_caps(event, &caps);
	  printCaps(filter, caps);
	  GstStructure *ptStru = gst_caps_get_structure(caps, 0);
	  ret = h264filter_negotiate_caps(filter, ptStru);
	  if (ret)
		  { ret = gst_pad_event_default(pad, parent, event);
	        filter->sinkcaps = caps;
		  }
	  else
	  {
		  GST_DEBUG_OBJECT(filter, "####h264filter sink caps negotiate fail! please check caps of element upstreams.");
		  return ret;
	  }
      break;
    }
	case GST_EVENT_SEGMENT:{
		ret = gst_pad_event_default(pad, parent, event);
		break;
	}
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

/*分析从buffer中获取的帧信息*/
static gint parseBufferFrameType(GstH264Filter *filter, GstBuffer *buf)
{
	GstMapInfo map;
	gst_buffer_map(buf, &map, GST_MAP_READ);

	//GST_DEBUG_OBJECT(filter, "==============Gstbuffsize:tsize:%u,memorys:%u ", gst_buffer_get_size(buf), gst_buffer_n_memory(buf));

	//从rtph264depay出来的buffer数据中，前4字节为帧的长度（大字节序），后面为NALU的数据。
	return map.data[4] & 0x1f;
}

/*发送单个buffer到srcpad*/
static GstFlowReturn pushbuf2pad(GstBuffer * buf, GstPad *srcpad)
{
	GstFlowReturn ret = gst_pad_push(srcpad, gst_buffer_ref(buf));
	return ret;
}

/*发送单个buffer到srcpad*/
static GstFlowReturn pushbuflist2pad(GstBufferList * buflist, GstPad *srcpad)
{
	//为保证buflist中的buf为多个pad所用，增加bufferlist引用。增加引用会引起gst_buffer_list_is_writable (list)的断言失败。
	GstFlowReturn ret = gst_pad_push_list(srcpad, gst_buffer_list_ref(buflist));
	return ret;
}

static gboolean senddata2srcpad(GstH264Filter *filter, GstBuffer * buf, gint buftype, GstH264FilterInputContext *context, GstPad *srcpad)
{
	guint gopbuflistsize = gst_buffer_list_length(filter->keyframebuflist);
	if (!context->isrecieved) //第一个关键帧还没有发送
	{
		if (gopbuflistsize> 0) //已经缓存了关键帧打头GOP
		{   
			GST_DEBUG_OBJECT(filter, "H264filter send buflist for pad:%s,there has %u frames.", gst_pad_get_name(srcpad), gopbuflistsize);
			pushbuflist2pad(filter->keyframebuflist, srcpad);
			pushbuf2pad(buf, srcpad);
			context->isrecieved = TRUE;
		}
		else
		{
			if (!NAL_TYPE_IS_KEY(buftype)) //没有发过GOP，当前包又不是关键帧，丢弃
				{return FALSE;
				}
			else
				{pushbuf2pad(buf, srcpad);
				}
			if (buftype == 5) //是IDR帧，发送并置位context->isrecieved为true
				{context->isrecieved = TRUE;
				}
		}
		
	}
	else
	{ //已经发送了关键帧，收到包转发即可
		pushbuf2pad(buf, srcpad);
	}
	return TRUE;
}

static void handlesinkbuf(GstH264Filter *filter, GstBuffer * buf, gint buftype)
{
	guint buflistlen = gst_buffer_list_length(filter->keyframebuflist);
	if (buflistlen > 0)
	{
		if (buftype == 5)//为IDR帧
		{   //之前buffer列表中的GOP被销毁
			gst_buffer_list_remove(filter->keyframebuflist, 0, buflistlen);
		}
		gst_buffer_list_add(filter->keyframebuflist, buf);
	}
	else
	{
		if (buftype == 5)//为IDR帧
		{
		  gst_buffer_list_add(filter->keyframebuflist,buf);
		}
	}
}


/* chain function
 * this function does the actual processing
 */
static GstFlowReturn gst_h264filter_chain(GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstH264Filter *filter;
  GstH264FilterInputContext *context;
  GstFlowReturn ret = GST_FLOW_OK;
  gint type;

  filter = GST_H264FILTER (parent);
  GstClock *clock = gst_element_get_clock(filter);
  GstClockTime  startclock = gst_clock_get_time(clock);

  /*分析buffer的264帧类型*/
  type = parseBufferFrameType(filter,buf);

  GList *padlist = GST_ELEMENT_PADS(parent);
  guint  listlen = g_list_length(padlist);

  /*这里第一个pad为sink pad,跳过去*/
  for (guint i = 1; i < listlen; i++)
  {
	  //GST_OBJECT_LOCK(filter);
	  GstPad *srcpad = GST_PAD(g_list_nth_data(padlist,i));
	  if (srcpad)
	  {
		  GstCaps *curcaps = gst_pad_get_current_caps(srcpad);
		  //printCaps(filter,curcaps);
		  context = (GstH264FilterInputContext *)gst_pad_get_element_private(srcpad);
		  if (context)
		  {
			  gboolean sendret = senddata2srcpad(filter, buf, type, context, srcpad);
			  if (!sendret)
			  {//没有发送到src pad的buffer,销毁退出
				  gst_buffer_unref(buf);
				  return ret;
			  }
		  }
		  else
		  {
			  GST_DEBUG_OBJECT(filter, "h264filter get %uth pad's context fail!", i);
			  ret = GST_FLOW_ERROR;
			  break;
		  }
	  }
	  else
	  {
		  GST_DEBUG_OBJECT(filter, "h264filter get %uth pad fail!", i);
		  ret = GST_FLOW_ERROR;
		  break;
	  }
  }
  handlesinkbuf(filter, buf, type);

  GstClockTime  endclock = gst_clock_get_time(clock);
  GST_DEBUG_OBJECT(filter, "----one chain collapsed time %u", endclock - startclock);

  return ret;
}


/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean
h264filter_init (GstPlugin * h264filter)
{
  /* debug category for fltering log messages
   *
   * exchange the string 'Template h264filter' with your description
   */
  GST_DEBUG_CATEGORY_INIT (gst_h264filter_debug, "h264filter",
      0, "h264filter");

  return gst_element_register (h264filter, "h264filter", GST_RANK_NONE,
      GST_TYPE_H264FILTER);
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "h264filter"
#endif

/* gstreamer looks for this structure to register h264filters
 *
 * exchange the string 'Template h264filter' with your h264filter description
 */
GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    h264filter,
    "h264filter",
    h264filter_init,
    "1.0"/*VERSION*/,
    "LGPL",
    "GStreamer",
    "http://gstreamer.net/"
)
