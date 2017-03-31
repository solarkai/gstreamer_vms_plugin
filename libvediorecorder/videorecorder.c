#include <gst/gst.h>
#include "videorecordermux.h"

#ifndef PACKAGE
#define PACKAGE "videorecorder"
#endif

static gboolean
plugin_init(GstPlugin * plugin)
{
	if (!register_videorecordermux(plugin))
		return FALSE;

	if (!register_mymatroskamux(plugin))
		return FALSE;

	return TRUE;
}

GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	videorecorder,
	"record video stream to files and replay them.",
	plugin_init, 
	"1.0", 
	"LGPL", 
	"GStreamer",
	"http://gstreamer.net/"
	)
