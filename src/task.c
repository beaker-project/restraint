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

#define _XOPEN_SOURCE 500

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <gio/gio.h>
#include <time.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "task.h"
#include "param.h"
#include "role.h"
#include "metadata.h"
#include "process.h"
#include "message.h"
#include "dependency.h"
#include "config.h"
#include "errors.h"
#include "fetch_git.h"
#include "fetch_uri.h"
#include "utils.h"
#include "env.h"
#include "xml.h"
#include "logging.h"

void
restraint_task_result (Task *task, AppData *app_data, gchar *result,
                       gint int_score, gchar *path, gchar *message);

void
archive_entry_callback (const gchar *entry, gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    GString *message = g_string_new (NULL);

    g_string_printf (message, "** Extracting %s\n", entry);
    restraint_log_task (app_data, RSTRNT_LOG_TYPE_HARNESS, message->str, message->len);
    g_string_free (message, TRUE);
}

void
taskrun_archive_entry_callback (const gchar *entry, gpointer user_data)
{
    TaskRunData *task_run_data = (TaskRunData *) user_data;
    return archive_entry_callback (entry, task_run_data->app_data);
}

static gboolean
fetch_retry (gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    Task *task = (Task *) app_data->tasks->data;

    task->state = TASK_FETCH;

    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler,
                                                app_data,
                                                NULL);
    return FALSE;
}

static gboolean
refresh_role_retry (gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    Task *task = (Task *) app_data->tasks->data;

    task->state = TASK_REFRESH_ROLES;

    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler,
                                                app_data,
                                                NULL);
    return FALSE;
}

void
fetch_finish_callback (GError *error, guint32 match_cnt,
                       guint32 nonmatch_cnt, gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    Task *task = (Task *) app_data->tasks->data;

    if (error) {
        if (app_data->fetch_retries < TASK_FETCH_RETRIES) {
            g_warning("* RETRY fetch [%d]**:%s\n", ++app_data->fetch_retries,
                    error->message);
            g_clear_error(&error);
            g_timeout_add_seconds (TASK_FETCH_INTERVAL, fetch_retry, app_data);
            return;
        } else {
            g_propagate_error (&task->error, error);
            task->state = TASK_COMPLETE;
        }
    } else {
        task->state = TASK_METADATA_PARSE;

        if ((match_cnt > 0) || (nonmatch_cnt > 0)) {
            g_autoptr (GString) message = g_string_new (NULL);

            g_string_printf (message, "** Fetch Summary: Match %d, "
                             "Nonmatch %d\n",
                             match_cnt, nonmatch_cnt);
            restraint_log_task (app_data, RSTRNT_LOG_TYPE_HARNESS, message->str, message->len);
        }
    }

    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler,
                                                app_data,
                                                NULL);
}

void
restraint_task_fetch(AppData *app_data) {
    g_return_if_fail(app_data != NULL);
    GError *error = NULL;
    Task *task = (Task *) app_data->tasks->data;

    switch (task->fetch_method) {
        case TASK_FETCH_UNPACK:
        {
            const char *scheme = task->fetch.url->scheme;
            if (g_strcmp0(scheme, "git") == 0) {
                restraint_fetch_git (task->fetch.url,
                                     task->path,
                                     task->keepchanges,
                                     archive_entry_callback,
                                     fetch_finish_callback,
                                     app_data);
            } else if (g_strcmp0(scheme, "http") == 0 ||
                       g_strcmp0(scheme, "https") == 0  ||
                       g_strcmp0(scheme, "file") == 0) {
                restraint_fetch_uri (task->fetch.url,
                                      task->path,
                                      task->keepchanges,
                                      task->ssl_verify,
                                      archive_entry_callback,
                                      fetch_finish_callback,
                                      app_data);
            } else {
                g_set_error (&error, RESTRAINT_ERROR,
                             RESTRAINT_TASK_RUNNER_SCHEMA_ERROR,
                             "Unimplemented schema method %s",
                             task->fetch.url->scheme);
                fetch_finish_callback (error, 0, 0, app_data);
                return;
            }
            break;
        }
        case TASK_FETCH_INSTALL_PACKAGE:
            ;
            gchar *command = NULL;
            if (task->keepchanges) {
              command = g_strdup_printf ("rstrnt-package install %s",
                                         task->fetch.package_name);
            } else {
              command = g_strdup_printf ("rstrnt-package reinstall %s",
                                         task->fetch.package_name);
            }
            // Use appropriate package install command
            TaskRunData *task_run_data = g_slice_new0(TaskRunData);
            task_run_data->app_data = app_data;
            task_run_data->pass_state = TASK_METADATA_PARSE;
            task_run_data->fail_state = TASK_COMPLETE;
            task_run_data->log_type = RSTRNT_LOG_TYPE_HARNESS;
            process_run ((const gchar *)command, NULL, NULL, FALSE, 0,
                         NULL, task_io_callback, task_handler_callback,
                         NULL, 0, FALSE, app_data->cancellable, task_run_data);
            g_free (command);
            break;
        default:
            // Set task_run_data->error and add task_handler_callback
            g_set_error (&error, RESTRAINT_ERROR,
                         RESTRAINT_TASK_RUNNER_FETCH_ERROR,
                         "Unknown fetch method");
            fetch_finish_callback (error, 0, 0, app_data);
            g_return_if_reached();
    }
}

