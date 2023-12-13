//gcc --std=c99 -Wall $(pkg-config --cflags gstreamer-1.0) gstappsrc.cpp $(pkg-config --libs gstreamer-1.0) -lgstapp-1.0

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <stdint.h>

GMainLoop *loop = NULL;

#define BATCH 32

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
    GstElement *bin = NULL, *source = NULL, *jpegparse = NULL, *jpegdec = NULL;
    gchar bin_name[16] = { };
    gchar appsrc_name[16] = { };

    g_snprintf (bin_name, 15, "source-bin-%02d", index);
    bin = gst_bin_new (bin_name);

    g_snprintf (appsrc_name, 15, "appsrc-%02d", index);
    source = gst_element_factory_make ("appsrc", appsrc_name);

    jpegparse = gst_element_factory_make ("jpegparse", NULL);

    jpegdec = gst_element_factory_make ("nvjpegdec", NULL);

    if (!bin || !source || !jpegparse || !jpegdec)
    {
        g_printerr ("One element in source bin could not be created.\n");
        return NULL;
    }

    gst_bin_add_many (GST_BIN (bin), source, jpegparse, jpegdec, NULL);

    if (!gst_element_link_many (source, jpegparse, jpegdec, NULL))
    {
        g_printerr ("Failed to link source_bin elements\n");
        return NULL;
    }

    /* We need to create a ghost pad for the source bin which acts as a proxy for
     * the decoder src pad */
    GstPad *gstpad = gst_element_get_static_pad (jpegdec, "src");
    if (!gstpad)
    {
        g_printerr ("could not create pad for jpegdec");
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


gint main (gint argc, gchar *argv[])
{
    int i;
    GstBus *bus = NULL;
    guint bus_watch_id;
    GstElement *pipeline, *nvstreammux, *fakesink;
    gchar pad_name[16]={0};

    gst_init (&argc, &argv);
    loop = g_main_loop_new (NULL, FALSE);
    GstPad *sinkpad = NULL, *src_bin_pad = NULL;

    /* setup pipeline */
    pipeline = gst_pipeline_new ("pipeline");

    nvstreammux = gst_element_factory_make ("nvstreammux", NULL);
    g_object_set (G_OBJECT(nvstreammux), "width", 1280, NULL);
    g_object_set (G_OBJECT(nvstreammux), "height", 720, NULL);
    g_object_set (G_OBJECT(nvstreammux), "batch-size", BATCH, NULL);

    fakesink = gst_element_factory_make ("fakesink", NULL);

    gst_bin_add_many (GST_BIN (pipeline), nvstreammux, fakesink, NULL);
    gst_element_link_many (nvstreammux, fakesink, NULL);

    for (i = 0; i < BATCH; i++)
    {
        GstElement *source_bin = create_source_bin(i, argv[1]);
        if (!source_bin)
        {
            g_printerr ("Failed to create source bin. Exiting.\n");
            return -1;
        }
        gst_bin_add (GST_BIN (pipeline), source_bin);

        g_snprintf (pad_name, 15, "sink_%u", i);
        sinkpad = gst_element_request_pad_simple (nvstreammux, pad_name);

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
    }

    gst_element_set_state (pipeline, GST_STATE_PLAYING);

    const gchar *jpeg_file_path = "./sample_720p.jpeg";
    GError *error = NULL;
    gsize file_size;
    guint8 *jpeg_data;
    GstElement *appsrc = NULL;

    while (1)
    {
        for (i = 0; i < BATCH; i++)
        {
            gchar appsrc_name[16] = { };

            if (g_file_get_contents(jpeg_file_path, (gchar **)&jpeg_data, &file_size, &error))
            {
                g_snprintf (appsrc_name, 15, "appsrc-%02d", i);
                appsrc = gst_bin_get_by_name (GST_BIN(pipeline), appsrc_name);
                //g_print ("pushing buffer into pipeline for appsrc %s of size %ld\n", appsrc_name, file_size);
                GstBuffer *buffer = gst_buffer_new_wrapped(jpeg_data, file_size);
                gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);

            }
            else
            {
                g_printerr("Error reading JPEG file: %s\n", error->message);
                g_clear_error(&error);
            }
        }

        //g_usleep (5000000);
    }

    /* play */

    //g_usleep(20000000);
    //
    GST_DEBUG_BIN_TO_DOT_FILE (GST_BIN (pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "appsrc-app");
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
    gst_object_unref (bus);

    g_main_loop_run (loop);
    gst_object_unref (GST_OBJECT (pipeline));
    g_source_remove (bus_watch_id);
    g_main_loop_unref (loop);
    gst_element_set_state (pipeline, GST_STATE_NULL);

    return 0;
}
