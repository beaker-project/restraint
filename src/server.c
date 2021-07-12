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
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gunixinputstream.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include "recipe.h"
#include "task.h"
#include "errors.h"
#include "common.h"
#include "config.h"
#include "process.h"
#include "logging.h"
#include "message.h"
#include "server.h"

SoupSession *soup_session;
GMainLoop *loop;
char *strsignal(int sig);
guint64 nproc;

static void
copy_header (SoupURI *uri, const char *name, const char *value, gpointer dest_headers)
{
    SoupURI *old_uri = NULL;
    SoupURI *new_uri = NULL;
    gchar *just_path = NULL;
    gchar *new_val = NULL;

    if (g_strcmp0 (name, "Location") == 0) {
        // convert value to URI
        old_uri = soup_uri_new_with_base (uri, value);
        // Get just the path and query
        just_path = soup_uri_to_string (old_uri, TRUE);
        // generate new uri with base plus path
        new_uri = soup_uri_new_with_base (uri, just_path);
        // convert to full url string
        new_val = soup_uri_to_string (new_uri, FALSE);
        soup_message_headers_append (dest_headers, name, new_val);
        g_free (new_val);
        g_free (just_path);
        soup_uri_free (old_uri);
        soup_uri_free (new_uri);
    } else {
        soup_message_headers_append (dest_headers, name, value);
    }
}

/**
 * Take orig and split on split_string and put new_base on beginning.
 **/
static gchar *
swap_base (const gchar *orig, const gchar *new_base, gchar *split_string)
{
  gchar *new;
  gchar **orig_pieces;
  gchar **new_pieces;

  orig_pieces = g_strsplit (orig, split_string, 2);
  new_pieces = g_strsplit (new_base, split_string, 2);
  new = g_strconcat(new_pieces[0], split_string, orig_pieces[1], NULL);
  g_strfreev(orig_pieces);
  g_strfreev(new_pieces);

  return new;
}

static void restraint_free_app_data(AppData *app_data)
{
  g_return_if_fail (app_data != NULL);

  g_free(app_data->recipe_url);
  g_free(app_data->config_file);
  g_free(app_data->restraint_url);

  if (app_data->recipe != NULL) {
    restraint_recipe_free(app_data->recipe);
    app_data->recipe = NULL;
  }

  g_clear_object (&app_data->cancellable);
  g_clear_error(&app_data->error);
  g_slice_free(AppData, app_data);
}

