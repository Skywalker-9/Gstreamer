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
#include "gstnvdsmeta.h"
#include "gst-nvmessage.h"
#include "nvdsmeta.h"
#include "nvdstilerconfig.h"

#define GPU_ID 0
#define SET_GPU_ID(object, gpu_id) g_object_set (G_OBJECT (object), "gpu-id", gpu_id, NULL);
#define SINK_ELEMENT "fakesink"
#define MUXER_OUTPUT_WIDTH 1280
#define MUXER_OUTPUT_HEIGHT 720


#define NVGSTDS_ELEM_ADD_PROBE(probe_id, elem, pad, probe_func, probe_type, probe_data) \
    do { \
        GstPad *gstpad = gst_element_get_static_pad (elem, pad); \
        if (!gstpad) { \
            g_print ("Could not find '%s' in '%s'", pad, \
                    GST_ELEMENT_NAME(elem)); \
        } \
        probe_id = gst_pad_add_probe(gstpad, (probe_type), probe_func, probe_data, NULL); \
        gst_object_unref (gstpad); \
    } while (0)

static gboolean seek_decode (gpointer data)
{
    GstElement *bin = (GstElement *) data;
    gboolean ret = TRUE;

    gst_element_set_state (bin, GST_STATE_PAUSED);

    //g_print ("seeking to start %s\n", GST_ELEMENT_NAME(bin));
    ret = gst_element_seek (bin, 1.0, GST_FORMAT_TIME,
            (GstSeekFlags) (GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_FLUSH),
            GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);

    if (!ret)
        GST_WARNING ("Error in seeking pipeline");

    gst_element_set_state (bin, GST_STATE_PLAYING);

    return FALSE;
}

typedef struct _decoder_data
{
    guint64 prev_accumulated_base;
    guint64 accumulated_base;
    GstElement *decoder;
}decoder_data;

void init_decoder_data (decoder_data *dec_data, GstElement *decoder)
{
    dec_data->prev_accumulated_base = 0;
    dec_data->accumulated_base = 0;
    dec_data->decoder = decoder;
}

static GstPadProbeReturn restart_stream_buf_prob (GstPad * pad, GstPadProbeInfo * info, gpointer u_data)
{
  GstEvent *event = GST_EVENT (info->data);
  //GstElement *bin = (GstElement *) u_data;
  decoder_data *data = (decoder_data *) u_data;

  //g_print ("inside buffer probe function\n");
  if ((info->type & GST_PAD_PROBE_TYPE_BUFFER))
  {
      GST_BUFFER_PTS(GST_BUFFER(info->data)) += data->prev_accumulated_base;
  }
  if ((info->type & GST_PAD_PROBE_TYPE_EVENT_BOTH))
  {
    if (GST_EVENT_TYPE (event) == GST_EVENT_EOS)
    {
      g_timeout_add (1, seek_decode, data->decoder);
    }

    if (GST_EVENT_TYPE (event) == GST_EVENT_SEGMENT)
    {
        GstSegment *segment;

        gst_event_parse_segment (event, (const GstSegment **) &segment);
        segment->base = data->accumulated_base;
        data->prev_accumulated_base = data->accumulated_base;
        data->accumulated_base += segment->stop;
    }

    switch (GST_EVENT_TYPE (event))
    {
      case GST_EVENT_EOS:
        /* QOS events from downstream sink elements cause decoder to drop
         * frames after looping the file since the timestamps reset to 0.
         * We should drop the QOS events since we have custom logic for
         * looping individual sources. */
         //g_print ("GST_EVENT_EOS received\n");
      case GST_EVENT_QOS:
      case GST_EVENT_SEGMENT:
      case GST_EVENT_FLUSH_START:
      case GST_EVENT_FLUSH_STOP:
        return GST_PAD_PROBE_DROP;
      default:
        break;
    }
  }
  return GST_PAD_PROBE_OK;
}

GMainLoop *loop = NULL;
GstElement **g_source_bin_list = NULL;

GstElement *pipeline = NULL, *streammux = NULL, *sink = NULL, *identity = NULL;
gint g_num_sources = 0;
gchar *uri = NULL;

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
        case GST_MESSAGE_ELEMENT:
            {
                if (gst_nvmessage_is_stream_eos (msg)) {
                    guint stream_id;
                    if (gst_nvmessage_parse_stream_eos (msg, &stream_id)) {
                        g_print ("Got EOS from stream %d\n", stream_id);
                    }
                }
                break;
            }
        default:
            break;
    }
    return TRUE;
}

