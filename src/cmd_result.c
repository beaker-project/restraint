/*  
    This file is part of Restraint.

    Restraint is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Restraint is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Restraint.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <glib.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>
#include <libsoup/soup.h>
#include "upload.h"
#include "utils.h"
#include "errors.h"

#include "cmd_result.h"
#include "cmd_utils.h"

static gboolean
callback_disable_plugin (const gchar *option_name, const gchar *value,
                 gpointer user_data, GError **error)
{
    AppData *app_data = (AppData *) user_data;

    g_ptr_array_add (app_data->disable_plugin, g_strdup (value));
    return TRUE;
}

static gboolean
callback_outputfile (const gchar *option_name, const gchar *value,
                     gpointer user_data, GError **error)
{
    AppData *app_data = (AppData *) user_data;

    g_free(app_data->filename);
    app_data->filename = g_filename_display_basename (value);
    g_free(app_data->outputfile);
    app_data->outputfile = g_strdup (value);
    return TRUE;
}

static gboolean
callback_server (const gchar *option_name, const gchar *value,
                 gpointer user_data, GError **error)
{
    AppData *app_data = (AppData *) user_data;

    g_free(app_data->s.server);
    app_data->s.server = g_strdup(value);
    return TRUE;
}

AppData* restraint_create_appdata()
{
    AppData *app_data = g_slice_new0 (AppData);
    return app_data;
}

void restraint_free_appdata(AppData *app_data)
{
    g_free(app_data->filename);
    g_free(app_data->outputfile);
    g_ptr_array_free(app_data->disable_plugin, TRUE);

    g_free(app_data->result_msg);

    g_free(app_data->test_name);
    g_free(app_data->test_result);
    g_free(app_data->score);

    clear_server_data(&app_data->s);

    g_slice_free(AppData, app_data);
}

void
format_result_server(ServerData *s_data)
{
    if (s_data->server_recipe && s_data->task_id) {
        s_data->server = g_strdup_printf ("%s/tasks/%s/results/",
            s_data->server_recipe, s_data->task_id);
    }
}

/*
    When parsing arguments we have two modes of operation
    RHTS compatability mode and normal rstrnt-report-result mode

    In RHTS compat mode we do not accept command line switches
    and rely on envrionment variables and positional arguments,
    the same way rhts-report-result works.  Positional arguments
    starting with a '-' should work without problems.

    In regular restraint mode we parse the switches and commands
    as normal.  If the user would like to pass positional arguments
    that start with a '-' then they must first use a '--' switch
    to tell glib to stop processing arguments.
*/
gboolean parse_arguments_rstrnt(AppData *app_data, int argc, char *argv[])
{
    gchar **positional_args = NULL;
    GOptionContext *context = NULL;
    GOptionGroup *option_group = NULL;
    GError *error = NULL;
    gboolean rc = FALSE;
    guint positional_arg_count = 0;

    GOptionEntry entry[] = {
        {"port", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT,
            &app_data->s.port, "restraintd port number (Service default: 8081)", "PORT"},
        {"server", 's', 0, G_OPTION_ARG_CALLBACK, callback_server,
            "Server to connect to", "URL" },
        { "message", 't', 0, G_OPTION_ARG_STRING, &app_data->result_msg,
            "Short 100 characters or less message", "TEXT" },
        { "outputfile", 'o', 0, G_OPTION_ARG_CALLBACK, callback_outputfile,
            "Log to upload with result, $OUTPUTFILE is used by default", "FILE" },
        { "disable-plugin", 'p', 0, G_OPTION_ARG_CALLBACK, callback_disable_plugin,
            "don't run plugin on server side", "PLUGIN" },
        { "no-plugins", 0, 0, G_OPTION_ARG_NONE, &app_data->no_plugins,
            "don't run any plugins on server side", NULL },
        { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &positional_args,
            NULL, NULL},
        { NULL }
    };

    option_group = g_option_group_new ("main",
                                       "Application Options",
                                       "Various application related options",
                                       app_data, NULL);

    g_option_group_add_entries(option_group, entry);
    context = g_option_context_new("TASK_PATH RESULT [SCORE]");
    g_option_context_set_summary(context,
            "Report results to lab controller. if you don't specify --port or\n"
            "the server url you must have RECIPE_URL and TASKID defined.\n"
            "If HARNESS_PREFIX is defined then the value of that must be\n"
            "prefixed to RECIPE_URL and TASKID");
    g_option_context_set_main_group (context, option_group);

    gboolean parse_succeeded = g_option_context_parse(context,
        &argc, &argv, &error);

    if (parse_succeeded && app_data->s.server == NULL) {
        format_server_string(&app_data->s, format_result_server, &error);
    }

    if(!parse_succeeded || app_data->s.server == NULL){
        cmd_usage(context);
        if (error) {
            g_print("\nERROR: %s\n", error->message);
        }
        rc = FALSE;
        goto cleanup;
    }

    if( positional_args != NULL ) {
        positional_arg_count = g_strv_length(positional_args);
    }

    if( positional_args == NULL ||
        app_data->s.server == NULL ||
        positional_arg_count > 4 ||
        positional_arg_count < 2 )
    {
        cmd_usage(context);
        rc = FALSE;
        goto cleanup;
    }

    app_data->test_name = g_strdup(positional_args[0]);
    app_data->test_result = g_strdup(positional_args[1]);
    if(positional_arg_count > 2){
        app_data->score = g_strdup(positional_args[2]);
    }

    rc = TRUE;
cleanup:
    g_option_context_free(context);
    g_strfreev(positional_args);
    g_clear_error(&error);

    return rc;
}