gboolean
server_io_callback (GIOChannel *io, GIOCondition condition, gpointer user_data) {
    //ProcessData *process_data = (ProcessData *) user_data;
    ClientData *client_data = (ClientData *) user_data;
    //ClientData *client_data = process_data->user_data;
    AppData *app_data = (AppData *) client_data->user_data;
    GError *tmp_error = NULL;

    gchar buf[IO_BUFFER_SIZE] = { 0 };
    gsize bytes_read = 0;

    if (condition & G_IO_IN) {
        switch (g_io_channel_read_chars (io, buf, IO_BUFFER_SIZE - 1, &bytes_read, &tmp_error)) {
          case G_IO_STATUS_NORMAL:

            /* Don't print to STDOUT when restraintd is launched by
               restraint client, to avoid duplicated messages.
               stdin is used when the client runs restraintd, and the
               output from restraintd is provided to the client through
               STDOUT using restraint_stdout_message in
               connections_write. */
            if (!app_data->stdin)
                g_print ("%s", buf);

	    // FIXME pass per-task data to it
            //restraint_log_task (app_data, RSTRNT_LOG_TYPE_HARNESS, buf, bytes_read);

            return G_SOURCE_CONTINUE;

          case G_IO_STATUS_ERROR:
            g_printerr ("IO error: %s\n", tmp_error->message);
            g_clear_error (&tmp_error);
            return G_SOURCE_REMOVE;

          case G_IO_STATUS_EOF:
            g_print ("finished!\n");
            return G_SOURCE_REMOVE;

          case G_IO_STATUS_AGAIN:
             g_warning("Not ready.. try again.");
             return G_SOURCE_CONTINUE;

          default:
             g_return_val_if_reached(G_SOURCE_REMOVE);
             break;
        }
    }
    if (condition & G_IO_HUP){
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_REMOVE;
}

void
plugin_finish_callback (gint pid_result, gboolean localwatchdog, gpointer user_data, GError *error)
{
    ClientData *client_data = (ClientData *) user_data;
    if (error) {
        g_warning ("** ERROR: running plugins, %s\n", error->message);
    }
    if (pid_result != 0) {
        g_warning ("** ERROR: running plugins returned non-zero %i\n", pid_result);
    }
    soup_server_unpause_message (client_data->server, client_data->client_msg);

    g_slice_free(ClientData, client_data);
}

static void
server_msg_complete (SoupSession *session, SoupMessage *server_msg, gpointer user_data)
{
    ClientData *client_data = (ClientData *) user_data;
    AppData *app_data = (AppData *) client_data->user_data;
    SoupMessage *client_msg = client_data->client_msg;
    Task *task;
    GHashTable *table;
    gboolean no_plugins = FALSE;
    SoupMessageHeadersIter iter;
    const gchar *name, *value;

    //SOUP_STATUS_IS_SUCCESSFUL(server_msg->status_code

    soup_message_headers_iter_init (&iter, server_msg->response_headers);
    while (soup_message_headers_iter_next (&iter, &name, &value))
        copy_header (soup_message_get_uri (client_msg), name, value, client_msg->response_headers);

    if (server_msg->response_body->length) {
      SoupBuffer *request = soup_message_body_flatten (server_msg->response_body);
      soup_message_body_append_buffer (client_msg->response_body, request);
      soup_buffer_free (request);
    }
    soup_message_set_status (client_msg, server_msg->status_code);

    if (g_str_has_suffix (client_data->path, "/results/")) {
        // Very important that we don't run plugins from results
        // reported from the plugins themselves.
        table = soup_form_decode (client_msg->request_body->data);
        no_plugins = g_hash_table_lookup_extended (table, "no_plugins", NULL, NULL);
	const gchar *index = soup_message_headers_get_one(client_msg->request_headers, "task-index");
        task = g_list_nth_data(app_data->tasks, g_ascii_strtoll(index, NULL, BASE10));

        // Execute report plugins
        if (!no_plugins) {
            // Create a new ProcessCommand
            gchar *command = g_strdup_printf ("%s %s", TASK_PLUGIN_SCRIPT, PLUGIN_SCRIPT);

            // Last four entries are NULL.  Replace first three with plugin vars
            gchar *result_server = g_strdup_printf("RSTRNT_RESULT_URL=%s", soup_message_headers_get_one (client_msg->response_headers, "Location"));
            if (task->env->pdata[task->env->len - 5] != NULL) {
                g_free (task->env->pdata[task->env->len - 5]);
            }
            task->env->pdata[task->env->len - 5] = result_server;

            gchar *plugin_dir = g_strdup_printf("RSTRNT_PLUGINS_DIR=%s/report_result.d", PLUGIN_DIR);
            if (task->env->pdata[task->env->len - 4] != NULL) {
                g_free (task->env->pdata[task->env->len - 4]);
            }
            task->env->pdata[task->env->len - 4] = plugin_dir;

            gchar *no_plugins = g_strdup_printf("RSTRNT_NOPLUGINS=1");
            if (task->env->pdata[task->env->len - 3] != NULL) {
                g_free (task->env->pdata[task->env->len - 3]);
            }
            task->env->pdata[task->env->len - 3] = no_plugins;
            gchar *disable_plugin = g_hash_table_lookup (table, "disable_plugin");
            if (disable_plugin) {
                gchar *disabled_plugins = g_strdup_printf ("RSTRNT_DISABLED=%s", disable_plugin);
                if (task->env->pdata[task->env->len - 2] != NULL) {
                    g_free (task->env->pdata[task->env->len - 2]);
                }
                task->env->pdata[task->env->len - 2] = disabled_plugins;
            }

            process_run ((const gchar *) command,
                         (const gchar **) task->env->pdata,
                         "/usr/share/restraint/plugins",
                         FALSE,
                         0,
                         NULL,
                         server_io_callback,
                         plugin_finish_callback,
                         NULL,
                         0,
                         FALSE,
                         app_data->cancellable,
                         client_data);
            g_free (command);
        }
        g_hash_table_destroy (table);
    } else {
        soup_server_unpause_message (client_data->server, client_msg);
        g_slice_free (ClientData, client_data);
    }

    // If no plugins are running we should return to the client right away.
    if (no_plugins) {
        soup_server_unpause_message (client_data->server, client_msg);
        g_slice_free (ClientData, client_data);
    }
}

static void
server_recipe_callback (SoupServer *server, SoupMessage *client_msg,
                     const char *path, GHashTable *query,
                     SoupClientContext *context, gpointer data)
{
    AppData *app_data = (AppData *) data;
    SoupMessage *server_msg;
    SoupURI *server_uri;
    SoupMessageHeadersIter iter;
    const gchar *name, *value;

    ClientData *client_data = g_slice_new0 (ClientData);
    client_data->path = path;
    client_data->user_data = app_data;
    client_data->client_msg = client_msg;
    client_data->server = server;

    if (app_data->state == RECIPE_IDLE) {
        soup_message_set_status_full (client_msg, SOUP_STATUS_BAD_REQUEST, "No Recipe Running");
        g_slice_free (ClientData, client_data);
        return;
    }
    // FIXME - make sure we have valid tasks first
    const gchar *index = soup_message_headers_get_one(client_msg->request_headers, "task-index");
    Task *task = g_list_nth_data(app_data->tasks, g_ascii_strtoll(index, NULL, BASE10));
    if (task == NULL) {
            g_print("you must specify with RSTRNT_INDEX\n");
            return;
    }

    if (g_str_has_suffix (path, "/results/")) {
        server_uri = soup_uri_new_with_base (task->task_uri, "results/");
        server_msg = soup_message_new_from_uri ("POST", server_uri);
        if (task != NULL) {
            task->results_reported = TRUE;
        }
    } else if (g_strrstr (path, "/logs/") != NULL) {
        gchar *uri = soup_uri_to_string(task->task_uri, FALSE);
        gchar *log_url = swap_base(path, uri, "/recipes/");
        server_uri = soup_uri_new (log_url);
        g_free (log_url);
        g_free (uri);
        server_msg = soup_message_new_from_uri ("PUT", server_uri);
    } else if (g_str_has_suffix (path, "watchdog")) {
        GHashTable *form_data;
        gchar      *encoded_form;
        gchar      *seconds_string;
        guint64     max_time;

        // Extract the number of watchdog seconds
        form_data = soup_form_decode (client_msg->request_body->data);
        seconds_string = g_hash_table_lookup (form_data, "seconds");

        if (seconds_string != NULL) {
            max_time = g_ascii_strtoull (seconds_string, NULL, BASE10);
            seconds_string = NULL;
        } else {
            /* TODO: Most likely this case should be a bad request or
             some value other than 0. EWD_TIME? */
            max_time = 0;
        }

        g_hash_table_destroy (form_data);

        // This updates the local watchdog
        if (task->metadata->nolocalwatchdog) {
            g_warning ("Adjustment to local watchdog ignored since "
                       "'no_localwatchdog' metadata is set");
        } else {
            task->remaining_time = max_time;

            if (task->time_chged == NULL)
                task->time_chged = g_malloc (sizeof (time_t));

            *task->time_chged = time (NULL);
        }

        // Update the number of watchdog seconds for External watchdog
        // by increasing it by EWD_TIME
        seconds_string = g_strdup_printf ("%" G_GUINT64_FORMAT,
                                          (max_time + EWD_TIME));
        encoded_form = soup_form_encode ("seconds", seconds_string, NULL);

        g_free (seconds_string);

        // Update client_msg with new data which gets copied
        // to server_msg further down
        soup_message_body_truncate (client_msg->request_body);
        soup_message_set_request (client_msg,
                                  "application/x-www-form-urlencoded",
                                  SOUP_MEMORY_TAKE,
                                  encoded_form,
                                  strlen (encoded_form));

        // Init header length to force soup to reset it when sending message.
        soup_message_headers_set_content_length (client_msg->request_headers, 0);

        // Start msg to send adjusted external watchdog timer to the lab controller
        // Client msg content gets copied further below.
        server_uri = soup_uri_new_with_base (task->recipe->recipe_uri, "watchdog");
        server_msg = soup_message_new_from_uri ("POST", server_uri);

    } else if (g_str_has_suffix(path, "status")) {
        gchar **splitpath = g_strsplit(path, "/", -1);
        guint pathlen = g_strv_length(splitpath);
        gchar *type = *(splitpath + pathlen - 3);
        gchar *recipe_id = NULL;
        gchar *task_id = NULL;

        // Currently supporting only "Aborted" status
        if (!g_str_has_suffix(client_msg->request_body->data, "Aborted")) {
          soup_message_set_status_full(client_msg, SOUP_STATUS_BAD_REQUEST,
                                       "Unknown status");
          goto status_cleanup;
        }

        if (g_strcmp0(type, "recipes") == 0) {
          recipe_id = *(splitpath + pathlen - 2);
        } else if (g_strcmp0(type, "tasks") == 0) {
          task_id = *(splitpath + pathlen - 2);
          recipe_id = *(splitpath + pathlen - 4);
        } else {
          soup_message_set_status_full(client_msg, SOUP_STATUS_BAD_REQUEST,
                                       "Malformed status url");
          goto status_cleanup;
        }

        if (recipe_id != NULL && g_strcmp0(recipe_id,
                                 app_data->recipe->recipe_id) != 0) {
          soup_message_set_status_full(client_msg, SOUP_STATUS_BAD_REQUEST,
                                       "Wrong recipe id");
          goto status_cleanup;
        }

        if (task_id != NULL && g_strcmp0(task_id, task->task_id) != 0) {
          soup_message_set_status_full(client_msg, SOUP_STATUS_BAD_REQUEST,
                                       "Wrong task id");
          goto status_cleanup;
        }

        if (task_id != NULL && recipe_id != NULL) {
          app_data->aborted = ABORTED_TASK;
          g_cancellable_cancel(app_data->cancellable);
          soup_message_set_status (client_msg, SOUP_STATUS_OK);
        } else if (recipe_id != NULL) {
          app_data->aborted = ABORTED_RECIPE;
          g_cancellable_cancel(app_data->cancellable);
          soup_message_set_status (client_msg, SOUP_STATUS_OK);
        }

status_cleanup:
        g_strfreev(splitpath);
        g_slice_free (ClientData, client_data);
        return;
    } else {
        soup_message_set_status_full (client_msg, SOUP_STATUS_BAD_REQUEST, "No Match, Invalid request");
        g_slice_free (ClientData, client_data);
        return;
    }

    soup_uri_free (server_uri);

    soup_message_headers_iter_init (&iter, client_msg->request_headers);
    while (soup_message_headers_iter_next (&iter, &name, &value))
        copy_header (soup_message_get_uri (server_msg), name, value, server_msg->request_headers);

    if (client_msg->request_body->length) {
      SoupBuffer *request = soup_message_body_flatten (client_msg->request_body);
      soup_message_body_append_buffer (server_msg->request_body, request);
      soup_buffer_free (request);
    }

    // Depending on how the recipe was started this will either issue a new connection back
    // to the uri that started the recipe or it will use the existing client connection
    // back to the restraint client.
    app_data->queue_message (soup_session,
                             server_msg,
                             app_data->message_data,
                             server_msg_complete,
                             app_data->cancellable,
                             client_data);
    soup_server_pause_message (server, client_msg);
}

static void
client_disconnected (SoupMessage *client_msg, gpointer data)
{
    AppData *app_data = (AppData *) data;
    ClientData *client_data = (ClientData *) app_data->message_data;

    g_message ("[%p] Client disconnected\n", client_msg);
    if (app_data->finished_handler_id != 0) {
        g_signal_handler_disconnect (client_msg, app_data->finished_handler_id);
        app_data->finished_handler_id = 0;
    }
    if (app_data->io_handler_id != 0) {
        g_source_remove (app_data->io_handler_id);
        app_data->io_handler_id = 0;
    }
    g_io_channel_unref(app_data->io_chan);
    app_data->message_data = NULL;
    g_slice_free(ClientData, client_data);
    //g_object_unref (client_msg);
}

gboolean
client_cb (GIOChannel *io, GIOCondition condition, gpointer user_data)
{
    // This is just to help libsoup "notice" that the client
    // has disconnectd
    AppData *app_data = (AppData *) user_data;
    ClientData *client_data = (ClientData *) app_data->message_data;
    // Cancel any running tasks
    g_cancellable_cancel (app_data->cancellable);
    // We exit FALSE so update app_data->io_handler to know we are gone.
    app_data->io_handler_id = 0;
    app_data->aborted = ABORTED_NONE;
    client_disconnected (client_data->client_msg, app_data);
    return FALSE;
}

static void
read_job_xml (AppData *app_data, SoupServer *soup_server)
{

    SoupMessage *client_msg = soup_message_new ("POST", "http://localhost");
    const gchar *path = "/run";

    // Record our client data..
    ClientData *client_data = g_slice_new0 (ClientData);
    client_data->path = path;
    client_data->client_msg = client_msg;
    client_data->server = soup_server;
    app_data->message_data = client_data;
    app_data->queue_message = (QueueMessage) restraint_stdout_message;
    app_data->close_message = (CloseMessage) restraint_close_message;
    app_data->state = RECIPE_FETCHING;

    GInputStream *stream = g_unix_input_stream_new (0, FALSE);
    // parse the xml from the stream
    restraint_recipe_parse_stream (stream, app_data);

    // Add recipe handler
    app_data->recipe_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                  recipe_handler,
                                                  app_data,
                                                  recipe_handler_finish);
}

