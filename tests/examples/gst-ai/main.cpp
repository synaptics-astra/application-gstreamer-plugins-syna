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
#include "options.h"
#include "gst_ai_ic.h"

static gboolean parse_options (int argc, char *argv[], AppOption *papp_options)
{
    GOptionContext *ctx;
    GError *err = NULL;

    GOptionEntry entries[] = {
        { "appmode", 'a', 0, G_OPTION_ARG_STRING, &papp_options->app_mode,
          "Application Mode - Image Classification(IC)/Object Detection(OD)", "MODE" },
        { "input", 'i', 0, G_OPTION_ARG_STRING, &papp_options->input,
          "Input Path - File Path/Camera dev node", "STRING" },
        { "output", 'o', 0, G_OPTION_ARG_STRING, &papp_options->output,
          "Output Mode - Display output on screen (screen)/Dump to file(file)", "MODE" },
        { "paramfile", 'f', 0, G_OPTION_ARG_STRING, &papp_options->param_file,
          "Parameter file", "STRING" },
        { NULL },
  };

  ctx = g_option_context_new ("GStreamer Synaptics AI Demo");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());
  if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
    g_print ("Failed to initialize: %s\n", err->message);
    g_clear_error (&err);
    g_option_context_free (ctx);
    return FALSE;
  }

  g_print ("Application Options: \n");
  if (papp_options->app_mode != NULL)
      g_print ("AppMode: %s\n", papp_options->app_mode);
  if (papp_options->input != NULL)
      g_print ("Input: %s\n", papp_options->input);
  if (papp_options->output != NULL)
      g_print ("Output: %s\n", papp_options->output);
  if (papp_options->param_file)
      g_print ("Parameter File: %s\n", papp_options->param_file);

  g_option_context_free (ctx);
  return TRUE;
}

int main (int argc, char *argv[])
{
    int ret = 0;
    AppOption app_options;

    // Initialize options
    app_options.app_mode      = NULL;
    app_options.input        = NULL;
    app_options.output       = NULL;
    app_options.param_file    = NULL;

    // Parse Options
    g_print ("Parse options\n");
    if (!parse_options (argc, argv, &app_options)) {
        g_print ("Invalid Options - Please run with --help\n");
        ret = -1;
        goto cleanup;
    }

    if (g_strcmp0 (app_options.output, "screen") != 0) {
        g_print ("Unsupported output format\n");
        ret = -1;
        goto cleanup;
    }
    if (g_strcmp0 (app_options.app_mode, "IC") == 0) {
        // Image classification
        ret = gst_ai_ic (&app_options);
    }
    else {
        g_print ("Unknown app mode\n");
        ret = -1;
        goto cleanup;
    }

cleanup:
    // Clean-up
    g_free (app_options.app_mode);
    g_free (app_options.input);
    g_free (app_options.output);
    g_free (app_options.param_file);
    return ret;
}