gboolean
io_callback (GIOChannel    *io,
             GIOCondition   condition,
             RstrntLogType  log_type,
             gpointer       user_data)
{
    AppData *app_data = (AppData *) user_data;
    GError *tmp_error = NULL;

    gchar buf[IO_BUFFER_SIZE] = { 0 };
    gsize bytes_read = 0;

    if (condition & G_IO_IN) {
        switch (g_io_channel_read_chars (io, buf, IO_BUFFER_SIZE - 1, &bytes_read, &tmp_error)) {
          case G_IO_STATUS_NORMAL:

            /* Suppress task output in restraintd stdout/stderr unless
               G_MESSAGES_DEBUG is used. */
            g_debug ("%s", buf);

            restraint_log_task (app_data, log_type, buf, bytes_read);

            return G_SOURCE_CONTINUE;

          case G_IO_STATUS_ERROR:
             g_warning ("IO error: %s", tmp_error->message);
             g_clear_error (&tmp_error);
             return G_SOURCE_REMOVE;

          case G_IO_STATUS_EOF:
             g_print ("finished!");
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

gboolean
task_io_callback (GIOChannel *io, GIOCondition condition, gpointer user_data) {
    TaskRunData *task_run_data = (TaskRunData *) user_data;
    return io_callback(io, condition, task_run_data->log_type, task_run_data->app_data);
}

gboolean
metadata_io_callback (GIOChannel *io, GIOCondition condition, gpointer user_data) {
    return io_callback(io, condition, RSTRNT_LOG_TYPE_HARNESS, user_data);
}

void
task_finish_plugins_callback (gint pid_result, gboolean localwatchdog, gpointer user_data, GError *error)
{
    TaskRunData *task_run_data = (TaskRunData *) user_data;
    AppData *app_data = task_run_data->app_data;

    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler,
                                                app_data,
                                                NULL);
    g_slice_free(TaskRunData, task_run_data);
}

void
task_finish_callback (gint pid_result, gboolean localwatchdog, gpointer user_data, GError *error)
{
    TaskRunData *task_run_data = (TaskRunData *) user_data;
    AppData *app_data = task_run_data->app_data;
    Task *task = app_data->tasks->data;

    // Did the command Succeed?
    if (pid_result == 0) {
        task->state = task_run_data->pass_state;

        // If not running in rhts compat mode and user hasn't reported results,
        // report PASS for exit code 0.
        if (!task->rhts_compat && !task->results_reported) {
            restraint_task_result(task, app_data, "PASS", 0, "exit_code", NULL);
        }
    } else {
        task->state = task_run_data->fail_state;

        if (localwatchdog) {
            task->localwatchdog = localwatchdog;
            restraint_config_set (app_data->config_file, task->task_id,
                                  "localwatchdog", NULL,
                                  G_TYPE_BOOLEAN, task->localwatchdog);

            g_set_error(&task->error, RESTRAINT_ERROR,
                            RESTRAINT_TASK_RUNNER_WATCHDOG_ERROR,
                            "Local watchdog expired!");
        } else {
            // If not running in rhts compat mode report FAIL if exit code is not 0
            if (!task->rhts_compat) {
                restraint_task_result(task, app_data, "FAIL", pid_result, 
                                      "exit_code", "Command returned non-zero");
            } else {
                g_set_error(&task->error, RESTRAINT_ERROR,
                            RESTRAINT_TASK_RUNNER_RC_ERROR,
                                "Command returned non-zero %i", pid_result);
            }
        }
    }

    // Run Finish/Completed plugins
    // Always run completed plugins and if localwatchdog triggered run those as well.
    gchar *command = g_strdup_printf ("%s %s", TASK_PLUGIN_SCRIPT, PLUGIN_SCRIPT);
    // Last four entries are NULL.  Replace first three with plugin vars
    gchar *localwatchdog_plugin = g_strdup_printf(" %s/localwatchdog.d", PLUGIN_DIR);
    gchar *plugin_dir = g_strdup_printf("RSTRNT_PLUGINS_DIR=%s/completed.d%s", PLUGIN_DIR, localwatchdog ? localwatchdog_plugin : "");
    g_free (localwatchdog_plugin);
    if (task->env->pdata[task->env->len - 5] != NULL) {
        g_free (task->env->pdata[task->env->len - 5]);
    }
    task->env->pdata[task->env->len - 5] = plugin_dir;

    gchar *no_plugins = g_strdup_printf("RSTRNT_NOPLUGINS=1");
    if (task->env->pdata[task->env->len - 4] != NULL) {
        g_free (task->env->pdata[task->env->len - 4]);
    }
    task->env->pdata[task->env->len - 4] = no_plugins;

    gchar *rstrnt_localwatchdog = g_strdup_printf("RSTRNT_LOCALWATCHDOG=%s", localwatchdog ? "TRUE" : "FALSE");
    if (task->env->pdata[task->env->len - 3] != NULL) {
        g_free (task->env->pdata[task->env->len - 3]);
    }
    task->env->pdata[task->env->len - 3] = rstrnt_localwatchdog;

    task_run_data->log_type = RSTRNT_LOG_TYPE_HARNESS;
    process_run ((const gchar *) command,
                 (const gchar **) task->env->pdata,
                 "/usr/share/restraint/plugins",
                 FALSE,
                 0,
                 NULL,
                 task_io_callback,
                 task_finish_plugins_callback,
                 NULL,
                 0,
                 FALSE,
                 app_data->cancellable,
                 task_run_data);
    g_free (command);
}

static void
check_param_for_override (Param *param, Task *task)
{
    const gchar *name;

    g_return_if_fail (param != NULL && task != NULL);

    name = param->name;

    if (STREQ (name, "KILLTIMEOVERRIDE") || STREQ (name, "RSTRNT_MAX_TIME")) {
        GError  *error = NULL;
        guint64  time_value;

        time_value = parse_time_string (param->value, &error);

        if (error == NULL) {
            task->remaining_time = time_value;
        } else {
            g_warning ("'max_time' override failed: %s", error->message);
            g_clear_error (&error);
        }
    } else if (STREQ (name, "RSTRNT_USE_PTY")) {
        gchar *value = g_ascii_strup (param->value, -1);

        task->metadata->use_pty = STREQ (value, "TRUE");

        g_free (value);
    }
}

void metadata_finish_cb(gpointer user_data, GError *error)
{
    AppData *app_data = (AppData *)user_data;
    Task *task = app_data->tasks->data;

    if (error) {
        g_propagate_error(&task->error, error);
    }

    if (task->error || task->metadata == NULL) {
        task->state = TASK_COMPLETE;
    } else {
        task->state = TASK_REFRESH_ROLES;
        app_data->fetch_retries = 0;

        // Set values from metadata first
        task->remaining_time = task->metadata->max_time;

        // Task param can override task metadata
        g_list_foreach (task->params, (GFunc) check_param_for_override, task);

        // Finally read remaining time from config
        gint64 remaining_time = restraint_config_get_int64 (app_data->config_file,
                                                            task->task_id,
                                                            "remaining_time",
                                                            NULL);

        task->remaining_time = remaining_time == 0 ? task->remaining_time : remaining_time;

        // If task->name is NULL then take name from metadata
        if (task->name == NULL) {
            task->name = task->metadata->name;
        }
    }

    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler,
                                                app_data,
                                                NULL);
}

void dependency_finish_cb (gpointer user_data, GError *error)
{
    TaskRunData *task_run_data = (TaskRunData *) user_data;
    AppData *app_data = (AppData *) task_run_data->app_data;
    Task *task = app_data->tasks->data;

    if (error) {
        g_propagate_error(&task->error, error);
        task->state = TASK_COMPLETE;
    } else {
        task->state = TASK_RUN;
    }
    g_slice_free (TaskRunData, task_run_data);
    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler,
                                                app_data,
                                                NULL);
}