gboolean
quit_loop_handler (gpointer user_data)
{
    printf("[*] Stopping mainloop\n");
    g_main_loop_quit (loop);
    return FALSE;
}

static gboolean
on_signal_term (AppData *app_data)
{
  if (app_data->close_message && app_data->message_data) {
      app_data->close_message(app_data->message_data);
  }

  g_idle_add_full (G_PRIORITY_LOW,
                   quit_loop_handler,
                   NULL,
                   NULL);
  return G_SOURCE_REMOVE;
}

static gboolean
on_sighup_term (gpointer user_data)
{
  AppData *app_data = (AppData *) user_data;
  app_data->last_signal = SIGHUP;
  return(on_signal_term(app_data));
}

static gboolean
on_sigterm_term (gpointer user_data)
{
  AppData *app_data = (AppData *) user_data;
  app_data->last_signal = SIGTERM;
  return(on_signal_term(app_data));
}

static gboolean
on_sigint_term (gpointer user_data)
{
  AppData *app_data = (AppData *) user_data;
  app_data->last_signal = SIGINT;
  return(on_signal_term(app_data));
}

static GLogWriterOutput
null_log_writer (GLogLevelFlags   log_level,
                 const GLogField *fields,
                 gsize            n_fields,
                 gpointer         user_data)
{
  return G_LOG_WRITER_HANDLED;
}