unsigned int cpu_percent_utilization = 0;
long int sum = 0, lastSum = 0, idle = 0, lastIdle = 0;

static int get_cpu_percentage_utilization ()
{
    int i = 0;
    char str[100];
    const char d[2] = " ";
    char* token;

    long double idleFraction;
    FILE* fp = fopen("/proc/stat","r");
    i = 0;
    fgets(str,100,fp);
    fclose(fp);
    token = strtok(str,d);
    sum = 0;

    while(token!=NULL)
    {
        token = strtok(NULL,d);
        if(token!=NULL)
        {
            sum += atoi(token);
            if(i==3)
            {
                idle = atoi(token);
            }

            i++;
        }
    }
    idleFraction = 100 - (idle-lastIdle)*100.0/(sum-lastSum);
    //printf("Busy for : %lf %% of the time.", idleFraction);

    lastIdle = idle;
    lastSum = sum;

    return idleFraction;
}


decoder_data *dec_data[300];
gboolean sw_decode = false;

unsigned int nvdec_percent_utilization = 0;

static int get_nvdec_percentage_utilization ()
{
    FILE *fp;
    char str[1024] = {0};

    fp = popen ("/usr/bin/nvidia-smi -q | grep Decoder", "r");
    if (fp == NULL)
    {
        printf ("failed to run command\n");
        return -1;
    }

#if 0
    if (fgets (str, sizeof(str), fp) != NULL)
        printf ("%s\n", str);
    else
        printf ("nothing in the string\n");
#else
    if (fgets (str, sizeof(str), fp) == NULL)
        printf ("nothing in the string\n");
#endif


    char *tmp = str;
    while (*tmp != '%')
    {
        if (isdigit (*tmp))
        {
            nvdec_percent_utilization = atoi(tmp);
            break;
        }
        tmp++;
    }

    pclose (fp);

    return nvdec_percent_utilization;
}

unsigned int hw_decoder = 0;
unsigned int sw_decoder = 0;