gboolean parse_arguments_rhts(AppData *app_data, int argc, char *argv[])
{
    /*
     * The first argument (that is, argv[1]) will be --rhts, so we need to +1
     * All the argv indexes.
     * */
    format_server_string(&app_data->s, format_result_server, NULL);

    if (argc < 5 || argc > 6 ||
        app_data->s.server == NULL) {
        g_print(
            "rstrnt-report-result running in rhts-report-result compatability mode.\n\n"

            "Report results to lab controller. You must have RECIPE_URL\n"
            "and TASKID defined.\n"
            "If HARNESS_PREFIX is defined then the value of that must be\n"
            "prefixed to RECIPE_URL and TASKID\n\n"

            "Usage:\n"
            "  %s: TESTNAME TESTRESULT LOGFILE [METRIC]\n",
            argv[0]
        );
        return FALSE;
    }

    app_data->test_name = g_strdup(argv[2]);
    app_data->test_result = g_strdup(argv[3]);
    app_data->outputfile = g_strdup(argv[4]);
    if(argc > 5){
        app_data->score = g_strdup(argv[5]);
    }

    return TRUE;
}

/*
 * Work out if we are running in RHTS compatablity mode or not
 * If the first argument is --rhts, then activate RHTS compat mode
 * */
static gboolean is_compat_mode(int argc, char *argv[])
{
    if(argc == 1){
        return FALSE;
    }

    return strcmp(argv[1], "--rhts") == 0;
}

gboolean parse_arguments(AppData *app_data, int argc, char *argv[])
{
    app_data->rhts_compat = is_compat_mode(argc, argv);

    app_data->filename = g_strdup("resultoutputfile.log");
    app_data->outputfile = g_strdup(getenv("OUTPUTFILE"));
    app_data->disable_plugin = g_ptr_array_new_with_free_func (g_free);

    if(app_data->rhts_compat){
        return parse_arguments_rhts(app_data, argc, argv);
    }
    else {
        return parse_arguments_rstrnt(app_data, argc, argv);
    }
}

gboolean upload_results(AppData *app_data) {
    GError *error = NULL;
    SoupURI *result_uri = NULL;

    guint ret = 0;

    SoupSession *session;
    SoupMessage *server_msg;
    SoupRequest *request;

    gchar *form_data;
    GHashTable *data_table = g_hash_table_new (NULL, NULL);

    result_uri = soup_uri_new (app_data->s.server);
    if (result_uri == NULL) {
        g_set_error (&error, RESTRAINT_ERROR,
                     RESTRAINT_PARSE_ERROR_BAD_SYNTAX,
                     "Malformed server url: %s", app_data->s.server);
        goto cleanup;
    }
    session = soup_session_new_with_options("timeout", 3600, NULL);

    g_hash_table_insert (data_table, "path", app_data->test_name);
    g_hash_table_insert (data_table, "result", app_data->test_result);
    gchar *index = getenv("RSTRNT_INDEX");

    // if AVC_ERROR=+no_avc_check then disable the selinux check plugin
    // This is for legacy rhts tests.. please use --disable-plugin
    gchar *avc_error = getenv("AVC_ERROR");
    if (g_strcmp0 (avc_error, "+no_avc_check") == 0) {
        g_ptr_array_add (app_data->disable_plugin, g_strdup ("10_avc_check"));
    }

    if (app_data->disable_plugin->pdata) {
        g_ptr_array_add (app_data->disable_plugin, NULL);
        g_hash_table_insert (data_table, "disable_plugin",
                             g_strjoinv (" ", (gchar **)app_data->disable_plugin->pdata));
    }
    if (app_data->no_plugins)
      g_hash_table_insert (data_table, "no_plugins", &app_data->no_plugins);
    if (app_data->score)
      g_hash_table_insert (data_table, "score", app_data->score);
    if (app_data->result_msg)
      g_hash_table_insert (data_table, "message", app_data->result_msg);

    request = (SoupRequest *)soup_session_request_http_uri (session, "POST", result_uri, &error);
    server_msg = soup_request_http_get_message (SOUP_REQUEST_HTTP (request));
    g_object_unref(request);
    soup_message_headers_append(server_msg->request_headers, "task-index", index);

    form_data = soup_form_encode_hash (data_table);
    soup_message_set_request (server_msg, "application/x-www-form-urlencoded",
                              SOUP_MEMORY_TAKE, form_data, strlen (form_data));
    g_print ("** %s %s Score:%s\n", app_data->test_name, app_data->test_result,
        app_data->score != NULL ? app_data->score : "N/A");

    ret = soup_session_send_message (session, server_msg);
    if (SOUP_STATUS_IS_SUCCESSFUL (ret)) {
        gchar *location = g_strdup_printf ("%s/logs/",
                                           soup_message_headers_get_one (server_msg->response_headers, "Location"));
        soup_uri_free (result_uri);
        result_uri = soup_uri_new (location);
        g_free (location);
        if (app_data->outputfile != NULL &&
            g_file_test (app_data->outputfile, G_FILE_TEST_EXISTS))
        {
            g_print ("Uploading %s ", app_data->filename);
            if (upload_file (session, app_data->outputfile, app_data->filename, result_uri, &error)) {
                g_print ("done\n");
            } else {
                g_print ("failed\n");
            }
        }
    } else {
       g_warning ("Failed to submit result, status: %d Message: %s\n", ret, server_msg->reason_phrase);
    }
    g_object_unref(server_msg);
    soup_session_abort(session);
    g_object_unref(session);

cleanup:
    g_hash_table_destroy(data_table);
    if (result_uri != NULL) {
        soup_uri_free (result_uri);
    }

    if (error) {
        g_printerr("%s [%s, %d]\n", error->message,
                g_quark_to_string(error->domain), error->code);
        g_clear_error(&error);
        return FALSE;
    } else {
        return TRUE;
    }
}