void
task_handler_callback (gint pid_result, gboolean localwatchdog, gpointer user_data, GError *error)
{
    /*
     * Generic task handler routine.
     * After running command set task state based on pass/fail of command
     * then re-add the task_handler
     */
    TaskRunData *task_run_data = (TaskRunData *) user_data;
    AppData *app_data = task_run_data->app_data;
    Task *task = app_data->tasks->data;
    GError *tmp_error = NULL;

    task->version = get_package_version(task->fetch.package_name, &tmp_error);
    if (tmp_error) {  // Error: However, report and continue on
        g_warning ("FAILED Get RPM version: %s", tmp_error->message);
        g_clear_error(&tmp_error);
    }
    if (error) {
        task->error = g_error_copy (error);
        task->state = task_run_data->fail_state;
    } else {
        // Did the command Succeed?
        if (pid_result == 0) {
            task->state = task_run_data->pass_state;
        } else {
            task->state = task_run_data->fail_state;
            g_set_error (&task->error, RESTRAINT_ERROR,
                        RESTRAINT_TASK_RUNNER_RC_ERROR,
                        "Command returned non-zero %i", pid_result);
        }
    }
    // free the task_run_data
    g_slice_free (TaskRunData, task_run_data);

    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler,
                                                app_data,
                                                NULL);
}

static void
restraint_log_lwd_message (AppData *app_data,
                           gchar *expire_time,
                           gboolean modified_wd)
{
    time_t rawtime;
    struct tm timeinfo;
    gchar currtime[80];
    GString *message;

    g_return_if_fail(app_data != NULL && expire_time != NULL);

    message = g_string_new(NULL);
    rawtime = time(NULL);
    localtime_r(&rawtime, &timeinfo);
    strftime(currtime,80,"%a %b %d %H:%M:%S %Y", &timeinfo);
    g_string_printf(message, "*** Current Time: %s %s Localwatchdog at: %s\n",
                    currtime,
                    (modified_wd) ? " User Adjusted" : "",
                    expire_time);

    g_printerr ("%s", message->str);
    restraint_log_task (app_data, RSTRNT_LOG_TYPE_HARNESS, message->str, message->len);
    g_string_free (message, TRUE);
}

void restraint_start_heartbeat(TaskRunData *task_run_data,
                               gint64 remaining_time,
                               time_t *expire_time)
{
    time_t rawtime = time (NULL);
    struct tm timeinfo;
    if (expire_time != NULL) {
        localtime_r(expire_time, &timeinfo);
    } else {
        localtime_r(&rawtime, &timeinfo);
        timeinfo.tm_sec += remaining_time;
    }

    if ((remaining_time != 0) || (expire_time != NULL)) {
        mktime(&timeinfo);
        strftime(task_run_data->expire_time,
                 sizeof(task_run_data->expire_time),
                 "%a %b %d %H:%M:%S %Y", &timeinfo);
    } else {
        snprintf(task_run_data->expire_time,
                  sizeof(task_run_data->expire_time),
                  " * Disabled! *");
    }

}