static void
null_print (const gchar *string)
{
    return;
}

/* Bind server to available IPv4 and IPv6 local addresses
 *
 * The server must NOT already be listening on any interface.
 *
 * If port is 0, the server will find an unused port to listen on. The
 * port will be the same for both addresses.
 *
 * Returns the port used if the server is bound to at least one address.
 * 0 otherwise.
 */
static guint
rstrnt_listen_any_local (SoupServer *server, guint port)
{
    GError   *error;
    GSList   *uris;
    gboolean  is_listening;

    g_return_val_if_fail (server != NULL, 0);

    /* Ensure that server is not listening on any interface */
    uris = soup_server_get_uris (server);
    is_listening = g_slist_length (uris) > 0;
    g_slist_free (uris);
    g_return_val_if_fail (!is_listening, 0);

    error = NULL;

    is_listening = soup_server_listen_local (server, port, SOUP_SERVER_LISTEN_IPV4_ONLY, &error);

    if (error != NULL) {
        g_warning ("Unable to listen on local IPv4 address: %s\n", error->message);
        g_clear_error (&error);
    } else if (port == 0) {
        /* When the port is chosen by the server, get the one used for
           IPv4 to use the same value for IPv6 */

        uris = soup_server_get_uris (server);
        port = ((SoupURI *) uris->data)->port;

        g_slist_free_full (uris, (GDestroyNotify) soup_uri_free);
    }

    is_listening |= soup_server_listen_local (server, port, SOUP_SERVER_LISTEN_IPV6_ONLY, &error);

    if (error != NULL) {
        g_warning ("Unable to listen on local IPv6 address: %s\n", error->message);
        g_clear_error (&error);
    }

    return is_listening ? port : 0;
}

