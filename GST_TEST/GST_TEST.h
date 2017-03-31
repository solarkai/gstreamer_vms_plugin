#ifndef GST_TEST_H
#define GST_TEST_H

#include <gst/gst.h>


int showVersion(int argc, char *argv[]);
int goption(int argc, char *argv[]);
int gCreateGElement(int argc, char *argv[]);
int linkElement(int argc, char *argv[]);
int loopBusMsg(int argc, char *argv[]);
int createPad(int argc, char *argv[]);
int linkRequestPad(int argc, char *argv[]);
int createOggPlayer(int argc, char *argv[]);
int getRtspStream(int argc, char *argv[]);
int appsrcsinktestMain(int argc, char *argv[]);
int testH264filterMuti(int argc, char *argv[]);
int recordRtspStream(int argc, char *argv[]);

#endif // GST_TEST_H