gboolean
task_heartbeat_callback (gpointer user_data)
{
    TaskRunData *task_run_data = (TaskRunData *) user_data;
    AppData *app_data = task_run_data->app_data;
    Task *task = (Task *) app_data->tasks->data;

    time_t rawtime;
    double delta_sec = 0;
    gboolean modified_wd = FALSE;

    if (task->time_chged != NULL) {
        // user requested WD change by way of
        // rstrnt-adjust-watchdog cli
        modified_wd = TRUE;
        if (task->remaining_time) {
            time(&rawtime);
            delta_sec = difftime(rawtime, *task->time_chged);
            // calculate expired time
            *task->time_chged += task->remaining_time;
            if (delta_sec < task->remaining_time) {
                task->remaining_time -= delta_sec;
            } else {
                task->remaining_time = 0;
            }
        }
        restraint_start_heartbeat(
            task_run_data,
            task->remaining_time,
            task->time_chged);
        g_free(task->time_chged);
        task->time_chged = NULL;
    } else {
        if (task->remaining_time > HEARTBEAT) {
            task->remaining_time -= HEARTBEAT;
        } else {
            task->remaining_time = 0;
        }
    }
    restraint_config_set (app_data->config_file, task->task_id,
                          "remaining_time", NULL,
                          G_TYPE_UINT64, task->remaining_time);

    restraint_log_lwd_message(task_run_data->app_data,
                              task_run_data->expire_time,
                              modified_wd);

    return G_SOURCE_CONTINUE;
}

void task_timeout_cb(gpointer user_data, guint64 *time_remain)
{
    TaskRunData *task_run_data = (TaskRunData *) user_data;
    AppData *app_data = task_run_data->app_data;
    Task *task = (Task *) app_data->tasks->data;
    gboolean result;

    result = task_heartbeat_callback(task_run_data);
    if (!result) {
        task->remaining_time = 0;
    }

    *time_remain = task->remaining_time;
}

void
task_run (AppData *app_data)
{
    Task *task = (Task *) app_data->tasks->data;
    TaskRunData *task_run_data = g_slice_new0 (TaskRunData);
    task_run_data->app_data = app_data;
    task_run_data->pass_state = TASK_COMPLETE;
    task_run_data->fail_state = TASK_COMPLETE;

    gchar *entry_point;
    if (task->metadata->entry_point) {
        entry_point = g_strdup_printf ("%s %s", TASK_PLUGIN_SCRIPT, task->metadata->entry_point);
    } else {
        entry_point = g_strdup_printf ("%s %s", TASK_PLUGIN_SCRIPT, DEFAULT_ENTRY_POINT);
    }
    if (task->metadata->nolocalwatchdog) {
      task->remaining_time = 0;
    }

    task_run_data->log_type = RSTRNT_LOG_TYPE_TASK;
    restraint_start_heartbeat(task_run_data, task->remaining_time, NULL);
    if (task->metadata->nolocalwatchdog) {
        restraint_log_lwd_message(task_run_data->app_data,
                                  task_run_data->expire_time,
                                  FALSE);
    }

    process_run ((const gchar *) entry_point,
                 (const gchar **)task->env->pdata,
                 task->path,
                 task->metadata->use_pty,
                 task->remaining_time,
                 task_timeout_cb,
                 task_io_callback,
                 task_finish_callback,
                 NULL,
                 0,
                 FALSE,
                 app_data->cancellable,
                 task_run_data);

    g_free (entry_point);
}

static void
task_message_complete (SoupSession *session, SoupMessage *msg, gpointer user_data)
{
    // Add the task_handler back to the main loop
    AppData *app_data = (AppData *) user_data;
    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                task_handler,
                                                app_data,
                                                NULL);
}

void
restraint_task_result (Task *task, AppData *app_data, gchar *result,
                       gint int_score, gchar *path, gchar *message)
{
    g_return_if_fail(task != NULL);

    g_autofree gchar *score = g_strdup_printf("%d", int_score);
    SoupURI *task_results_uri;
    SoupMessage *server_msg;

    task_results_uri = soup_uri_new_with_base(task->task_uri, "results/");
    server_msg = soup_message_new_from_uri("POST", task_results_uri);

    soup_uri_free(task_results_uri);
    g_return_if_fail(server_msg != NULL);

    gchar *data = NULL;
    if (message == NULL) {
        data = soup_form_encode("result", result,
                                "path", path,
                                "score", score, NULL);
    } else {
        data = soup_form_encode("result", result,
                                "path", path,
                                "score", score,
                                "message", message, NULL);
        g_message("%s task %s due to : %s", result, task->task_id, message);
    }
    soup_message_set_request(server_msg, "application/x-www-form-urlencoded",
            SOUP_MEMORY_TAKE, data, strlen(data));

    app_data->queue_message(soup_session,
                            server_msg,
                            app_data->message_data,
                            NULL,
                            app_data->cancellable,
                            app_data);
}

