/*******************************************************************************
 * Copyright (c) 2023-2024 Synaptics Incorporated.

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/

#include <gst/gst.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <glib-object.h>
#include "gst_ai_ic.h"

/**
 * @brief Structure for parameters in json input
 */
typedef struct _ParameterData {
    gchar* model;
    gchar* meta;
    gint count;
    gfloat confidence;
    gchar* postproc_mode;
} ParameterData ;

/**
 * @brief Custom application data
 */
typedef struct _CustomData {
  GstElement *pipeline;
  GstElement *appsink;
  GMainLoop *loop;  /* GLib's Main Loop */

  /* Label Overlay */
  GList *labels; /**< list of loaded labels */
  guint total_labels; /**< count of labels */
  gint current_label_index;
  gint new_label_index;
  float current_max_score;
  float new_max_score;
  float level;
  gint frame_cntr;

  /* Input Parameters */
  ParameterData params;
} CustomData;

static CustomData ic_data;

/**
 * @brief Get label in given index
 */
static gchar *
get_label (CustomData *data, guint index)
{
  guint length;

  length = g_list_length (data->labels);
  g_return_val_if_fail (index >= 0 && index < length, NULL);

  return (gchar *) g_list_nth_data (data->labels, index);
}

/**
 * @brief Load Label text from given input file
 */
static gboolean
load_labels (gchar* file_name, CustomData *app_data)
{
    JsonParser *parser;
    JsonNode *root, *node;
    JsonArray *array;
    gchar *label = NULL;
    GError *error;
    error = NULL;

    // Create a new json parser
    parser = json_parser_new();
    g_assert (JSON_IS_PARSER (parser));

	gboolean result = json_parser_load_from_file(parser, file_name, &error);
	if (result) {
        root = json_parser_get_root(parser);
        JsonObject *object = json_node_get_object (root);
        g_assert (object != NULL);

        node = json_object_get_member (object, "labels");
        array = json_node_get_array (node);
		g_assert (array != NULL);

        for (guint i = 0; i < json_array_get_length (array); i++) {
            label = g_strdup(json_array_get_string_element(array,i));
            // TO DO - use insert
            app_data->labels = g_list_append (app_data->labels, label);
        }
		app_data->total_labels = json_array_get_length (array);
	}
	g_object_unref (parser);
	return (result);
}

/**
 * @brief Bus callback
 */
static gboolean
bus_call (GstBus     *bus,
          GstMessage *msg,
          gpointer    data)
{
  GMainLoop *loop = (GMainLoop *) data;

  switch (GST_MESSAGE_TYPE (msg)) {

    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (loop);
      break;

    case GST_MESSAGE_ERROR: {
      gchar  *debug;
      GError *error;

      gst_message_parse_error (msg, &error, &debug);
      g_free (debug);

      g_printerr ("Error: %s\n", error->message);
      g_error_free (error);

      g_main_loop_quit (loop);
      break;
    }
    default:
      break;
  }

  return TRUE;
}

/**
 * @brief Appsink callback for new sample
 */
