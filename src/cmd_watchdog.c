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
#include <unistd.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include "cmd_watchdog.h"
#include "cmd_utils.h"
#include "errors.h"
#include "process.h"
#include "utils.h"

void
format_watchdog_server(ServerData *s_data)
{
    if (s_data->server_recipe) {
        s_data->server = g_strdup_printf ("%s/watchdog", s_data->server_recipe);
    }
}


gboolean
parse_watchdog_arguments(WatchdogAppData *app_data, int argc, char *argv[], GError **error) {

    gboolean ret = TRUE;


    GOptionEntry entries[] = {
        {"port", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_INT,
            &app_data->s.port, "restraintd port number (Service default: 8081)", "PORT"},
        {"server", 's', 0, G_OPTION_ARG_STRING, &app_data->s.server,
            "Server to connect to", "URL" },
        { NULL }
    };

    GOptionContext *context = g_option_context_new("<time>");
    g_option_context_set_summary(context,
            "Adjust watchdog on lab controller. if you don't specify --port or \n"
            "the server url you must have RECIPE_URL defined.\n"
            "If HARNESS_PREFIX is defined then the value of that must be\n"
            "prefixed to RECIPE_URL");
    g_option_context_add_main_entries(context, entries, NULL);

    if (!g_option_context_parse (context, &argc, &argv, error)) {
        ret = FALSE;
        goto parse_cleanup;
    }

    if (argc < 2) {
        g_set_error (error, RESTRAINT_ERROR,
                     RESTRAINT_PARSE_ERROR_BAD_SYNTAX,
                     "Wrong arguments");
        ret = FALSE;
        goto parse_cleanup;
    }

    app_data->seconds = parse_time_string (argv[1], error);
    if (*error) {
        ret = FALSE;
        goto parse_cleanup;
     }

    if (!app_data->s.server) {
        format_server_string(&app_data->s, format_watchdog_server, error);
        if (error != NULL && *error != NULL) {
            ret = FALSE;
            goto parse_cleanup;
        }
    }

    if (!app_data->s.server) {
        ret = FALSE;
        goto parse_cleanup;
    }

parse_cleanup:
    if (ret == FALSE) {
        cmd_usage(context);
    }
    g_option_context_free(context);
    return ret;
}

gboolean
upload_watchdog (WatchdogAppData *app_data, GError **error)
{
    SoupSession *session;
    SoupURI *watchdog_uri = NULL;
    gchar *form_data;
    gchar *form_seconds;
    GHashTable *data_table = g_hash_table_new (NULL, NULL);
    gint ret = 0;
    gboolean result = TRUE;

    watchdog_uri = soup_uri_new (app_data->s.server);

    if (!watchdog_uri) {
        g_set_error (error, RESTRAINT_ERROR,
                     RESTRAINT_PARSE_ERROR_BAD_SYNTAX,
                     "Malformed server url: %s", app_data->s.server);
        result = FALSE;
        goto cleanup;
    }
    gchar* index = getenv("RSTRNT_INDEX");

    session = soup_session_new_with_options("timeout", 3600, NULL);
    SoupMessage *server_msg = soup_message_new_from_uri ("POST", watchdog_uri);
    form_seconds = g_strdup_printf ("%" G_GUINT64_FORMAT, app_data->seconds);
    g_hash_table_insert (data_table, "seconds", form_seconds);
    form_data = soup_form_encode_hash (data_table);
    g_free (form_seconds);
    soup_message_set_request (server_msg, "application/x-www-form-urlencoded",
                              SOUP_MEMORY_TAKE, form_data, strlen (form_data));
    soup_message_headers_append(server_msg->request_headers, "task-index", index);

    ret = soup_session_send_message (session, server_msg);
    if (SOUP_STATUS_IS_SUCCESSFUL (ret)) {
        if (app_data->seconds < HEARTBEAT) {
            g_warning ("Expect up to a 1 minute delay for watchdog thread to notice change.\n");
        }
    } else {
        result = FALSE;
        g_warning ("Failed to adjust watchdog, status: %d Message: %s\n", ret,
                   server_msg->reason_phrase);
    }
    g_object_unref(server_msg);

    soup_session_abort(session);
    g_object_unref(session);

cleanup:
    g_hash_table_destroy(data_table);

    if (watchdog_uri != NULL) {
        soup_uri_free (watchdog_uri);
    }
    return result;

}