void
restraint_task_status (Task *task, AppData *app_data, gchar *status,
                       gchar *taskversion, GError *reason)
{
    g_return_if_fail(task != NULL);

    SoupURI *task_status_uri;
    SoupMessage *server_msg;
    gchar *stime = g_strdup_printf("%ld", task->starttime);
    gchar *etime = g_strdup_printf("%ld", task->endtime);

    task_status_uri = soup_uri_new_with_base(task->task_uri, "status");
    server_msg = soup_message_new_from_uri("POST", task_status_uri);

    soup_uri_free(task_status_uri);
    g_return_if_fail(server_msg != NULL);

    gchar *data = NULL;
    GHashTable *data_table = g_hash_table_new (NULL, NULL);
    g_hash_table_insert(data_table, "status", status);
    g_hash_table_insert(data_table, "stime", stime);
    g_hash_table_insert(data_table, "etime", etime);
    if (taskversion != NULL) {
        g_hash_table_insert(data_table, "version", taskversion);
    }
    if (reason != NULL) {
        g_hash_table_insert(data_table, "etime", etime);
        g_message("%s task %s due to error: %s", status, task->task_id, reason->message);
    }
    data = soup_form_encode_hash(data_table);

    soup_message_set_request(server_msg, "application/x-www-form-urlencoded",
            SOUP_MEMORY_TAKE, data, strlen(data));

    g_hash_table_destroy(data_table);
    g_free(etime);
    g_free(stime);

    app_data->queue_message(soup_session,
                            server_msg,
                            app_data->message_data,
                            task_message_complete,
                            app_data->cancellable,
                            app_data);
}

void
restraint_task_watchdog (Task *task, AppData *app_data, guint64 seconds)
{
    g_return_if_fail(task != NULL);
    g_return_if_fail(seconds != 0);

    gchar *seconds_char = NULL;
    SoupURI *recipe_watchdog_uri;
    SoupMessage *server_msg;
    gchar *data = NULL;

    recipe_watchdog_uri = soup_uri_new_with_base(task->recipe->recipe_uri, "watchdog");
    server_msg = soup_message_new_from_uri("POST", recipe_watchdog_uri);

    soup_uri_free(recipe_watchdog_uri);
    g_return_if_fail(server_msg != NULL);

    // Add EWD_TIME to seconds and use that for the external watchdog.
    seconds_char = g_strdup_printf("%" G_GUINT64_FORMAT, seconds);
    data = soup_form_encode("seconds", seconds_char, NULL);

    soup_message_set_request(server_msg, "application/x-www-form-urlencoded",
            SOUP_MEMORY_TAKE, data, strlen(data));

    app_data->queue_message(soup_session,
                            server_msg,
                            app_data->message_data,
                            task_message_complete,
                            app_data->cancellable,
                            app_data);

    g_free(seconds_char);
}

Task *restraint_task_new(void) {
    Task *task = g_slice_new0(Task);
    task->remaining_time = -1;
    task->offsets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          g_free);
    return task;
}

void restraint_task_free(Task *task) {
    g_return_if_fail(task != NULL);
    g_free(task->task_id);

    if (NULL != task->task_uri)
        soup_uri_free (task->task_uri);

    g_free(task->name);
    g_free(task->version);
    g_free(task->path);
    g_hash_table_destroy(task->offsets);
    switch (task->fetch_method) {
        case TASK_FETCH_INSTALL_PACKAGE:
            g_free(task->fetch.package_name);
            break;
        case TASK_FETCH_UNPACK:
            if (NULL != task->fetch.url)
                soup_uri_free (task->fetch.url);
            break;
        default:
            g_return_if_reached();
    }
    g_list_free_full(task->params, (GDestroyNotify) restraint_param_free);
    g_list_free_full(task->roles, (GDestroyNotify) restraint_role_free);
    //g_strfreev (task->env);
    if (task->env)
        g_ptr_array_free (task->env, TRUE);
    restraint_metadata_free(task->metadata);
    g_slice_free(Task, task);
}

gboolean
restraint_next_task (AppData *app_data, TaskSetupState task_state) {
    Task *task = NULL;

    while ((app_data->tasks = g_list_next (app_data->tasks)) != NULL) {
        task = (Task *) app_data->tasks->data;
        task->state = task_state;
        return TRUE;
    }

    // No more tasks, let the recipe_handler know we are done.
    app_data->state = RECIPE_COMPLETE;
    app_data->aborted = ABORTED_NONE;
    app_data->recipe_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                                  recipe_handler,
                                                  app_data,
                                                  recipe_handler_finish);
    return FALSE;
}

