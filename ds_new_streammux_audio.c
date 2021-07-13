/*gcc ds_new_streammux_audio.c `pkg-config --cflags --libs gstreamer-1.0` -g*/

#include <stdlib.h>
#include <gst/gst.h>
#include <glib.h>
#include <math.h>
#include <gmodule.h>
#include <string.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>

#define GPU_ID 0
#define SET_GPU_ID(object, gpu_id) g_object_set (G_OBJECT (object), "gpu-id", gpu_id, NULL);
#define SINK_ELEMENT "fakesink"
#define MUXER_OUTPUT_WIDTH 1280
#define MUXER_OUTPUT_HEIGHT 720
#define USE_DEMUX
#define USE_FILESINK  //USE_DEMUX should be defined for correct output

GMainLoop *loop = NULL;
GstElement **g_source_bin_list = NULL;

GstElement *pipeline = NULL, *streammux = NULL, *sink = NULL, *identity = NULL, *streamdemux = NULL;
gint g_num_sources = 0;
gchar *uri = NULL;
guint ret_value = 0;

static gboolean bus_call (GstBus * bus, GstMessage * msg, gpointer data)
{
    GMainLoop *loop = (GMainLoop *) data;
    switch (GST_MESSAGE_TYPE (msg)) {
        case GST_MESSAGE_EOS:
            g_print ("End of stream\n");
            g_main_loop_quit (loop);
            break;
        case GST_MESSAGE_WARNING:
            {
                gchar *debug;
                GError *error;
                gst_message_parse_warning (msg, &error, &debug);
                g_printerr ("WARNING from element %s: %s\n",
                        GST_OBJECT_NAME (msg->src), error->message);
                g_free (debug);
                g_printerr ("Warning: %s\n", error->message);
                g_error_free (error);
                break;
            }
        case GST_MESSAGE_ERROR:
            {
                gchar *debug;
                GError *error;
                gst_message_parse_error (msg, &error, &debug);
                g_printerr ("ERROR from element %s: %s\n",
                        GST_OBJECT_NAME (msg->src), error->message);
                if (debug)
                    g_printerr ("Error details: %s\n", debug);
                g_free (debug);
                g_error_free (error);
                g_main_loop_quit (loop);
                break;
            }
        default:
            break;
    }
    return TRUE;
}

static GstElement *create_source_bin (guint index, gchar *filename)
{
    GstElement *bin = NULL, *source = NULL, *wavparse = NULL;
    gchar bin_name[16] = { };

    g_snprintf (bin_name, 15, "source-bin-%02d", index);
    bin = gst_bin_new (bin_name);

    source = gst_element_factory_make ("filesrc", NULL);

    wavparse = gst_element_factory_make ("wavparse", NULL);

    if (!bin || !source || !wavparse)
    {
        g_printerr ("One element in source bin could not be created.\n");
        return NULL;
    }

    /* We set the input filename to the source element */
    g_object_set (G_OBJECT (source), "location", filename, NULL);

    gst_bin_add_many (GST_BIN (bin), source, wavparse, NULL);

    if (!gst_element_link_many (source, wavparse, NULL))
    {
        g_printerr ("Failed to link source_bin elements\n");
        return NULL;
    }

    /* We need to create a ghost pad for the source bin which acts as a proxy for
     * the decoder src pad */
    GstPad *gstpad = gst_element_get_static_pad (wavparse, "src");
    if (!gstpad)
    {
        g_printerr ("could not create pad for wavparse");
        return NULL;
    }

    if (!gst_element_add_pad (bin, gst_ghost_pad_new ("src", gstpad)))
    {
        g_printerr ("Failed to add ghost pad in source bin\n");
        return NULL;
    }
    gst_object_unref (gstpad);

    return bin;
}