static void
rstrnt_uploader_override (AppData *app_data)
{
    g_autofree gchar     *file = NULL;
    g_autoptr (GError)    err = NULL;
    g_autoptr (GKeyFile)  key_file = NULL;
    gint                  interval;

    g_return_if_fail (NULL != app_data);

    key_file = g_key_file_new ();

    file = g_build_filename (VAR_LIB_PATH, "log_manager.conf", NULL);

    if (!g_key_file_load_from_file (key_file, file, G_KEY_FILE_NONE, &err)) {
        g_debug ("%s(): %s: %s", __func__, file, err->message);

        return;
    }

    interval = g_key_file_get_integer (key_file, "log-manager", "upload_interval", &err);

    if (NULL != err) {
        g_debug ("%s(): %s", __func__, err->message);

        return;
    }

    if (0 == interval)
        /* Printed to stderr to make sure it ends up in console log. */
        g_printerr ("Log manager disabled in configuration\n");
    else if (interval < LOG_UPLOAD_MIN_INTERVAL)
        interval = LOG_UPLOAD_MIN_INTERVAL;
    else if (interval > LOG_UPLOAD_MAX_INTERVAL)
        interval = LOG_UPLOAD_MAX_INTERVAL;

    g_debug ("%s(): Log manager upload interval overridden to %d", __func__, interval);

    app_data->uploader_interval = interval;

}

