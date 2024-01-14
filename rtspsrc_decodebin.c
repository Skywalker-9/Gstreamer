#include <gst/gst.h>


// Function to handle "pad-added" signal
static void rtspsrc_pad_added(GstElement *src, GstPad *new_pad, gpointer user_data)
{
    //g_print ("RTSPSRC new pad added caps = %s\n\n\n", gst_caps_to_string (caps));
    GstCaps *caps = gst_pad_query_caps (new_pad, NULL);
    const GstStructure *str = gst_caps_get_structure (caps, 0);
    const gchar *name = gst_structure_get_name (str);
    const gchar* media = gst_structure_get_string (str, "media");

    if (g_strrstr (name, "x-rtp") && !strcmp (media, "video"))
    {

        GstElement *decodebin = GST_ELEMENT(user_data);

        // Request a new pad from decodebin
        GstPad *sink_pad = gst_element_get_static_pad(decodebin, "sink");
        if (!sink_pad)
        {
            g_printerr("Failed to get request pad from decodebin.\n");
            return;
        }

        // Link the new pad to the sink pad of decodebin
        if (gst_pad_link(new_pad, sink_pad) != GST_PAD_LINK_OK)
        {
            g_printerr("Failed to link pads.\n");
        }
        else
        {
            g_print("Pads linked successfully.\n");
        }

        // Unreference the sink pad
        gst_object_unref(sink_pad);
    }
}

static void decodebin_pad_added(GstElement *src, GstPad *new_pad, gpointer user_data)
{

    GstCaps *caps = gst_pad_query_caps (new_pad, NULL);
    const GstStructure *str = gst_caps_get_structure (caps, 0);
    const gchar *name = gst_structure_get_name (str);

    if (!strncmp (name, "video", 5))
    {
        GstElement *sink = GST_ELEMENT(user_data);
        GstPad *sinkpad = gst_element_get_static_pad (sink, "sink");

        if (gst_pad_link(new_pad, sinkpad) != GST_PAD_LINK_OK)
        {
            g_print ("could not link decodebin src pad to videosink sink pad\n");
        }
    }
}

static GstPadProbeReturn
decoder_src_pad_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
    //g_print ("A buffer is output from decoder \n");
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
videosink_pad_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
    //g_print ("A buffer is received on nveglglessink pad \n");
    return GST_PAD_PROBE_OK;
}

static void
decodebin_child_added (GstChildProxy * child_proxy, GObject * object,
    gchar * name, gpointer user_data)
{
  GstElement *decodebin = GST_ELEMENT(user_data);
  if (g_strrstr (name, "decodebin") == name)
  {
    g_signal_connect (G_OBJECT (object), "child-added", G_CALLBACK (decodebin_child_added), user_data);
  }

  if ((g_strstr_len (name, -1, "h264parse") == name)  || (g_strstr_len (name, -1, "h265parse") == name))
  {
      g_print ("parser found\n");
      GstElement *parser = GST_ELEMENT(object);
      g_object_set(parser, "config-interval", -1, NULL);
  }
  
  if (g_strstr_len (name, -1, "nvv4l2decoder") == name)
  {
      g_print ("nvv4l2decoder found\n");
      GstElement *decoder = GST_ELEMENT(object);
      GstPad *decoder_src_pad = gst_element_get_static_pad (decoder, "src");
      gst_pad_add_probe(decoder_src_pad, GST_PAD_PROBE_TYPE_BUFFER, decoder_src_pad_probe, NULL, NULL);
  }
}


int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    if (argc != 2)
    {
        g_print ("Usage    ./application <URL>\n");
        exit (0);
    }

    const char *url = argv[1];

    GstElement *pipeline, *rtspsrc, *decodebin, *videosink;

    pipeline = gst_pipeline_new("rtspsrc-decodebin-pipeline");
    rtspsrc = gst_element_factory_make("rtspsrc", "rtspsrc");
    decodebin = gst_element_factory_make("decodebin", "decodebin");
    videosink = gst_element_factory_make("nveglglessink", "videosink");


    GstPad *videosink_pad = gst_element_get_static_pad (videosink, "sink");
    gst_pad_add_probe(videosink_pad, GST_PAD_PROBE_TYPE_BUFFER, videosink_pad_probe, NULL, NULL);

    g_object_set (videosink, "sync", 0, NULL);

    if (!pipeline || !rtspsrc || !decodebin || !videosink)
    {
        g_printerr("Not all elements could be created.\n");
        return -1;
    }

    gst_bin_add_many(GST_BIN(pipeline), rtspsrc, decodebin, videosink, NULL);

    g_signal_connect(rtspsrc, "pad-added", G_CALLBACK(rtspsrc_pad_added), decodebin);

    g_signal_connect(decodebin, "pad-added", G_CALLBACK(decodebin_pad_added), videosink);

    g_object_set(rtspsrc, "location", url, NULL);

    g_signal_connect (G_OBJECT (decodebin), "child-added", G_CALLBACK (decodebin_child_added), decodebin);

    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "pipeline");

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);

    return 0;
}