gboolean
task_config_set_offset (const gchar  *config_file,
                        Task         *task,
                        const gchar  *path,
                        goffset       value,
                        GError      **error)
{
    GError           *tmp_err = NULL;
    g_autofree gchar *section = NULL;

    g_return_val_if_fail (task != NULL, FALSE);
    g_return_val_if_fail (config_file != NULL, FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    section = g_strdup_printf ("offsets_%s", task->task_id);

    restraint_config_set ((gchar *) config_file, section, path, &tmp_err, G_TYPE_UINT64, value);

    if (NULL == tmp_err)
        return TRUE;

    g_propagate_error (error, tmp_err);

    return FALSE;
}

static goffset *
restraint_task_new_offset (GHashTable  *offsets,
                           const gchar *path,
                           goffset      value)
{
    goffset *offset;

    g_return_val_if_fail (offsets != NULL, NULL);
    g_return_val_if_fail (path != NULL, NULL);

    offset = g_malloc (sizeof (goffset));

    *offset = value;

    g_warn_if_fail (g_hash_table_insert (offsets, g_strdup (path), offset));

    return offset;
}

goffset *
restraint_task_get_offset (Task        *task,
                           const gchar *path)
{
    goffset *offset;

    g_return_val_if_fail (task != NULL, NULL);
    g_return_val_if_fail (path != NULL, NULL);

    offset = g_hash_table_lookup (task->offsets, path);

    if (offset == NULL)
        offset = restraint_task_new_offset (task->offsets, path, 0);

    return offset;
}

static void
task_config_get_offsets (const gchar  *config_file,
                         Task         *task,
                         GError      **error)
{
    g_autofree gchar  *section = NULL;
    gchar            **pathv = NULL;
    GError            *tmp_err = NULL;

    section = g_strdup_printf ("offsets_%s", task->task_id);
    pathv = restraint_config_get_keys ((gchar *) config_file, section, &tmp_err);

    if (NULL != tmp_err)
        goto error;

    if (NULL == pathv)
        return;

    for (int i = 0; pathv[i] != NULL; i++) {
        goffset value;

        value = restraint_config_get_uint64 ((gchar *) config_file, section, pathv[i], &tmp_err);

        if (NULL != tmp_err)
            goto error;

        (void) restraint_task_new_offset (task->offsets, pathv[i], value);
    }

    goto cleanup;

  error:
    g_propagate_error (error, tmp_err);

  cleanup:
    g_strfreev (pathv);
}

static gboolean
parse_task_config (gchar   *config_file,
                   Task    *task,
                   GError **error)
{
    GError *tmp_error = NULL;

    task->reboots = restraint_config_get_uint64 (config_file,
                                                 task->task_id,
                                                 "reboots",
                                                 &tmp_error);

    if (NULL != tmp_error)
        goto error;

    task_config_get_offsets (config_file, task, &tmp_error);

    if (NULL != tmp_error)
        goto error;

    task->started = restraint_config_get_boolean (config_file,
                                                  task->task_id,
                                                  "started",
                                                  &tmp_error);

    if (NULL != tmp_error)
        goto error;

    task->localwatchdog = restraint_config_get_boolean (config_file,
                                                        task->task_id,
                                                        "localwatchdog",
                                                        &tmp_error);

    if (NULL != tmp_error)
        goto error;

    return TRUE;

  error:
    g_propagate_prefixed_error (error, tmp_error,
                                "Task %s:  %s,",
                                task->task_id, __func__);

    return FALSE;
}

static void
recipe_fetch_complete(GError *error, xmlDoc *doc, gpointer user_data)
{
    AppData *app_data = (AppData *)user_data;
    Task *task = app_data->tasks->data;

    if (error) {
        if (app_data->fetch_retries < ROLE_REFRESH_RETRIES) {
            g_print("* RETRY refresh roles [%d]**:%s\n", ++app_data->fetch_retries,
                    error->message);
            g_timeout_add_seconds (ROLE_REFRESH_INTERVAL, refresh_role_retry, app_data);
            return;
        } else {
            g_warn_if_fail(!doc);
            g_propagate_error(&task->error, error);
            task->state = TASK_COMPLETE;
        }
    } else {
        g_warn_if_fail(doc != NULL);
        GError *tmp_error = NULL;
        restraint_recipe_update_roles(app_data->recipe, doc, &tmp_error);
        if (tmp_error) {
            g_propagate_error(&task->error, tmp_error);
            task->state = TASK_COMPLETE;
        } else {
            task->state = TASK_ENV;
        }
    }

    app_data->task_handler_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
            task_handler, app_data, NULL);
}

static gboolean
uploader_func (gpointer user_data)
{
    AppData *app_data;
    Task    *task;

    g_return_val_if_fail (NULL != user_data, G_SOURCE_REMOVE);

    app_data = user_data;

    g_return_val_if_fail (NULL != app_data->tasks, G_SOURCE_REMOVE);
    g_return_val_if_fail (NULL != app_data->tasks->data, G_SOURCE_REMOVE);

    task = app_data->tasks->data;

    g_debug ("%s(): Upload event for task %s", __func__, task->task_id);

    rstrnt_upload_logs (task, app_data, soup_session, app_data->cancellable);

    return G_SOURCE_CONTINUE;
}

static void
start_uploader (AppData *app_data)
{
    Task  *task;

    g_return_if_fail (NULL != app_data);
    g_return_if_fail (app_data->uploader_interval > 0);

    app_data->uploader_source_id = g_timeout_add_seconds (app_data->uploader_interval,
                                                          uploader_func,
                                                          app_data);

    task = app_data->tasks->data;

    g_debug ("%s(): Added upload event source %d for task %s",
             __func__, app_data->uploader_source_id, task->task_id);
}

static void
stop_uploader (AppData *app_data)
{
    Task *task;

    g_return_if_fail (NULL != app_data);
    g_return_if_fail (0 != app_data->uploader_source_id);

    g_source_remove (app_data->uploader_source_id);

    task = app_data->tasks->data;

    g_debug ("%s(): Removed upload event source %d for task %s",
             __func__, app_data->uploader_source_id, task->task_id);

    app_data->uploader_source_id = 0;
}