void get_nproc()
{
    gchar *output = NULL;

    g_spawn_command_line_sync("nproc", &output, NULL, NULL, NULL);
    g_strstrip(output);
    nproc = g_ascii_strtoull(output, NULL, BASE10);
}

int main(int argc, char *argv[]) {
  AppData *app_data;
  const gchar *config = "config.conf";
  SoupServer *soup_server = NULL;
  GError *error = NULL;

  app_data = g_slice_new0 (AppData);
  app_data->cancellable = g_cancellable_new ();
  app_data->aborted = ABORTED_NONE;
  app_data->config_file = NULL;
  app_data->port = 0;
  app_data->uploader_source_id = 0;
  app_data->uploader_interval = LOG_UPLOAD_INTERVAL;

  rstrnt_uploader_override (app_data);

  GOptionEntry entries [] = {
    { "port", 'p', 0, G_OPTION_ARG_INT, &app_data->port, "Port to listen on", "PORT" },
    { "stdin", 's', 0, G_OPTION_ARG_NONE, &app_data->stdin, "Run from STDIN/STDOUT", NULL },
    { NULL }
  };
  GOptionContext *context = g_option_context_new(NULL);
  g_option_context_set_summary(context,
          "Test harness for Beaker. Runs tasks according to a recipe.");
  g_option_context_add_main_entries(context, entries, NULL);
  gboolean parse_succeeded = g_option_context_parse(context, &argc, &argv, &app_data->error);
  g_option_context_free(context);

  if (!parse_succeeded) {
    exit (PARSE_ARGS_FAILED);
  }

  get_nproc();

  if (!app_data->stdin) {
      app_data->config_file = g_build_filename (VAR_LIB_PATH, config, NULL);
      app_data->recipe_url = restraint_config_get_string (app_data->config_file,
                                                          "restraint",
                                                          "recipe_url", &error);
  }

  if (error) {
      g_printerr ("%s [%s, %d]\n",
                  error->message,
                  g_quark_to_string (error->domain),
                  error->code);
      g_clear_error (&error);
      exit (FAILED_GET_CONFIG_FILE);
  }

  if (app_data->recipe_url) {
    app_data->queue_message = (QueueMessage) restraint_queue_message;
    app_data->fetch_retries = 0;
    app_data->state = RECIPE_FETCH;
    app_data->recipe_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                  recipe_handler,
                                                  app_data,
                                                  recipe_handler_finish);
  }

  soup_session = soup_session_new();
  soup_session_add_feature_by_type (soup_session, SOUP_TYPE_CONTENT_SNIFFER);

  // Define a soup server
  soup_server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "restraint ", NULL);

  soup_server_add_handler (soup_server, "/recipes",
                           server_recipe_callback, app_data, NULL);

  /* Tell our soup server to listen on any local interface. This includes
     IPv4 and IPv6 if available */
  app_data->port = rstrnt_listen_any_local (soup_server, app_data->port);

  if (app_data->port == 0) {
      g_printerr ("Unable to listen on any IPv4 or IPv6 local address, exiting...\n");
      exit (FAILED_LISTEN);
  }

  app_data->restraint_url = g_strdup_printf ("http://localhost:%d", app_data->port);
  g_print ("Listening on %s\n", app_data->restraint_url);

  g_unix_signal_add (SIGINT, on_sigint_term, app_data);
  g_unix_signal_add (SIGTERM, on_sigterm_term, app_data);
  g_unix_signal_add (SIGHUP, on_sighup_term, app_data);
  int r = prctl(PR_SET_PDEATHSIG, SIGHUP);
  if (r == -1) {
     g_printerr ("Unable to set Parent Death Signal to SIGHUP: %s\n", g_strerror (errno));
     exit (FAILED_SET_PDEATHSIG);
  }

  // Read job.xml from STDIN
  if (app_data->stdin) {
      read_job_xml(app_data, soup_server);
      g_set_printerr_handler (null_print);
      g_log_set_writer_func (null_log_writer, NULL, NULL);
  }

  /* enter mainloop */
  loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (loop);

  if (app_data->last_signal != 0) {
      g_message("restraintd quit on received signal: %s(%u)\n",
                strsignal(app_data->last_signal),
                app_data->last_signal);
  }

  soup_session_abort(soup_session);
  soup_session_remove_feature_by_type (soup_session, SOUP_TYPE_CONTENT_SNIFFER);
  g_object_unref(soup_session);

  // no longer need to call soup_server_quit as disconnect does it all.
  soup_server_disconnect(soup_server);
  g_object_unref(soup_server);

  g_main_loop_unref(loop);

  if (rstrnt_log_manager_enabled (app_data)) {
      RstrntLogManager *log_manager;

      log_manager = rstrnt_log_manager_get_instance ();
      g_object_unref (log_manager);
  }

  restraint_free_app_data (app_data);

  return 0;
}