static GstFlowReturn
on_new_sample_from_sink (GstElement * sink, CustomData *appdata)
{
  GstSample *sample;
  GstBuffer *buffer;

  /* Retrieve the buffer */
  g_signal_emit_by_name (sink, "pull-sample", &sample);
  if (!sample) {
      return GST_FLOW_ERROR;
  }
  buffer = gst_sample_get_buffer (sample);
  if (!buffer) {
      gst_sample_unref (sample);
      return GST_FLOW_ERROR;
  }

  GstCustomMeta *meta = gst_buffer_get_custom_meta (buffer, "GstSynapMeta");
  const gchar* resStr = NULL;
  if (meta) {
      GstStructure *s = gst_custom_meta_get_structure (meta);
	  if (s) {
		resStr = gst_structure_get_string (s, "ic-result");
	  }
  }

  if (resStr != NULL) {
	JsonParser *parser;
	JsonNode *root, *node;
	JsonArray *array;
    GError *error;
    error = NULL;

    // Create a new json parser
    parser = json_parser_new();
    g_assert (JSON_IS_PARSER (parser));

	gboolean result = json_parser_load_from_data(parser, resStr, strlen(resStr), &error);
    if (result) {
        root = json_parser_get_root(parser);
        JsonObject *object = json_node_get_object (root);
        g_assert (object != NULL);

        node = json_object_get_member (object, "items");
        array = json_node_get_array (node);

		JsonObject *arrobject = json_array_get_object_element (array, 0);
        appdata->new_label_index = json_object_get_int_member (arrobject, "class_index");
        appdata->new_max_score = json_object_get_double_member (arrobject, "confidence");
        //g_print ("Data: %s %f\n", get_label (appdata, appdata->new_label_index), appdata->new_max_score);
	}
    g_object_unref (parser);
  }
  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

/**
 * @brief Timer callback for updating overlay
 */
static gboolean
timer_update_result_cb (gpointer user_data)
{
   CustomData *data = (CustomData*)user_data;

   GstElement *overlay;
   gchar *label = NULL;

    if (data->current_label_index != data->new_label_index) {
      data->current_label_index = data->new_label_index;
      data->current_max_score = data->new_max_score;

      if (data->current_max_score > data->level) {
        overlay = gst_bin_get_by_name (GST_BIN (data->pipeline), "ic_label");
        label = get_label (data, data->current_label_index);
        g_object_set (overlay, "text", (label != NULL) ? label : "", NULL);

        gst_object_unref (overlay);
      } else {
            overlay = gst_bin_get_by_name (GST_BIN (data->pipeline), "ic_label");
            g_object_set (overlay, "text",  "", NULL);
            gst_object_unref (overlay);
      }
    }
    else {
       /* Index is same - check if max score has increased. This shows detection confidence has increased */
       if (data->new_max_score > data->current_max_score) {
           data->current_max_score = data->new_max_score;
               if (data->current_max_score > data->level) {
                  overlay = gst_bin_get_by_name (GST_BIN (data->pipeline), "ic_label");
                  label = get_label (data, data->current_label_index);
                  g_object_set (overlay, "text", (label != NULL) ? label : "", NULL);
                  gst_object_unref (overlay);
               }
      }
   }
   return TRUE;
}

/**
 * @brief Parse parameters from json file
 */
static gboolean
parse_parameters (gchar *file_name, ParameterData *params)
{
    JsonParser *parser;
    JsonNode *root;
    GError *error;
    error = NULL;

    // Create a new json parser
    parser = json_parser_new();
    g_assert (JSON_IS_PARSER (parser));
    gboolean result = json_parser_load_from_file(parser, file_name, &error);
    g_print ("Result: %d\n", result);
    if (result) {
        root = json_parser_get_root(parser);
        JsonObject *object;
        object = json_node_get_object(root);
        params->model = g_strdup (json_object_get_string_member (object, "model"));
        params->meta  = g_strdup (json_object_get_string_member (object, "meta"));
        params->count = json_object_get_int_member (object, "count");
        params->confidence = json_object_get_double_member (object, "confidence");
        params->postproc_mode = g_strdup (json_object_get_string_member (object, "postprocmode"));

        g_print ("Model: %s\n", params->model);
        g_print ("Meta: %s\n", params->meta);
        g_print ("Count: %d\n", params->count);
        g_print ("Confidence: %f\n", params->confidence);
        g_print ("Post Processing Mode: %s\n", params->postproc_mode);
    }
    g_object_unref (parser);
    return TRUE;
}

/**
 * @brief Main function for image classification
 */
int gst_ai_ic (AppOption *papp_options)
{
  GstBus *bus = NULL;
  guint bus_watch_id = 0;
  gchar *str_pipeline = NULL;
  guint timer_id = 0;
  gint argc = 1;
  gint ret = 0;

  /* Initialisation */
  ic_data.current_label_index = -1;
  ic_data.new_label_index = -1;
  ic_data.current_max_score = -1;
  ic_data.new_max_score = -1;
  ic_data.level = -1;
  ic_data.frame_cntr = 0;

  /* Parse Parameters from json input file */
  if (!parse_parameters (papp_options->param_file, &ic_data.params)) {
    g_print ("Invalid parameters in json\n");
    ret = -1;
    goto cleanup;
  }
  ic_data.level = ic_data.params.confidence;

  /* Initialize gstreamer */
  gst_init (&argc, NULL);

  /* Main loop */
  ic_data.loop = g_main_loop_new (NULL, FALSE);

  /* Load Labels */
  if (!load_labels (ic_data.params.meta, &ic_data)) {
    g_print ("Error in loading labels\n");
    ret = -1;
    goto cleanup;
  }
  g_print ("finished to load labels, total %d\n", ic_data.total_labels);

  /* Init pipeline */
  str_pipeline =
      g_strdup_printf
      ("filesrc location=%s ! decodebin ! videoconvert ! video/x-raw,format=RGB ! tee name=t_data "
       "t_data. ! queue ! videoconvert ! videoscale ! video/x-raw,width=224,height=224,format=RGB ! synap model=%s mode=classifier ! appsink name=synap_sink "
       "t_data. ! textoverlay name=ic_label font-desc=Sans,24 ! videoconvert ! waylandsink fullscreen=true "
       , papp_options->input, ic_data.params.model);
  ic_data.pipeline = gst_parse_launch (str_pipeline, NULL);
  g_free (str_pipeline);

  /* we add a message handler */
  bus = gst_pipeline_get_bus (GST_PIPELINE (ic_data.pipeline));
  bus_watch_id = gst_bus_add_watch (bus, bus_call, ic_data.loop);
  gst_object_unref (bus);

  /* Configure signals */
  g_print ("Configure appsink\n");
  ic_data.appsink = gst_bin_get_by_name (GST_BIN (ic_data.pipeline), "synap_sink");
  g_assert (ic_data.appsink);
  g_object_set (G_OBJECT (ic_data.appsink), "emit-signals", TRUE, "sync", FALSE, NULL);
  g_signal_connect (ic_data.appsink, "new-sample", G_CALLBACK (on_new_sample_from_sink), &ic_data);
  gst_object_unref (ic_data.appsink);

  /* timer to update result */
  timer_id = g_timeout_add (200, timer_update_result_cb, &ic_data);

  /* Set the pipeline to "playing" state*/
  gst_element_set_state (ic_data.pipeline, GST_STATE_PLAYING);

  /* Iterate */
  g_print ("Running...\n");
  g_main_loop_run (ic_data.loop);

  /* Out of the main loop, clean up nicely */
  g_print ("Returned, stopping playback: %d\n", ic_data.frame_cntr);
  gst_element_set_state (ic_data.pipeline, GST_STATE_NULL);

cleanup:
  if (timer_id > 0) {
    g_source_remove (timer_id);
  }

  if (bus_watch_id > 0) {
      g_source_remove (bus_watch_id);
  }

  if (ic_data.pipeline) {
    gst_object_unref (GST_OBJECT (ic_data.pipeline));
    ic_data.pipeline = NULL;
  }

  if (ic_data.loop) {
      g_main_loop_unref (ic_data.loop);
      ic_data.loop = NULL;
  }

  g_free (ic_data.params.model);
  g_free (ic_data.params.meta);
  g_free (ic_data.params.postproc_mode);
  if (ic_data.labels) {
    g_list_free_full (ic_data.labels, g_free);
    ic_data.labels = NULL;
  }
  return ret;
}