gboolean
task_handler (gpointer user_data)
{
  AppData *app_data = (AppData *) user_data;

  // No More tasks, return false so this handler gets removed.
  if (app_data->tasks == NULL) {
      g_message ("no more tasks..");
      return G_SOURCE_REMOVE;
  }

  Task *task = app_data->tasks->data;
  GString *message = g_string_new(NULL);
  gboolean result = G_SOURCE_CONTINUE;

  /*
   *  - Fetch the task
   *  - Update metadata
   *  - Build env variables
   *  - Update external Watchdog
   *  - Add localwatchdog timeout
   *  - Install dependencies
   *  - Run task
   *  - Add child pid watcher
   *  - Add io_add_watch on pty output
   */
  switch (task->state) {
    case TASK_IDLE:
      // Read in previous state..
      if (parse_task_config (app_data->config_file, task, &task->error)) {
          if (task->finished) {
              // If the task is finished skip to the next task.
              task->state = TASK_NEXT;
          } else if (g_cancellable_is_cancelled (app_data->cancellable)) {
              task->state = TASK_COMPLETE;
          } else if (task->localwatchdog) {
              // If the task is not finished but localwatchdog expired.
              g_string_printf(message, "** Localwatchdog task: %s [%s]\n", task->task_id, task->path);
              task->state = TASK_COMPLETE;
          } else if (task->started) {
              // If the task is not finished but started then skip fetching the task again.
              g_string_printf(message, "** Continuing task: %s [%s]\n", task->task_id, task->path);
              task->state = TASK_METADATA_PARSE;
          } else {
              // If neither started nor finished then fetch the task
              restraint_task_status (task, app_data, "Running", NULL, NULL);
              result = G_SOURCE_REMOVE;
              g_string_printf(message, "** Fetching task: %s [%s]\n", task->task_id, task->path);
              app_data->fetch_retries = 0;
              task->state = TASK_FETCH;
          }
      } else {
          task->state = TASK_COMPLETE;
      }
      break;
    case TASK_FETCH:
        if (!app_data->stdin && recipe_wait_on_beaker (app_data->recipe_url, "** Task fetch"))
            break;

        // Fetch Task from rpm or url
        if (app_data->fetch_retries > 0) {
            g_string_printf(message, "** Fetching task: Retries %" G_GINT32_FORMAT "\n",
                            app_data->fetch_retries);
        }
        restraint_task_fetch (app_data);
        result = G_SOURCE_REMOVE;
        break;
    case TASK_METADATA_PARSE:
      g_string_printf (message, "** Preparing metadata\n");
      task->rhts_compat = restraint_get_metadata(task->path,
                            task->recipe->osmajor, &task->metadata,
                            app_data->cancellable, metadata_finish_cb,
                            metadata_io_callback, app_data);
      result = G_SOURCE_REMOVE;
      break;
    case TASK_REFRESH_ROLES:
      if (app_data->recipe_url) {
          if (!app_data->stdin && recipe_wait_on_beaker (app_data->recipe_url, "** Task role refresh"))
              break;

          g_string_printf(message, "** Refreshing peer role hostnames: Retries %"
                                     G_GINT32_FORMAT "\n", app_data->fetch_retries);
          restraint_xml_parse_from_url(soup_session, app_data->recipe_url,
                  recipe_fetch_complete, app_data);
          result = G_SOURCE_REMOVE;
      } else {
          task->state = TASK_ENV;
      }
      break;
    case TASK_ENV:
      // Build environment to execute task
      // Includes JOBID, TASKID, OSDISTRO, etc..
      // If not running in rhts_compat mode it will prepend
      // the variables with ENV_PREFIX.
      g_string_printf(message, "** Updating env vars\n");
      build_env(app_data->restraint_url, app_data->port, task);
      task->state = TASK_WATCHDOG;
      break;
    case TASK_WATCHDOG:
      // Setup external watchdog
      if (!task->started) {
          g_string_printf(message, "** Updating external watchdog: %" G_GINT64_FORMAT " seconds\n", task->remaining_time + EWD_TIME);
          restraint_task_watchdog (task, app_data, task->remaining_time + EWD_TIME);
          result = G_SOURCE_REMOVE;
      }
      task->state = TASK_DEPENDENCIES;
      break;
    case TASK_DEPENDENCIES:
      // Install Task Dependencies
      // All dependencies are installed with system package command
      // All repodependencies are installed via fetch_git
      if (!task->started) {
          if (!app_data->stdin && recipe_wait_on_beaker (app_data->recipe_url, "** Task dependencies"))
              break;

          g_string_printf(message, "** Installing dependencies\n");
          TaskRunData *task_run_data = g_slice_new0(TaskRunData);
          task_run_data->app_data = app_data;
          task_run_data->log_type = RSTRNT_LOG_TYPE_HARNESS;
          restraint_start_heartbeat(task_run_data, 0, NULL);
          restraint_install_dependencies (task, task_io_callback,
                                          taskrun_archive_entry_callback,
                                          dependency_finish_cb,
                                          app_data->cancellable,
                                          task_run_data);
          result = G_SOURCE_REMOVE;
      }
      task->state = TASK_RUN;
      break;
    case TASK_RUN:
      // Run TASK
      // Setup pid_handler
      //       io_handler
      //       timeout_handler
      //       heartbeat_handler
      if (g_cancellable_is_cancelled (app_data->cancellable)) {
          task->state = TASK_COMPLETE;
      } else {
          g_string_printf(message, "** Running task: %s [%s]\n", task->task_id, task->name);
          task_run (app_data);
          task->starttime = time(NULL);
          result = G_SOURCE_REMOVE;
          task->started = TRUE;
          restraint_config_set (app_data->config_file,
                                task->task_id,
                                "started", NULL,
                                G_TYPE_BOOLEAN,
                                task->started);
          // Update reboots count
          restraint_config_set (app_data->config_file,
                                task->task_id,
                                "reboots", NULL,
                                G_TYPE_UINT64,
                                task->reboots + 1);
      }
      break;
    case TASK_COMPLETE:
      // Set task finished
      if (g_cancellable_is_cancelled(app_data->cancellable) &&
          app_data->aborted != ABORTED_NONE) {
        g_clear_error(&task->error);
        g_set_error(&task->error, RESTRAINT_ERROR,
                    RESTRAINT_TASK_RUNNER_ABORTED,
                    "Aborted by rstrnt-abort");
      }

      if (task->error) {
          g_string_printf(message, "** ERROR: %s\n** Completed Task : %s\n",
                         task->error->message, task->task_id);
      } else {
          g_string_printf(message, "** Completed Task : %s\n", task->task_id);
      }
      task->endtime = time(NULL);
      task->state = TASK_COMPLETED;
      break;
    case TASK_COMPLETED:
    {
      if (rstrnt_log_manager_enabled (app_data)) {
          if (0 != app_data->uploader_source_id)
              stop_uploader (app_data);

          rstrnt_upload_logs (task, app_data, soup_session, app_data->cancellable);
      }
      // Some step along the way failed.
      if (task->error) {
        restraint_task_status(task, app_data, "Aborted", task->version, task->error);
        g_clear_error(&task->error);
      } else {
        restraint_task_status(task, app_data, "Completed", task->version, NULL);
      }
      // Rmeove the entire [task] section from the config.
      restraint_config_set (app_data->config_file, task->task_id, NULL, NULL, -1);
      task->state = TASK_NEXT;

      if (g_cancellable_is_cancelled(app_data->cancellable) &&
          app_data->aborted == ABORTED_TASK) {
          app_data->aborted = ABORTED_NONE;
        g_cancellable_reset(app_data->cancellable);
      }
      result = G_SOURCE_REMOVE;
      break;
    }
    case TASK_NEXT:
      // Get the next task and run it.
      result = restraint_next_task (app_data, TASK_IDLE);
      break;
    default:
      result = G_SOURCE_CONTINUE;
      break;
  }

  if (message->len > 0) {
      g_printerr ("%s", message->str);
      restraint_log_task (app_data, RSTRNT_LOG_TYPE_HARNESS, message->str, message->len);
  }

  g_string_free (message, TRUE);

  return result;
}