static gboolean add_sources(gpointer data);
static gboolean add_sources(gpointer data)
{
    gint source_id = g_num_sources;
    GstElement *source_bin;
    GstStateChangeReturn state_return;
    gchar pad_name[16]={0};
    gchar fname[16] = {0};
    GstPad *sinkpad = NULL, *srcpad = NULL, *wav_sinkpad = NULL;
    GstPad *src_bin_pad = NULL;
        
    GstElement *wavenc = NULL, *sink = NULL;
#ifdef USE_DEMUX
    wavenc = gst_element_factory_make ("wavenc", NULL);
#else
    wavenc = gst_element_factory_make ("identity", NULL);
#endif

#ifdef USE_FILESINK
    sink = gst_element_factory_make ("filesink", NULL);
#else
    sink = gst_element_factory_make ("fakesink", NULL);
#endif

    if (!wavenc || !sink)
    {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }
    g_object_set (G_OBJECT(sink), "async", 0, NULL);
#ifdef USE_FILESINK
    g_snprintf (fname, 15, "temp_%d.wav", source_id);
    g_object_set (G_OBJECT(sink), "location", fname, NULL);
#endif

    if (g_num_sources > 2)
    {
        g_source_remove (ret_value);
        return TRUE;
    }

    g_print ("Adding Source %d \n", source_id);
    source_bin = create_source_bin(source_id, uri);
    if (!source_bin)
    {
        g_printerr ("Failed to create source bin. Exiting.\n");
        return -1;
    }
    g_source_bin_list[source_id] = source_bin;
    gst_bin_add (GST_BIN (pipeline), source_bin);
    gst_bin_add_many (GST_BIN (pipeline), wavenc, sink, NULL);

    g_snprintf (pad_name, 15, "sink_%u", source_id);
    sinkpad = gst_element_get_request_pad (streammux, pad_name);

    src_bin_pad = gst_element_get_static_pad (source_bin, "src");
    if (!src_bin_pad)
    {
        g_printerr ("Could not get src pad or source bin");
    }

    if (gst_pad_link (src_bin_pad, sinkpad) != GST_PAD_LINK_OK)
    {
        g_print ("Failed to link source bin to pipeline\n");
    }
    else
    {
        g_print("source bin linked to pipeline\n");
    }

    g_snprintf (pad_name, 15, "src_%u", source_id);
    srcpad = gst_element_get_request_pad (streamdemux, pad_name);

    wav_sinkpad = gst_element_get_static_pad (wavenc, "sink");
    if (!wav_sinkpad)
    {
        g_printerr ("Could not get sink pad of wavenc");
    }

    if (gst_pad_link (srcpad, wav_sinkpad) != GST_PAD_LINK_OK)
    {
        g_print ("Failed to link demux src pad to wav sink pad\n");
    }
    else
    {
        g_print("demux srcpad linked to wavenc sink pad\n");
    }

    if (!gst_element_link (wavenc, sink))
    {
        g_printerr ("Failed to link wavenc to sink\n");
        return FALSE;
    }

    state_return = gst_element_set_state(g_source_bin_list[source_id], GST_STATE_PLAYING);
    switch (state_return)
    {
        case GST_STATE_CHANGE_SUCCESS:
            g_print ("STATE CHANGE SUCCESS\n\n");
            break;
        case GST_STATE_CHANGE_FAILURE:
            g_print ("STATE CHANGE FAILURE\n\n");
            break;
        case GST_STATE_CHANGE_ASYNC:
            g_print ("STATE CHANGE ASYNC\n\n");
            state_return = gst_element_get_state (g_source_bin_list[source_id], NULL, NULL, GST_CLOCK_TIME_NONE);
            break;
        case GST_STATE_CHANGE_NO_PREROLL:
            g_print ("STATE CHANGE NO PREROLL\n\n");
            break;
        default:
            break;
    }
    g_num_sources++;
    return TRUE;
}


