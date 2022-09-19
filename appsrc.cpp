/*
 * g++ appsrc.cpp -g -fpermissive `pkg-config --libs --cflags gstreamer-1.0 gstreamer-app-1.0`
 * */

#include <gst/gst.h>
#include <string.h>
#include <iostream>
#include <gst/app/gstappsrc.h>

using namespace std;

#define BUFF_SIZE (6144) /* 6 KB */

typedef struct _AppContext
{
    GstElement *pipeline;

    GstElement *app_src;
    GstElement *demux;
    GstElement *h264parse;
    GstElement *nvv4l2decoder;
    GstElement *nveglglessink;

    GMainLoop *main_loop;

    guint8 *data_ptr;
    guint sourceid;

    FILE *file;
}AppContext;

static void
demux_newpad (GstElement *demux, GstPad *demux_src_pad, gpointer data)
{
  GstCaps *caps = gst_pad_get_current_caps (demux_src_pad);
  if (!caps)
  {
    caps = gst_pad_query_caps (demux_src_pad, NULL);
  }
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);

  if (!strncmp (name, "video", 5))
  {
      GstElement *parser = (GstElement *) data;
      GstPad *sinkpad = gst_element_get_static_pad (parser, "sink");
      if (gst_pad_link (demux_src_pad, sinkpad) != GST_PAD_LINK_OK)
      {
          printf ("failed to link demux SRC pad to video pipeline\n");
      }
      gst_object_unref(sinkpad);
      printf ("demux pad is linked to downstream video pipeline successfully\n");
  }
}


static gboolean read_data (AppContext *app)
{
    GstBuffer *buffer;
    gint size;
    GstFlowReturn ret;
    static GstClockTime timestamp = 0;

    size = fread(app->data_ptr, 1, BUFF_SIZE, app->file);

    if(size == 0)
    {
        ret = gst_app_src_end_of_stream((GstAppSrc *)app->app_src);
        g_print("eos returned %d at %d\n", ret, __LINE__);
        return FALSE;
    }

    buffer = gst_buffer_new_and_alloc (BUFF_SIZE);

    GST_BUFFER_PTS (buffer) = timestamp;
    GST_BUFFER_DURATION (buffer) = gst_util_uint64_scale_int (1, GST_SECOND, 4);
    timestamp += GST_BUFFER_DURATION (buffer);

    GstMapInfo map;

    gst_buffer_map (buffer, &map, GST_MAP_WRITE);
    memcpy (map.data, app->data_ptr, BUFF_SIZE);
    gst_buffer_unmap (buffer, &map);

    g_signal_emit_by_name (app->app_src, "push-buffer", buffer, &ret);

    gst_buffer_unref (buffer);

    if (ret != GST_FLOW_OK)
    {
        g_print ("push-buffer failed\n");
        return FALSE;
    }

    if(size != BUFF_SIZE)
    {
        ret = gst_app_src_end_of_stream((GstAppSrc *)app->app_src);
        g_print("eos returned %d at %d\n", ret, __LINE__);
        return FALSE;
    }

    return TRUE;
}

static void start_feed (GstElement * pipeline, guint size, AppContext *app)
{
    if (app->sourceid == 0)
    {
        GST_DEBUG ("start feeding");
        app->sourceid = g_idle_add ((GSourceFunc) read_data, app);
    }
}

static void stop_feed (GstElement * pipeline, AppContext *app)
{
    if (app->sourceid != 0)
    {
        GST_DEBUG ("stop feeding");
        g_source_remove (app->sourceid);
        app->sourceid = 0;
    }
}


static void error_cb (GstBus *bus, GstMessage *message, AppContext *app)
{
    switch(GST_MESSAGE_TYPE(message))
    {

        case GST_MESSAGE_ERROR:
            {
                gchar *debug;
                GError *err;

                gst_message_parse_error(message, &err, &debug);
                g_print("Error %s\n", err->message);
                g_error_free(err);
                g_free(debug);
                g_main_loop_quit(app->main_loop);
            }
            break;

        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(app->main_loop);
            break;

        default:
            g_print("got message %s\n", gst_message_type_get_name (GST_MESSAGE_TYPE (message)));
            break;
    }
}


int main (int argc, char *argv[])
{
    AppContext app;
    memset (&app, 0, sizeof(app));

    if (argc != 2)
    {
        g_print ("Usage : <application> <input_h264_elementary_stream>\n");
        return -1;
    }

    app.file = fopen (argv[1], "rb");
    g_assert (app.file);

    gst_init(NULL, NULL);

    GstBus *bus;


    app.data_ptr = (guint8 *) g_malloc0(BUFF_SIZE);

    app.app_src = gst_element_factory_make ("appsrc", "app_source");
    app.demux = gst_element_factory_make ("matroskademux", "demux");
    app.h264parse = gst_element_factory_make ("h264parse", "parser");
    app.nvv4l2decoder = gst_element_factory_make ("nvv4l2decoder", "decoder");
    app.nveglglessink = gst_element_factory_make ("nveglglessink", "sink");


    app.pipeline = gst_pipeline_new ("simple appsrc pipeline");

    g_signal_connect (app.app_src, "need-data", G_CALLBACK (start_feed), &app);
    g_signal_connect (app.app_src, "enough-data", G_CALLBACK (stop_feed), &app);

    g_object_set (G_OBJECT(app.nveglglessink), "sync", 0, NULL);

    if (!app.pipeline || ! app.app_src || !app.demux  || !app.h264parse || !app.nvv4l2decoder || !app.nveglglessink)
    {
        g_printerr ("Not all elements could be created.\n");
        return -1;
    }

    bus = gst_element_get_bus (app.pipeline);
    gst_bus_add_signal_watch (bus);
    g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, &app);
    gst_object_unref (bus);

    gst_bin_add_many (GST_BIN(app.pipeline), app.app_src, app.demux, app.h264parse, app.nvv4l2decoder, app.nveglglessink, NULL);

    g_signal_connect (app.demux, "pad-added", G_CALLBACK (demux_newpad), app.h264parse);

    if (gst_element_link_many (app.app_src, app.demux, NULL) != TRUE)
    {
        g_printerr ("Failed to link elements in the pipeline\n");
        gst_object_unref (app.pipeline);
        return -1;
    }

    if (gst_element_link_many (app.h264parse, app.nvv4l2decoder, app.nveglglessink, NULL) != TRUE)
    {
        g_printerr ("Failed to link elements in the pipeline\n");
        gst_object_unref (app.pipeline);
        return -1;
    }

    gst_element_set_state (app.pipeline, GST_STATE_PLAYING);

    app.main_loop = g_main_loop_new (NULL, FALSE);
    g_main_loop_run (app.main_loop);

    fclose (app.file);
    g_free (app.data_ptr);
    gst_element_set_state (app.pipeline, GST_STATE_NULL);
    g_main_loop_unref (app.main_loop);
    gst_object_unref (app.pipeline);
}