static void
connections_write (AppData     *app_data,
                   const gchar *path,
                   const gchar *msg_data,
                   gsize        msg_len)
{
    SoupMessage         *server_msg;
    Task                *task;
    goffset             *offset;
    g_autoptr (SoupURI)  task_output_uri = NULL;
    g_autofree gchar    *section = NULL;
    g_autoptr (GError)   err = NULL;

    if (app_data->tasks == NULL || g_cancellable_is_cancelled (app_data->cancellable))
        return;

    task = (Task *) app_data->tasks->data;
    task_output_uri = soup_uri_new_with_base (task->task_uri, path);
    server_msg = soup_message_new_from_uri ("PUT", task_output_uri);

    g_return_if_fail (server_msg != NULL);

    offset = restraint_task_get_offset (task, path);

    soup_message_headers_set_content_range (server_msg->request_headers,
                                            *offset,
                                            *offset + msg_len - 1,
                                            -1);
    *offset += msg_len;

    soup_message_headers_append (server_msg->request_headers, "log-level", "2");
    soup_message_set_request (server_msg, "text/plain", SOUP_MEMORY_COPY, msg_data, msg_len);

    app_data->queue_message (soup_session,
                             server_msg,
                             app_data->message_data,
                             NULL,
                             app_data->cancellable,
                             NULL);

    if (!task_config_set_offset (app_data->config_file, task, path, *offset, &err)) {
        g_warning ("%s(): Failed to set offset in config for task %s: %s",
                   __func__, task->task_id, err->message);
    }
}

void
restraint_log_task (AppData       *app_data,
                    RstrntLogType  type,
                    const char    *data,
                    gsize          size)
{
    const char *log_path = NULL;

    g_return_if_fail (app_data != NULL);
    g_return_if_fail (data != NULL && size > 0);

    if (rstrnt_log_manager_enabled (app_data)) {
        rstrnt_log_bytes (app_data->tasks->data, type, data, size);

        if (0 == app_data->uploader_source_id)
            start_uploader (app_data);

        return;
    }

    log_path = rstrnt_log_type_get_path (type);

    g_return_if_fail (NULL != log_path);

    connections_write (app_data, log_path, data, size);
}