int main (int argc, char *argv[])
{
    GstBus *bus = NULL;
    guint bus_watch_id;
    gulong tiler_probe_id = 0;
    GstPad *tiler_src_pad = NULL;
    guint i, num_sources;
    guint tiler_rows, tiler_columns;
    guint pgie_batch_size;
    GstElement *wavenc = NULL, *sink = NULL;

    g_setenv ("USE_NEW_NVSTREAMMUX", "yes", TRUE);

    /* Check input arguments */
    if (argc != 3 )
    {
        g_printerr ("Usage: %s <uri1> <num_sources>\n", argv[0]);
        return -1;
    }
    num_sources = atoi(argv[2]);

    /* Standard GStreamer initialization */
    gst_init (&argc, &argv);
    loop = g_main_loop_new (NULL, FALSE);

    /* Create gstreamer elements */
    /* Create Pipeline element that will form a connection of other elements */
    pipeline = gst_pipeline_new ("dstest-pipeline");

    /* Use nvinfer to run inferencing on decoder's output,
     * behaviour of inferencing is set through config file */
    streammux = gst_element_factory_make ("nvstreammux", "stream-muxer");
    //g_object_set(G_OBJECT(streammux), "batched-push-timeout", 25000, NULL);
    //g_object_set(G_OBJECT(streammux), "batched-push-timeout", 33333, NULL);
    //g_object_set(G_OBJECT(streammux), "batch-size", 200, NULL);
    //SET_GPU_ID (streammux, GPU_ID);

#ifdef USE_DEMUX
    streamdemux = gst_element_factory_make ("nvstreamdemux", "stream-demuxer");
#else
    streamdemux = gst_element_factory_make ("tee", "stream-demuxer");
#endif

    if (!pipeline || !streammux || !streamdemux)
    {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    g_object_set (G_OBJECT(streammux), "sync-inputs", 1, NULL);

    gst_bin_add_many (GST_BIN (pipeline), streammux, streamdemux, NULL);
    //g_object_set(G_OBJECT(streammux), "live-source", 1, NULL);
    if (!gst_element_link (streammux, streamdemux))
    {
        g_printerr ("could not link streammux and streamdemux\n");
        return FALSE;
    }

    g_source_bin_list = g_malloc0 (sizeof (GstElement*)*num_sources*200);
    uri = g_strdup (argv[1]);

    gchar pad_name[16]={0};
    gchar fname[16] = {0};
    GstPad *sinkpad = NULL, *srcpad = NULL, *wav_sinkpad = NULL;
    GstPad *src_bin_pad = NULL;

    for (i = 0; i < num_sources; i++)
    {
        GstElement *source_bin = create_source_bin(i, argv[1]);
        if (!source_bin)
        {
            g_printerr ("Failed to create source bin. Exiting.\n");
            return -1;
        }
        g_source_bin_list[i] = source_bin;
        gst_bin_add (GST_BIN (pipeline), source_bin);

        g_snprintf (pad_name, 15, "sink_%u", i);
        sinkpad = gst_element_get_request_pad (streammux, pad_name);

        src_bin_pad = gst_element_get_static_pad (source_bin, "src");
        if (!src_bin_pad)
        {
            g_printerr ("Could not get src pad or source bin");
        }

        if (gst_pad_link (src_bin_pad, sinkpad) != GST_PAD_LINK_OK)
        {
            g_print ("Failed to link source bin to pipeline\n");
        }
        else
        {
            g_print("source bin linked to pipeline\n");
        }

#ifdef USE_DEMUX
        wavenc = gst_element_factory_make ("wavenc", NULL);
#else
        wavenc = gst_element_factory_make ("identity", NULL);
#endif
#ifdef USE_FILESINK
        sink = gst_element_factory_make ("filesink", NULL);
#else
        sink = gst_element_factory_make ("fakesink", NULL);
#endif
        gst_bin_add_many (GST_BIN (pipeline), wavenc, sink, NULL);

        if (!wavenc || !sink)
        {
            g_printerr ("One element could not be created. Exiting.\n");
            return -1;
        }
        g_object_set (G_OBJECT(sink), "async", 0, NULL);
#ifdef USE_FILESINK
        g_snprintf (fname, 15, "temp_%d.wav", i);
        g_object_set (G_OBJECT(sink), "location", fname, NULL);
#endif

        g_snprintf (pad_name, 15, "src_%u", i);
        srcpad = gst_element_get_request_pad (streamdemux, pad_name);
        if (!srcpad)
        {
            g_printerr ("Could not get src pad of streamdemux");
        }

        wav_sinkpad = gst_element_get_static_pad (wavenc, "sink");
        if (!wav_sinkpad)
        {
            g_printerr ("Could not get sink pad of wavenc");
        }

        if (gst_pad_link (srcpad, wav_sinkpad) != GST_PAD_LINK_OK)
        {
            g_print ("Failed to link demux src pad to wav sink pad\n");
        }
        else
        {
            g_print("demux srcpad linked to wavenc sink pad\n");
        }

        if (!gst_element_link (wavenc, sink))
        {
            g_printerr ("Failed to link wavenc to sink\n");
            return -1;
        }

    }

    g_num_sources = num_sources;

    GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-playing");
    /* we add a message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
    gst_object_unref (bus);

    gst_element_set_state (pipeline, GST_STATE_PAUSED);
    /* Set the pipeline to "playing" state */
    //g_usleep(100000);
    g_print ("Now playing: %s\n", argv[1]);
    if (gst_element_set_state (pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr ("Failed to set pipeline to playing. Exiting.\n");
        return -1;
    }


    /* Wait till pipeline encounters an error or EOS */
    g_print ("Running...\n");
    ret_value = g_timeout_add (10, add_sources, (gpointer)g_source_bin_list);
    g_main_loop_run (loop);

    /* Out of the main loop, clean up nicely */
    g_print ("Returned, stopping playback\n");
    gst_element_set_state (pipeline, GST_STATE_NULL);
    g_print ("Deleting pipeline\n");
    gst_object_unref (GST_OBJECT (pipeline));
    g_source_remove (bus_watch_id);
    g_main_loop_unref (loop);
    g_free (g_source_bin_list);
    g_free (uri);

    return 0;
}