static GstElement *create_source_bin (guint index, gchar *filename)
{
    GstElement *bin = NULL, *source = NULL, *h264parser = NULL, *decoder = NULL, *nvvideoconvert = NULL, *capsfilter = NULL;
    gchar bin_name[16] = { };

    dec_data[index] = (decoder_data *) malloc (sizeof (decoder_data));
    if (dec_data == NULL)
    {
        g_print ("Failed to allocate decoder data\n");
    }

    g_snprintf (bin_name, 15, "source-bin-%02d", index);
    bin = gst_bin_new (bin_name);

    source = gst_element_factory_make ("filesrc", "file-source");

    h264parser = gst_element_factory_make ("h264parse", "h264-parser");

    nvdec_percent_utilization = get_nvdec_percentage_utilization ();
    printf ("nvdec utilization = %d \n", nvdec_percent_utilization);

    cpu_percent_utilization = get_cpu_percentage_utilization ();
    printf ("cpu   utilization = %d \n", cpu_percent_utilization);

    if (nvdec_percent_utilization > 99)
        sw_decode = true;
    else
        sw_decode = false;

    //sw_decode = true;
    if (sw_decode == false)
    {
        decoder = gst_element_factory_make ("nvv4l2decoder", "nvv4l2decoder");
        hw_decoder++;
    }
    else
    {
        printf ("Linking SW decoder\n");
        decoder = gst_element_factory_make ("avdec_h264", "avdec_h264");
        sw_decoder++;
    }

    nvvideoconvert = gst_element_factory_make ("nvvideoconvert", "nvvideoconvert");

    capsfilter = gst_element_factory_make ("capsfilter", "caps-filter");

    if (sw_decode == true)
    {
        GstCaps *caps;
        GstCapsFeatures *feature;
        caps = gst_caps_new_simple ("video/x-raw", "format", G_TYPE_STRING, "NV12", "width", G_TYPE_INT, 640, "height", G_TYPE_INT, 480, NULL);
        feature = gst_caps_features_new ("memory:NVMM", NULL);
        gst_caps_set_features (caps, 0, feature);

        g_object_set (G_OBJECT(capsfilter), "caps", caps, NULL);
    }

    if (!bin || !source || !h264parser || !decoder || !nvvideoconvert || !capsfilter)
    {
        g_printerr ("One element in source bin could not be created.\n");
        return NULL;
    }

    init_decoder_data (dec_data[index], decoder);
    gulong src_buffer_probe;
    NVGSTDS_ELEM_ADD_PROBE (src_buffer_probe, decoder,
            "sink", restart_stream_buf_prob,
            (GstPadProbeType) (GST_PAD_PROBE_TYPE_EVENT_BOTH | GST_PAD_PROBE_TYPE_EVENT_FLUSH | GST_PAD_PROBE_TYPE_BUFFER),
            dec_data[index]);

    /* We set the input filename to the source element */
    g_object_set (G_OBJECT (source), "location", filename, NULL);

    gst_bin_add_many (GST_BIN (bin), source, h264parser, decoder, nvvideoconvert, capsfilter,  NULL);

    if (!gst_element_link_many (source, h264parser, decoder, nvvideoconvert, capsfilter, NULL))
    {
        g_printerr ("Failed to link source_bin elements\n");
        return NULL;
    }

    /* We need to create a ghost pad for the source bin which acts as a proxy for
     * the decoder src pad */
    GstPad *gstpad = gst_element_get_static_pad (capsfilter, "src");
    if (!gstpad)
    {
        g_printerr ("Could not find src in '%s'\n", GST_ELEMENT_NAME(capsfilter));
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
    GstPad *sinkpad = NULL;
    GstPad *src_bin_pad = NULL;


    if (nvdec_percent_utilization > 90 && cpu_percent_utilization > 75)
    {
        printf ("nvdec utilization = %d  CPU utiliztion = %d \n", nvdec_percent_utilization, cpu_percent_utilization);
        return TRUE;
    }
    //if (g_num_sources > 300)
        //return TRUE;

    g_print ("Adding Source %d \n", source_id);
    source_bin = create_source_bin(source_id, uri);
    if (!source_bin)
    {
        g_printerr ("Failed to create source bin. Exiting.\n");
        return -1;
    }
    g_source_bin_list[source_id] = source_bin;
    gst_bin_add (GST_BIN (pipeline), source_bin);

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
    printf ("HW decoders used = %d SW decoders used = %d\n", hw_decoder, sw_decoder);

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
    GstElement* nvvideoconvert2;

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
    g_object_set(G_OBJECT(streammux), "batched-push-timeout", 33333, NULL);
    g_object_set(G_OBJECT(streammux), "batch-size", 200, NULL);
    SET_GPU_ID (streammux, GPU_ID);

    if (!pipeline || !streammux)
    {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    gst_bin_add (GST_BIN (pipeline), streammux);
    g_object_set(G_OBJECT(streammux), "live-source", 1, NULL);

    g_source_bin_list = g_malloc0 (sizeof (GstElement*)*num_sources*200);
    uri = g_strdup (argv[1]);

    gchar pad_name[16]={0};
    GstPad *sinkpad = NULL;
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
    }

    g_num_sources = num_sources;

    /* Finally render the osd output */
    sink = gst_element_factory_make (SINK_ELEMENT, "fakesink");
    if (!sink)
    {
        g_printerr ("One element could not be created. Exiting.\n");
        return -1;
    }

    g_object_set (G_OBJECT (streammux), "width", MUXER_OUTPUT_WIDTH, "height", MUXER_OUTPUT_HEIGHT, NULL);

    identity = gst_element_factory_make ("identity", "identity");
    if (!identity)
    {
        g_printerr ("identity plugin could not be created\n");
        return -1;
    }

    g_object_set (G_OBJECT (identity), "silent", 0, NULL);


    /* we add a message handler */
    bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
    bus_watch_id = gst_bus_add_watch (bus, bus_call, loop);
    gst_object_unref (bus);

    /* Set up the pipeline */
    /* we add all elements into the pipeline */
    gst_bin_add_many (GST_BIN (pipeline), identity, sink, NULL);

    /* we link the elements together */
    if (!gst_element_link_many (streammux, identity, sink, NULL))
    {
        g_printerr ("Elements could not be linked. Exiting.\n");
        return -1;
    }

    g_object_set(G_OBJECT(sink), "sync", FALSE, "qos", FALSE, NULL);

    gst_element_set_state (pipeline, GST_STATE_PAUSED);
    /* Set the pipeline to "playing" state */
    //g_usleep(100000);
    g_print ("Now playing: %s\n", argv[1]);
    if (gst_element_set_state (pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr ("Failed to set pipeline to playing. Exiting.\n");
        return -1;
    }

    //GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS (GST_BIN (pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "ds-app-playing");

    /* Wait till pipeline encounters an error or EOS */
    g_print ("Running...\n");
    g_timeout_add_seconds (1, add_sources, (gpointer)g_source_bin_list);
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
