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

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <libxml/xpath.h>
#include <libxml/relaxng.h>
#include <libxml/tree.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "client.h"
#include "errors.h"
#include "xml.h"
#include "process.h"

#define TIMESTRLEN 26

static void recipe_finish(RecipeData *recipe_data);
static gboolean run_recipe_handler (gpointer user_data);

static void restraint_free_recipe_data(RecipeData *recipe_data)
{
    g_return_if_fail (recipe_data != NULL);

    if (recipe_data->tasks != NULL) {
        g_hash_table_destroy(recipe_data->tasks);
    }

    g_string_free(recipe_data->body, TRUE);
    g_clear_object (&recipe_data->cancellable);
    g_free (recipe_data->rhost);
    g_free (recipe_data->connect_uri);

    g_slice_free(RecipeData, recipe_data);
}

void clear_regex (gpointer user_data)
{
    RegexData *regex_data = (RegexData *) user_data;
    regfree (&regex_data->regex);
    g_slice_free (RegexData, regex_data);
}

static void restraint_free_app_data(AppData *app_data)
{
    g_clear_error(&app_data->error);
    g_free(app_data->run_dir);
    g_free(app_data->rsh_cmd);

    if (app_data->result_states_to != NULL) {
        g_hash_table_destroy(app_data->result_states_to);
    }
    g_hash_table_destroy(app_data->recipes);
    if (app_data->loop != NULL) {
        g_main_loop_unref(app_data->loop);
    }

    // Free the regexes
    g_slist_free_full (app_data->regexes, clear_regex);

    g_slice_free(AppData, app_data);
}

gboolean
headers_get_content_range (GHashTable *headers,
                   goffset    *start,
                   goffset    *end,
                   goffset    *total_length)
{
    const char *header = g_hash_table_lookup (headers, "Content-Range");
    goffset length;
    char *p;

    if (!header || strncmp (header, "bytes ", 6) != 0)
                return FALSE;

    header += 6;
    while (g_ascii_isspace (*header))
        header++;
    if (!g_ascii_isdigit (*header))
        return FALSE;

    *start = g_ascii_strtoull (header, &p, 10);
    if (*p != '-')
        return FALSE;
    *end = g_ascii_strtoull (p + 1, &p, 10);
    if (*p != '/')
        return FALSE;
    p++;
    if (*p == '*') {
        length = -1;
        p++;
    } else
        length = g_ascii_strtoull (p, &p, 10);

    if (total_length)
        *total_length = length;
    return *p == '\0';
}

GHashTable *
json_to_hashtable (struct json_object *jobj) {
    GHashTable *form_data_set;
    int val_type;
    form_data_set = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          NULL, NULL);
    json_object_object_foreach(jobj, key, val) {
        val_type = json_object_get_type(val);
        switch (val_type) {
            case json_type_null:
                g_hash_table_replace (form_data_set, key, NULL);
                break;

            case json_type_string:
                g_hash_table_replace (form_data_set, key, (char *) json_object_get_string(val));
                break;

        }
    }

    return form_data_set;
}

static void
update_chunk (gchar *filename, const gchar *data, gsize size, goffset offset)
{
    gint fd = g_open(filename, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        g_warning("Failed to open %s: %s", filename, strerror(errno));
        return;
    }

    if (lseek (fd, offset, SEEK_SET) < 0) {
        g_warning("Failed to seek %s to %" G_GOFFSET_FORMAT ": %s", filename, offset,
                   strerror(errno));
    } else {
        ssize_t written = write (fd, data, size);
        g_warn_if_fail(written == size);
    }

    if (g_close(fd, NULL) < 0) {
        g_warning("Failed to close %s: %s", filename, strerror(errno));
    }
}

static void
init_result_hash (AppData *app_data)
{
    static gint none = 0;
    static gint skip = 1;
    static gint pass = 2;
    static gint warn = 3;
    static gint fail = 4;

    app_data->result_states_to = g_hash_table_new(g_str_hash, g_str_equal);
    g_hash_table_insert(app_data->result_states_to, "NONE", &none);
    g_hash_table_insert(app_data->result_states_to, "SKIPPED", &skip);
    g_hash_table_insert(app_data->result_states_to, "PASS", &pass);
    g_hash_table_insert(app_data->result_states_to, "WARN", &warn);
    g_hash_table_insert(app_data->result_states_to, "FAIL", &fail);
}

static gboolean
tasks_finished (xmlDocPtr doc, xmlNodePtr node, xmlChar *path)
{
    gboolean finished = TRUE;

    xmlXPathObjectPtr task_nodes = get_node_set (doc, node, path);
    if (task_nodes == NULL) {
        g_printerr("Failed to get node %s\n", path);
        return finished;
    }
    for (guint i = 0; i < task_nodes->nodesetval->nodeNr; i++) {
        xmlNodePtr task_node = task_nodes->nodesetval->nodeTab[i];
        xmlChar *status = xmlGetNoNsProp(task_node, (const xmlChar*) "status");
        finished &= (strcmp ((const char*)status, "Completed") == 0 ||
            strcmp((const char*)status, "Aborted") == 0);
        xmlFree (status);
    }
    xmlXPathFreeObject (task_nodes);
    return finished;
}

gboolean
quit_loop_handler (gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_main_loop_quit (loop);
    return FALSE;
}

void
record_log (xmlNodePtr node_ptr,
            const gchar *path,
            const gchar *filename)
{
    xmlNodePtr log_node_ptr = xmlNewTextChild (node_ptr,
                                               NULL,
                                               (xmlChar *) "log",
                                               NULL);
    xmlSetProp (log_node_ptr, (xmlChar *)"path", (xmlChar *) path);
    xmlSetProp (log_node_ptr, (xmlChar *)"filename", (xmlChar *) filename);
}

static gint
result_to_id (const gchar *result, GHashTable *states)
{
    gint id = 0;
    gpointer val = g_hash_table_lookup (states, result);
    if (val)
        id = *((gint *)val);
    return id;
}

static void
record_result (xmlNodePtr recipe_node_ptr,
               xmlNodePtr task_node_ptr,
               const gchar *result_id,
               const gchar *result,
               const gchar *message,
               const gchar *path,
               const gchar *score,
               AppData *app_data,
               const gchar *rhost)
{
    gchar *trunc_host = g_strndup (rhost, 20);
    xmlNodePtr results_node_ptr = first_child_with_name(task_node_ptr,
                                                        "results", TRUE);
    // record result under results_node_ptr
    xmlNodePtr result_node_ptr = xmlNewTextChild (results_node_ptr,
                                                  NULL,
                                                  (xmlChar *) "result",
                                                  (xmlChar *) message);
    xmlSetProp (result_node_ptr, (xmlChar *)"id", (xmlChar *) result_id);
    xmlSetProp (result_node_ptr, (xmlChar *)"path", (xmlChar *) path);
    xmlSetProp (result_node_ptr, (xmlChar *)"result", (xmlChar *) result);

    // add a logs node
    xmlNewTextChild (result_node_ptr,
                     NULL,
                     (xmlChar *) "logs",
                     NULL);

    if (score)
        xmlSetProp (result_node_ptr, (xmlChar *)"score", (xmlChar *) score);

    xmlChar *recipe_result = xmlGetNoNsProp(recipe_node_ptr,
                                                   (xmlChar*)"result");
    xmlChar *task_result = xmlGetNoNsProp(task_node_ptr,
                                                 (xmlChar*)"result");

    // Push higher priority results up the chain, result->task->recipe.
    if (result_to_id(result, app_data->result_states_to) >
            result_to_id((const gchar*)task_result, app_data->result_states_to))
        xmlSetProp(task_node_ptr, (xmlChar *)"result", (xmlChar *) result);
    if (result_to_id(result, app_data->result_states_to) >
            result_to_id((const gchar*)recipe_result, app_data->result_states_to))
        xmlSetProp (recipe_node_ptr, (xmlChar*)"result",
                    (xmlChar*)result);

    if (app_data->verbose == 1) {
        // FIXME - read the terminal width and base this value off that.
        gint offset = (gint) strlen (path) - 43;
        const gchar *offset_path = NULL;
        if (offset < 0) {
            offset_path = path;
        } else {
            offset_path = &path[offset];
        }
        g_print ("[%-20s] %10s [%-43s] %s", 
                 trunc_host, result_id,
                 offset_path, result);
        if (score != NULL) {
            g_print (" Score: %s", score);
        }
        g_print ("\n");
        if (message) {
            g_print ("[%-20s]           %s\n", trunc_host, message);
        }
    }
    xmlFree(recipe_result);
    xmlFree(task_result);
    g_free(trunc_host);
}

static void
put_doc (xmlDocPtr xml_doc, gchar *filename)
{
    FILE *outxml = fopen(filename, "w");
    if (outxml == NULL) {
        g_warning("Failed to open %s: %s", filename, strerror(errno));
        return;
    }
    xmlDocFormatDump (outxml, xml_doc, 1);
    fclose (outxml);
}

void
remote_process_finish (gint pid_result,
                gboolean localwatchdog,
                gpointer user_data,
                GError *error)
{
    RecipeData *recipe_data = (RecipeData *) user_data;
    AppData *app_data = recipe_data->app_data;

    // If we get an error on the first connection then we simply abort
    if (app_data->started && ! tasks_finished(app_data->xml_doc, recipe_data->recipe_node_ptr, (xmlChar *)"task")
                          && ! g_cancellable_is_cancelled(recipe_data->cancellable)
                          && app_data->conn_retries < app_data->max_retries) {
        if (error) {
            g_print ("%s [%s, %d]\n", error->message,
                     g_quark_to_string (error->domain), error->code);
        }
        g_print ("Disconnected.. delaying %d seconds. Retry %d/%d.\n",
                 DEFAULT_DELAY, app_data->conn_retries + 1, app_data->max_retries);
        app_data->conn_retries++;
        // Try and re-connect to the other host
        g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                    DEFAULT_DELAY,
                                    run_recipe_handler,
                                    recipe_data,
                                    NULL);
    } else {
        if (! app_data->error && error) {
            app_data->error = g_error_copy (error);
        }
        recipe_finish (recipe_data);
    }
}

void
tasks_results_cb (const char *path,
                  GHashTable *headers,
                  struct json_object *json_body,
                  gpointer user_data)
{
    RecipeData *recipe_data = (RecipeData*) user_data;
    AppData *app_data = recipe_data->app_data;
    GHashTable *body;
    gchar *task_id = NULL;
    gchar *recipe_id = NULL;
    gchar **entries = NULL;

    const gchar *transaction_id = g_hash_table_lookup (headers, "transaction-id");
    // Pull some values out of the path.
    entries = g_strsplit (path, "/", 0);
    task_id = g_strdup (entries[4]);
    recipe_id = g_strdup (entries[2]);

    app_data->started = TRUE;

    // Lookup our task
    xmlNodePtr task_node_ptr = g_hash_table_lookup(recipe_data->tasks,
                                                   task_id);
    if (!task_node_ptr) {
        goto cleanup;
    }

    // Record results
    body = json_to_hashtable (json_body);
    gchar *result = g_hash_table_lookup (body, "result");
    gchar *message = g_hash_table_lookup (body, "message");
    gchar *result_path = g_hash_table_lookup (body, "path");
    gchar *score = g_hash_table_lookup (body, "score");

    // Record the result
    record_result(recipe_data->recipe_node_ptr, task_node_ptr, transaction_id, result, message,
                  result_path, score, app_data, (const gchar *) recipe_data->rhost);

    g_hash_table_destroy(body);
cleanup:
    g_free (task_id);
    g_free (recipe_id);
    g_strfreev (entries);
}

gboolean
watchdog_timeout_cb (gpointer user_data)
{
    RecipeData *recipe_data = (RecipeData*) user_data;
    AppData *app_data = recipe_data->app_data;
    if (! app_data->error) {
        g_set_error (&app_data->error, RESTRAINT_ERROR,
                     RESTRAINT_TASK_RUNNER_WATCHDOG_ERROR,
                     "Recipe %d exceeded lab watchdog timer",
                     recipe_data->recipe_id);
    }
    g_cancellable_cancel (recipe_data->cancellable);
    return FALSE;
}

void
watchdog_cb (const char *path,
             GHashTable *headers,
             struct json_object *json_body,
             gpointer user_data)
{
    RecipeData *recipe_data = (RecipeData*) user_data;
    GHashTable *body;

    body = json_to_hashtable (json_body);
    gchar *seconds_string = g_hash_table_lookup (body, "seconds");
    guint64 max_time = 0;
    max_time = g_ascii_strtoull(seconds_string, NULL, 10); // XXX check errno
    g_hash_table_destroy(body);
    if (recipe_data->timeout_handler_id != 0) {
        g_source_remove (recipe_data->timeout_handler_id);
    }
    recipe_data->timeout_handler_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                                  max_time,
                                                                  watchdog_timeout_cb,
                                                                  recipe_data,
                                                                  NULL);
}

void
recipe_start_cb (const char *path,
                 GHashTable *headers,
                 struct json_object *json_body,
                 gpointer user_data)
{
    RecipeData *recipe_data = (RecipeData*) user_data;
    AppData *app_data = recipe_data->app_data;

    app_data->started = TRUE;
}

static gchar *format_datetime(time_t time) {
    struct tm result;
    localtime_r(&time, &result);
    gchar *timestr = g_malloc0(TIMESTRLEN);
    strftime(timestr, TIMESTRLEN, "%FT%T%z", &result);
    return timestr;
}

void
tasks_status_cb (const char *path,
                 GHashTable *headers,
                 struct json_object *json_body,
                 gpointer user_data)
{
    RecipeData *recipe_data = (RecipeData*) user_data;
    AppData *app_data = recipe_data->app_data;
    GHashTable *body;
    gchar *task_id = NULL;
    gchar *recipe_id = NULL;
    gchar **entries = NULL;
    gchar *trunc_host = NULL;

    const gchar *transaction_id = g_hash_table_lookup (headers, "transaction-id");
    // Pull some values out of the path.
    entries = g_strsplit (path, "/", 0);
    task_id = g_strdup (entries[4]);
    recipe_id = g_strdup (entries[2]);
    trunc_host = g_strndup ((const gchar *) recipe_data->rhost, 20);

    app_data->started = TRUE;

    // Lookup our task
    xmlNodePtr task_node_ptr = g_hash_table_lookup(recipe_data->tasks,
                                                   task_id);
    if (!task_node_ptr) {
        goto cleanup;
    }

    body = json_to_hashtable (json_body);
    gchar *status = g_hash_table_lookup (body, "status");
    gchar *message = g_hash_table_lookup (body, "message");
    gchar *version = g_hash_table_lookup (body, "version");
    if (version) {
        xmlSetProp (task_node_ptr, (xmlChar *)"version", (xmlChar *) version);
    }
    time_t stime = 0;
    time_t etime = 0;
    if (g_hash_table_contains(body, "stime")) {
        stime = atoll(g_hash_table_lookup(body, "stime"));
        if (stime > 0) {
            gchar *stimestr = format_datetime(stime);
            xmlSetProp (task_node_ptr, (xmlChar *)"start_time", (xmlChar*)stimestr);
            g_free(stimestr);
        }
    }

    if (g_hash_table_contains(body, "etime")) {
        etime = atoll(g_hash_table_lookup(body, "etime"));
        if (etime > 0) {
            gchar *etimestr = format_datetime(etime);
            xmlSetProp (task_node_ptr, (xmlChar *)"end_time", (xmlChar*)etimestr);
            g_free(etimestr);
        }
    }

    if  (stime > 0 && etime > stime) {
        time_t duration = etime - stime;
        gchar *dstr = g_strdup_printf("%02ld", duration);
        xmlSetProp (task_node_ptr, (xmlChar *)"duration", (xmlChar*)dstr);
        g_free(dstr);
    }

    if (app_data->verbose < 2) {
        xmlChar *task_name = xmlGetNoNsProp(task_node_ptr,
                                            (xmlChar *)"name");
        xmlChar *task_result = xmlGetNoNsProp(task_node_ptr,
                                              (xmlChar *)"result");
        const int offset = xmlStrlen (task_name) - 43;
        const gchar *offset_name = NULL;
        if (offset < 0) {
            offset_name = (const gchar *) task_name;
        } else {
            offset_name = (const gchar *) &task_name[offset];
        }
        g_print ("[%-20s] T: %7s [%-43s] %s",
                 trunc_host,
                 task_id,
                 offset_name,
                 status);
        if (g_strcmp0 ("None", (gchar *)task_result) != 0) {
            g_print (": %s", (gchar *)task_result);
        }
        g_print ("\n");
        xmlFree (task_name);
        xmlFree (task_result);
    }
    // If message is passed then record a result with that.
    if (message) {
        record_result(recipe_data->recipe_node_ptr,
                      task_node_ptr,
                      transaction_id,
                      "WARN",
                      message,
                      "/",
                      NULL,
                      app_data,
                      (const gchar *) recipe_data->rhost);
    }

    xmlSetProp (task_node_ptr, (xmlChar *)"status", (xmlChar *) status);
    xmlChar *recipe_status = xmlGetNoNsProp(
            recipe_data->recipe_node_ptr, (xmlChar*)"status");

    // If recipe status is not already "Aborted" then record push
    // task status to recipe.
    if (g_strcmp0((const gchar*)recipe_status, "Aborted") != 0)
        xmlSetProp(recipe_data->recipe_node_ptr, (xmlChar*)"status",
                   (xmlChar*)status);
    xmlFree(recipe_status);

    // Write out the current version of the results xml.
    gchar *filename = g_build_filename(app_data->run_dir, "job.xml",
                                       NULL);
    put_doc (app_data->xml_doc, filename);

    g_free(filename);
    g_hash_table_destroy(body);

cleanup:
    g_free (task_id);
    g_free (recipe_id);
    g_strfreev (entries);
    g_free (trunc_host);
}

void
tasks_logs_cb (const char *path,
               GHashTable *headers,
               struct json_object *json_body,
               gpointer user_data)
{
    RecipeData *recipe_data = (RecipeData*) user_data;
    AppData *app_data = recipe_data->app_data;
    gchar *task_id = NULL;
    gchar *recipe_id = NULL;
    gchar **entries = NULL;
    gchar **lines = NULL;
    gint i = 0;
    gchar *trunc_host = NULL;
    gchar *body_data = NULL;
    gsize body_length;

    // Pull some values out of the path.
    entries = g_strsplit (path, "/", 0);
    task_id = g_strdup (entries[4]);
    recipe_id = g_strdup (entries[2]);

    app_data->started = TRUE;

    // Lookup our task
    xmlNodePtr task_node_ptr = g_hash_table_lookup(recipe_data->tasks,
                                                   task_id);
    if (!task_node_ptr) {
        goto cleanup;
    }

    goffset start;
    goffset end;
    goffset total_length;
    gchar *short_path = NULL;
    gchar *log_path = g_strjoinv ("/", &entries[1]);
    gchar *filename = g_strdup_printf("%s/%s", app_data->run_dir,
                                      log_path);

    gchar *logs_xpath = NULL;
    if (g_strcmp0 (entries[5], "logs") == 0) {
        gchar *fpath = g_strjoinv ("/", &entries[6]);
        logs_xpath = g_strdup_printf("task[@id='%s']/logs", task_id);
        short_path = g_uri_unescape_string(fpath, NULL);
        g_free(fpath);
    } else {
        // We shouldn't have to specify recipe and task id since we
        // are searching from recipe node.
        gchar *fpath = g_strjoinv ("/", &entries[8]);
        logs_xpath = g_strdup_printf("task[@id='%s']/results/result[@id='%s']/logs",
                                     task_id, entries[6]);
        short_path = g_uri_unescape_string(fpath, NULL);
        g_free(fpath);
    }

    gboolean content_range = headers_get_content_range(
            headers, &start, &end, &total_length);

    gchar *basedir = g_path_get_dirname (filename);
    g_mkdir_with_parents (basedir, 0755 /* drwxr-xr-x */);
    g_free (basedir);

    body_data = (gchar *) g_base64_decode (json_object_get_string(json_body), &body_length);
    if (content_range) {
        if (body_length != (end - start + 1)) {
            g_warning("Content length does not match range length");
            goto logs_cleanup;
        }
        if (total_length > 0 && total_length < end ) {
            g_warning("Total length is smaller than range end");
            goto logs_cleanup;
        }
        if (total_length > 0 && access( filename, F_OK ) != -1 ) {
            int result = truncate ((const char *)filename, total_length);
            g_warn_if_fail(result == 0);
        }
        if (start == 0) {
            // Record log in xml
            xmlXPathObjectPtr logs_node_ptrs = get_node_set(
                    app_data->xml_doc, recipe_data->recipe_node_ptr,
                    (xmlChar*)logs_xpath);
            if (logs_node_ptrs) {
                record_log(logs_node_ptrs->nodesetval->nodeTab[0],
                           log_path, short_path);
            }
            xmlXPathFreeObject (logs_node_ptrs);
        }
        update_chunk (filename, body_data, body_length, start);
    } else {
        if (access( filename, F_OK ) != -1 ) {
            int result = truncate ((const char *)filename, body_length);
            g_warn_if_fail(result == 0);
        }
        // Record log in xml
        xmlXPathObjectPtr logs_node_ptrs = get_node_set(app_data->xml_doc,
                recipe_data->recipe_node_ptr, (xmlChar *)logs_xpath);
        if (logs_node_ptrs) {
            record_log (logs_node_ptrs->nodesetval->nodeTab[0], log_path,
                        short_path);
        }
        xmlXPathFreeObject (logs_node_ptrs);
        update_chunk (filename, body_data, body_length, (goffset) 0);
    }
    trunc_host = g_strndup ((const gchar *) recipe_data->rhost, 20);
    const gchar *log_level_char = g_hash_table_lookup (headers, "log-level");
    if (log_level_char) {
        gint log_level = g_ascii_strtoll (log_level_char, NULL, 0);
        if (app_data->verbose >= log_level) {
            lines = g_strsplit_set (body_data, "\r\n", 0);
            for (i = 0; lines[i] != NULL; i++) {
                if (strlen(lines[i]) > 0) {
                    g_print ("[%-20s] %s\n", trunc_host, lines[i]);
                }
            }
            g_strfreev(lines);
        }
    }
logs_cleanup:
    g_free (short_path);
    g_free (logs_xpath);
    g_free (log_path);
    g_free (filename);
    g_free (body_data);

cleanup:
    g_free (task_id);
    g_free (recipe_id);
    g_strfreev (entries);
    g_free (trunc_host);
}

GSList *
register_path (GSList *callbacks, char *pattern, gpointer callback)
{
    RegexData *regex_data = g_slice_new0(RegexData);
    int reti;
    reti = regcomp (&regex_data->regex, pattern, REG_EXTENDED);
    if (reti) {
        g_slice_free (RegexData, regex_data);
        g_printerr ("Could not compile regex\n");
    } else {
        regex_data->callback = callback;
        callbacks = g_slist_append (callbacks, regex_data);
    }
    return callbacks;
}

gpointer
process_path (GSList *regexes, const char *path)
{
    int reti;
    while (regexes) {
        RegexData *regex_data = (RegexData *) regexes->data;
        reti = regexec (&regex_data->regex, path, 0, NULL, 0);
        if (!reti) {
            return regex_data->callback;
        }
        regexes = g_slist_next(regexes);
    }
    return NULL;
}

struct json_object * find_object (struct json_object *jobj, const char *key) {
        struct json_object *tmp;

        json_object_object_get_ex(jobj, key, &tmp);

        return tmp;
}

void
handle_message (const char *message,
                gpointer user_data)
{
    RecipeData *recipe_data = (RecipeData*) user_data;
    AppData *app_data = recipe_data->app_data;
    RegexCallback callback;
    GHashTable *headers;
    char *rstrnt_path = NULL;
    struct json_object *jobj, *json_headers, *json_body;

    jobj = json_tokener_parse(message);
    json_headers = find_object(jobj, "headers");

    if (json_headers == NULL) {
        gchar *trunc_host = g_strndup (recipe_data->rhost, 20);
        g_print ("[%-20s] %s", trunc_host, message);
        g_free (trunc_host);
        return;
    }

    json_body = find_object(jobj, "body");
    headers = json_to_hashtable(json_headers);
    rstrnt_path = g_hash_table_lookup (headers, "rstrnt-path");

    // rstrnt_path must be defined in order to dispatch
    if (!rstrnt_path) {
        g_message("Invalid message! rstrnt-path not defined");
        return;
    }
    callback = process_path (app_data->regexes, rstrnt_path);
    if (callback) {
        // Valid message, reset connection retries.
        app_data->conn_retries = 0;
        callback (rstrnt_path,
                  headers,
                  json_body,
                  user_data);
    } else {
        g_message ("no registered callback matches %s", rstrnt_path);
    }

    json_object_put (jobj);
    g_hash_table_destroy(headers);
}

gboolean
remote_io_callback (GIOChannel *io, GIOCondition condition, gpointer user_data) {
    GError *tmp_error = NULL;

    gchar *s;
    gsize bytes_read;

    if (condition & G_IO_IN) {
        switch (g_io_channel_read_line(io, &s, &bytes_read, NULL, &tmp_error)) {
          case G_IO_STATUS_NORMAL:
            handle_message(s, user_data);
            g_free(s);
            return TRUE;

          case G_IO_STATUS_ERROR:
             g_warning ("IO Error: %s", tmp_error->message);
             g_clear_error (&tmp_error);
             return FALSE;

          case G_IO_STATUS_EOF:
             return FALSE;

          case G_IO_STATUS_AGAIN:
             return TRUE;

          default:
             g_return_val_if_reached(FALSE);
             break;
        }
    }
    if (condition & G_IO_HUP){
        return FALSE;
    }
    return FALSE;
}

xmlDocPtr
get_doc (char *docname)
{
    xmlDocPtr doc;
    doc = xmlReadFile (docname, NULL, XML_PARSE_NOBLANKS);

    if (doc == NULL ) {
        fprintf(stderr,"Document not parsed successfully. \n");
        return NULL;
    }

    return doc;
}

static void
copy_task_nodes(xmlNodeSetPtr nodeset, xmlDocPtr orig_xml_doc,
                xmlDocPtr dst_xml_doc, xmlNodePtr dst_node_ptr)
{
    gchar *new_id;
    for (gint i=0; i < nodeset->nodeNr; i++) {
        xmlNodePtr task_node_ptr = xmlNewChild (dst_node_ptr,
                                                NULL,
                                                (xmlChar *) "task",
                                                NULL);
        // Add a logs node
        xmlNewChild (task_node_ptr,
                     NULL,
                     (xmlChar *) "logs",
                     NULL);

        // Copy <fetch> node if present.
        xmlNodePtr fetch_node_ptr = first_child_with_name (nodeset->nodeTab[i],
                                                           "fetch", FALSE);
        if (fetch_node_ptr) {
            xmlNodePtr copy_fetch_node_ptr = xmlDocCopyNode(fetch_node_ptr,
                                                            dst_xml_doc, 1);
            xmlAddChild (task_node_ptr, copy_fetch_node_ptr);
        }
        // Copy <rpm> node if present.
        xmlNodePtr rpm_node_ptr = first_child_with_name (nodeset->nodeTab[i],
                                                         "rpm", FALSE);
        if (rpm_node_ptr) {
            xmlNodePtr copy_rpm_node_ptr = xmlDocCopyNode(rpm_node_ptr,
                                                          dst_xml_doc, 1);
            xmlAddChild (task_node_ptr, copy_rpm_node_ptr);
        }
        // Copy <params> node if present.
        xmlNodePtr params_node_ptr = first_child_with_name(nodeset->nodeTab[i],
                                                          "params", FALSE);
        xmlNodePtr copy_params_node_ptr = NULL;
        if (params_node_ptr) {
            copy_params_node_ptr = xmlDocCopyNode(params_node_ptr,
                                                  dst_xml_doc, 1);
            xmlAddChild (task_node_ptr, copy_params_node_ptr);
        }

        xmlChar *name = xmlGetNoNsProp (nodeset->nodeTab[i], (xmlChar*)"name");
        xmlSetProp (task_node_ptr, (xmlChar *) "name", name);
        xmlChar *keepchanges = xmlGetNoNsProp (nodeset->nodeTab[i],
                                               (xmlChar*)"keepchanges");
        xmlSetProp (task_node_ptr, (xmlChar *) "keepchanges", keepchanges);
        xmlChar *role = xmlGetNoNsProp(nodeset->nodeTab[i], (xmlChar*)"role");
        if (role != NULL) {
            xmlSetProp (task_node_ptr, (xmlChar*)"role", (xmlChar*)role);
            xmlFree(role);
        }
        new_id = g_strdup_printf ("%d", i + 1);
        xmlSetProp (task_node_ptr, (xmlChar *) "id", (xmlChar *) new_id);
        xmlSetProp (task_node_ptr, (xmlChar *) "status", (xmlChar *) "New");
        xmlSetProp (task_node_ptr, (xmlChar *) "result", (xmlChar *) "None");
        g_free(new_id);
        xmlFree(name);
        xmlFree(keepchanges);
    }
}

static void
parse_task_nodes (xmlNodeSetPtr nodeset, GHashTable *tasks)
{
    for (gint i=0; i < nodeset->nodeNr; i++) {
        xmlChar *id = xmlGetNoNsProp (nodeset->nodeTab[i], (xmlChar *)"id");
        g_hash_table_insert (tasks, id, nodeset->nodeTab[i]);
    }
}

// remove_ext: removes the "extension" from a file spec.
//   mystr is the string to process.
//   dot is the extension separator.
//   sep is the path separator (0 means to ignore).
// Returns an allocated string identical to the original but
//   with the extension removed. It must be freed when you're
//   finished with it.
// If you pass in NULL or the new string can't be allocated,
//   it returns NULL.
char *
remove_ext (char* mystr, char dot, char sep)
{
    char *retstr, *lastdot, *lastsep;

    // Error checks and allocate string.

    if (mystr == NULL)
        return NULL;
    if ((retstr = malloc (strlen (mystr) + 1)) == NULL)
        return NULL;

    // Make a copy and find the relevant characters.

    strcpy (retstr, mystr);
    lastdot = strrchr (retstr, dot);
    lastsep = (sep == 0) ? NULL : strrchr (retstr, sep);

    // If it has an extension separator.

    if (lastdot != NULL) {
        // and it's before the extenstion separator.

        if (lastsep != NULL) {
            if (lastsep < lastdot) {
                // then remove it.

                *lastdot = '\0';
            }
        } else {
            // Has extension separator with no path separator.

            *lastdot = '\0';
        }
    }

    // Return the modified string.

    return retstr;
}

static gchar *
find_next_dir (gchar *basename)
{
    guint recipe_id = 1;
    gchar *file = g_strdup_printf ("./%s.%02d", basename, recipe_id);
    while (g_file_test (file, G_FILE_TEST_EXISTS)) {
        recipe_id += 1;
        g_free (file);
        file = g_strdup_printf ("./%s.%02d", basename, recipe_id);
    }
    gint ret = g_mkdir_with_parents (file, 0755 /* drwxr-xr-x */);
    if (ret != 0) {
        g_warning("Failed to make directory %s: %s", file, g_strerror(errno));
        exit (1);
    }
    return file;
}

static void
recipe_finish(RecipeData *recipe_data)
{
    AppData *app_data = recipe_data->app_data;
    GHashTableIter iter;
    xmlNodePtr task_node;

    g_hash_table_iter_init(&iter, recipe_data->tasks);
    while (g_hash_table_iter_next(&iter, NULL, (void *)&task_node)) {
        xmlChar *status = xmlGetNoNsProp(task_node, (xmlChar*)"status");
        if (g_strcmp0((gchar *) status, "New") == 0 ||
            g_strcmp0((gchar *) status, "Running") == 0) {
            xmlSetProp(recipe_data->recipe_node_ptr, (xmlChar*)"status",
                       (xmlChar*)"Aborted");
            xmlSetProp(task_node, (xmlChar*)"status", (xmlChar*)"Aborted");
        }
        xmlFree(status);
        xmlChar *result = xmlGetNoNsProp (task_node, (xmlChar *)"result");
        if (g_strcmp0 ((gchar *) result, "PASS") != 0 && 
            g_strcmp0 ((gchar *) result, "SKIPPED") != 0 && !app_data->error) {
            g_set_error (&app_data->error, RESTRAINT_ERROR,
                         RESTRAINT_TASK_RUNNER_RESULT_ERROR,
                         "One or more tasks failed");
        }
        xmlFree(result);
    }

    if (tasks_finished(app_data->xml_doc, NULL, (xmlChar *) "//task"))
        g_idle_add_full (G_PRIORITY_LOW,
                         quit_loop_handler,
                         app_data->loop,
                         NULL);
}

static gboolean
run_recipe_handler (gpointer user_data)
{
    RecipeData *recipe_data = (RecipeData*)user_data;
    AppData *app_data = recipe_data->app_data;
    const gchar **env = NULL;
    gchar *command;

    // return the xml doc
    xmlBufferPtr buffer = xmlBufferCreate();
    gssize size = xmlNodeDump(buffer, app_data->xml_doc, recipe_data->recipe_node_ptr, 0, 1);

    command = g_strdup_printf ("%s %s -- %s --port %d --stdin",
                               app_data->rsh_cmd,
                               recipe_data->connect_uri,
                               app_data->restraint_path,
                               app_data->restraint_port);

    g_print ("Connecting to host: %s, recipe id:%d\n",
             recipe_data->connect_uri, recipe_data->recipe_id);

    process_run ((const gchar *) command,
                  env,
                  NULL,
                  FALSE,
                  0,
                  NULL,
                  remote_io_callback,
                  remote_process_finish,
                  (const gchar *) buffer->content,
                  size,
                  TRUE,
                  recipe_data->cancellable,
                  recipe_data);

    g_free (command);
    xmlBufferFree (buffer);

    return G_SOURCE_REMOVE;
}

static xmlDocPtr
new_job ()
{
    xmlDocPtr xml_doc_ptr = xmlNewDoc ((xmlChar *) "1.0");
    xml_doc_ptr->children = xmlNewDocNode (xml_doc_ptr,
                                           NULL,
                                           (xmlChar *) "job",
                                           NULL);
    return xml_doc_ptr;
}

static char *
gen_new_filename(char *prefix, char *suffix, int random_byte_size)
{
    int result, buf_size;
    char *buffer1, *buffer2;

    srand(time(0));
    result = rand();

    buf_size = sizeof(prefix) + sizeof(suffix) + random_byte_size + 1;
    buffer1 = g_malloc(random_byte_size+1);
    buffer2 = g_malloc(buf_size);

    g_snprintf(buffer1, random_byte_size+1, "%d", result);
    g_snprintf(buffer2, buf_size, "%s%s%s", prefix, buffer1, suffix);
    g_free(buffer1);

    return buffer2;
}

static xmlNodePtr
new_recipe (xmlDocPtr xml_doc_ptr, xmlChar *recipe_id,
            xmlNodePtr recipe_set_node_ptr, const xmlChar *wboard,
            const xmlChar *role, const xmlChar *owner, const xmlChar *family,
            const xmlChar *job_id)
{
    xmlNodePtr recipe_node_ptr = xmlNewTextChild (recipe_set_node_ptr,
                                       NULL,
                                       (xmlChar *) "recipe",
                                       NULL);
    xmlSetProp (recipe_node_ptr, (xmlChar *)"id", (xmlChar *) recipe_id);
    xmlSetProp (recipe_node_ptr, (xmlChar *)"status", (xmlChar *) "New");
    xmlSetProp (recipe_node_ptr, (xmlChar *)"result", (xmlChar *) "None");

    // Make random filename
    char *filename = gen_new_filename("checkpoint_", ".conf", 6);

    xmlSetProp (recipe_node_ptr, (xmlChar *)"checkpoint_file",
                (const xmlChar *)filename);
    g_free(filename);

    if (wboard != NULL) {
        xmlSetProp(recipe_node_ptr, (xmlChar *)"whiteboard", wboard);
    }
    if (role != NULL) {
        xmlSetProp(recipe_node_ptr, (xmlChar *)"role", role);
    }
    if (owner != NULL) {
        xmlSetProp(recipe_node_ptr, (xmlChar *)"owner", owner);
    }
    if (family != NULL) {
        xmlSetProp(recipe_node_ptr, (xmlChar *)"family", family);
    }
    if (job_id != NULL) {
        xmlSetProp(recipe_node_ptr, (xmlChar *)"job_id", job_id);
    }
    return recipe_node_ptr;
}

static void add_r_params(gchar *role, GSList *hostlist,
                         xmlNodePtr r_params)
{
    gchar *hoststr = NULL;
    gchar **hostarr = g_new0(gchar *, g_slist_length(hostlist) + 1);
    gchar **p = hostarr;
    xmlNodePtr param = NULL;

    for (GSList *host = hostlist; host != NULL; host = g_slist_next(host)) {
        *p++ = host->data;
    }
    hoststr = g_strjoinv(" ", hostarr);

    xmlNode *child = r_params->children;
    while (child != NULL) {
        if (child->type == XML_ELEMENT_NODE &&
                g_strcmp0((gchar *)child->name, "param") == 0) {
            xmlChar *name = xmlGetNoNsProp(child, (xmlChar*)"name");
            if (g_strcmp0((gchar*)name, role) == 0) {
                param = child;
            }

            xmlFree(name);
            if (param != NULL) {
                break;
            }
        }
        child = child->next;
    }

    if (param == NULL) {
        param = xmlNewChild(r_params, NULL, (xmlChar*)"param",
                                       NULL);
    }
    xmlSetProp(param, (xmlChar*)"name", (xmlChar*)role);
    xmlSetProp(param, (xmlChar*)"value", (xmlChar*)hoststr);

    g_free(hoststr);
    g_free(hostarr);
}

static RecipeData *new_recipe_data(AppData *app_data, gchar *recipe_id)
{
    RecipeData *recipe_data = g_slice_new0(RecipeData);
    recipe_data->body = g_string_new(NULL);
    recipe_data->app_data = app_data;
    recipe_data->cancellable = g_cancellable_new();
    // Prime the watchdog handler, give us 5 minutes to get things
    // going.
    recipe_data->timeout_handler_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                                  300,
                                                                  watchdog_timeout_cb,
                                                                  recipe_data,
                                                                  NULL);
    g_hash_table_insert(app_data->recipes, g_strdup((gchar*)recipe_id),
                        recipe_data);
    return recipe_data;
}

static guint get_node_role(xmlNodePtr node, GHashTable *roletable,
                             RecipeData *recipe_data)
{
    xmlChar *role = xmlGetNoNsProp(node, (xmlChar*)"role");

    if (role != NULL) {
        gchar *host = recipe_data->rhost;
        GSList *hostlist = g_hash_table_lookup(roletable, role);
        if (hostlist == NULL) {
            hostlist = g_slist_prepend(hostlist, host);
            g_hash_table_insert(roletable, g_strdup((gchar*)role),
                                hostlist);
        } else {
            if (g_slist_find_custom(hostlist, host,
                        (GCompareFunc)g_strcmp0) == NULL) {
                GSList *tmp_list = g_slist_copy(hostlist);
                tmp_list = g_slist_prepend(tmp_list, host);
                g_hash_table_replace(roletable,
                                     g_strdup((gchar*)role),
                                     tmp_list);
            }
        }
        xmlFree(role);
        return 1;
    }
    return 0;
}

static GSList *get_recipe_members(AppData *app_data, xmlXPathObjectPtr recipe_nodes)
{
    GSList *allhosts = NULL;

    for (guint i = 0; i < recipe_nodes->nodesetval->nodeNr; i++) {
        char *host = NULL;
        xmlNodePtr node = recipe_nodes->nodesetval->nodeTab[i];
        xmlChar *id = xmlGetNoNsProp(node, (xmlChar*)"id");
        RecipeData *recipe_data = g_hash_table_lookup(app_data->recipes,
                                                      id);
        if (recipe_data == NULL) {
            g_slist_free_full(allhosts, g_free);
            return NULL;
        }

        host = recipe_data->rhost;

        if (g_slist_find_custom(allhosts, host,
                    (GCompareFunc)g_strcmp0) == NULL) {
            allhosts = g_slist_prepend(allhosts, host);
        }
        xmlFree(id);
    }
    return allhosts;
}

static GHashTable *merge_roles(GHashTable *troles, GHashTable *rroles)
{
    GHashTableIter iter;
    gpointer role, hostlist;
    GHashTable *result = g_hash_table_new(g_str_hash, g_str_equal);

    g_hash_table_iter_init(&iter, troles);
    while (g_hash_table_iter_next(&iter, &role, &hostlist)) {
        g_hash_table_insert(result, role, hostlist);
    }

    g_hash_table_iter_init(&iter, rroles);
    while (g_hash_table_iter_next(&iter, &role, &hostlist)) {
        if (!g_hash_table_contains(result, role)) {
            g_hash_table_insert(result, role, hostlist);
        }
    }

    return result;
}

static gchar *copy_job_as_template(gchar *job, gboolean novalid,
                                   AppData *app_data)
{
    guint recipe_id = 1;
    gchar *run_dir = NULL;

    // get xmldoc
    xmlDocPtr template_xml_doc_ptr = get_doc(job);
    if (!template_xml_doc_ptr) {
        g_printerr ("Unable to parse %s\n", job);
        xmlFreeDoc(template_xml_doc_ptr);
        return NULL;
    }

    if (novalid == FALSE) {
        xmlRelaxNGParserCtxtPtr parserCtxt = xmlRelaxNGNewParserCtxt(RNG_SCHEMA);
        if (parserCtxt == NULL) {
            g_printerr ("Unable to create relaxng parsing context\n");
            xmlFreeDoc(template_xml_doc_ptr);
            return NULL;
        }

        xmlRelaxNGPtr relaxng = xmlRelaxNGParse(parserCtxt);
        if (relaxng == NULL) {
            g_printerr ("Unable to parse relaxng\n");
            xmlFreeDoc(template_xml_doc_ptr);
            xmlRelaxNGFreeParserCtxt(parserCtxt);
            return NULL;
        }

        xmlRelaxNGValidCtxtPtr validCtxt = xmlRelaxNGNewValidCtxt(relaxng);
        if (validCtxt == NULL) {
            g_printerr ("Unable to create relaxng validation context\n");
            xmlFreeDoc(template_xml_doc_ptr);
            xmlRelaxNGFree(relaxng);
            xmlRelaxNGFreeParserCtxt(parserCtxt);
            return NULL;
        }

        gint valid = xmlRelaxNGValidateDoc(validCtxt, template_xml_doc_ptr);

        xmlRelaxNGFreeValidCtxt(validCtxt);
        xmlRelaxNGFree(relaxng);
        xmlRelaxNGFreeParserCtxt(parserCtxt);

        if (valid != 0) {
            g_printerr ("Document failed validation.\n");
            xmlFreeDoc(template_xml_doc_ptr);
            return NULL;
        }
    }

    xmlXPathObjectPtr recipe_nodes = get_node_set(template_xml_doc_ptr, NULL,
            (xmlChar*)"/job/recipeSet/recipe");

    if (recipe_nodes == NULL) {
        g_printerr("Unable to find any recipes in %s\n", job);
        xmlFreeDoc(template_xml_doc_ptr);
        return NULL;
    }
    xmlXPathFreeObject(recipe_nodes);

    xmlDocPtr new_xml_doc_ptr = new_job ();

    // Find next result dir job.0, job.1, etc..
    gchar *basename = g_path_get_basename (job);
    gchar *base = remove_ext (basename, '.', 0);
    run_dir = find_next_dir(base);
    g_print ("Using %s for job run\n", run_dir);
    g_free(basename);
    g_free(base);

    GHashTable *posroles = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                  NULL, (GDestroyNotify)g_hash_table_destroy);

    xmlXPathObjectPtr recipeset_nodes = get_node_set(template_xml_doc_ptr, NULL,
                                            (xmlChar*)"/job/recipeSet");
    for (guint rs = 0; rs < recipeset_nodes->nodesetval->nodeNr; rs++) {
        xmlNodePtr rsnode = recipeset_nodes->nodesetval->nodeTab[rs];
        recipe_nodes = get_node_set(template_xml_doc_ptr, rsnode,
                                    (xmlChar*)"recipe");

        xmlNodePtr recipe_set_node_ptr = xmlNewTextChild(new_xml_doc_ptr->children,
                                                         NULL,
                                                         (xmlChar *) "recipeSet",
                                                         NULL);

        GHashTable *rsroles = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                      NULL, (GDestroyNotify)g_hash_table_destroy);
        g_hash_table_insert(posroles, GINT_TO_POINTER(rs), rsroles);

        GHashTable *rroles = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, (GDestroyNotify)g_slist_free);
        g_hash_table_insert(rsroles, GINT_TO_POINTER(0), rroles);

        for (guint i = 0; i < recipe_nodes->nodesetval->nodeNr; i++) {
            xmlNodePtr node = recipe_nodes->nodesetval->nodeTab[i];
            xmlChar *wboard = xmlGetNoNsProp(node, (xmlChar*)"whiteboard");
            xmlChar *id = xmlGetNoNsProp(node, (xmlChar*)"id");
            xmlChar *role = xmlGetNoNsProp(node, (xmlChar*)"role");
            xmlChar *owner = xmlGetNoNsProp(node, (xmlChar*)"owner");
            xmlChar *family = xmlGetNoNsProp(node, (xmlChar*)"family");
            xmlChar *job_id = xmlGetNoNsProp(node, (xmlChar*)"job_id");

            if (id == NULL) {
                /* This is so that we know the 'id' pointer is always an
                   xmlChar* allocated by libxml2 and cleaned up with xmlFree */
                gchar *id_temp = g_strdup_printf ("%u", recipe_id++);
                id = xmlStrdup((const xmlChar*) id_temp);
                g_free(id_temp);
            }

            RecipeData *recipe_data = g_hash_table_lookup(app_data->recipes,
                                                          id);
            if (recipe_data == NULL) {
                g_printerr ("Unable to find matching host for recipe id:%s did you pass --host on the cmd line?\n", id);
                xmlFreeDoc(template_xml_doc_ptr);
                xmlXPathFreeObject(recipe_nodes);
                g_hash_table_destroy(posroles);
                return NULL;
            }

            get_node_role(node, rroles, recipe_data);
            xmlNodePtr new_recipe_ptr = new_recipe(new_xml_doc_ptr, id,
                                           recipe_set_node_ptr, wboard,
                                           role, owner, family, job_id);

            // Copy recipe params
            xmlXPathObjectPtr params_node = get_node_set(template_xml_doc_ptr,
                    node, (xmlChar*)"params");
            if (params_node) {
                xmlNodePtr copy_params_node_ptr = xmlDocCopyNode(params_node->nodesetval->nodeTab[0],
                                                  new_xml_doc_ptr, 1);
                xmlAddChild (new_recipe_ptr, copy_params_node_ptr);
                xmlXPathFreeObject(params_node);
                params_node = NULL;
            }

            // find task nodes
            xmlXPathObjectPtr task_nodes = get_node_set(template_xml_doc_ptr,
                    node, (xmlChar*)"task");
            if (task_nodes) {
                copy_task_nodes(task_nodes->nodesetval, template_xml_doc_ptr,
                                new_xml_doc_ptr, new_recipe_ptr);

                for (guint j = 0; j < task_nodes->nodesetval->nodeNr; j++) {
                    xmlNodePtr tnode = task_nodes->nodesetval->nodeTab[j];
                    guint newtable = 0;
                    GHashTable *trole = g_hash_table_lookup(rsroles,
                                            GINT_TO_POINTER(j + 1));
                    if (trole == NULL) {
                        trole = g_hash_table_new_full(g_str_hash, g_str_equal,
                                    g_free, (GDestroyNotify)g_slist_free);
                        newtable = 1;
                    }
                    guint insert = get_node_role(tnode, trole, recipe_data);
                    if (newtable) {
                        if (insert) {
                            g_hash_table_insert(rsroles, GINT_TO_POINTER(j + 1),
                                                trole);
                        } else {
                            g_hash_table_destroy(trole);
                        }
                    }
                }
            }

            xmlXPathFreeObject(task_nodes);
            xmlFree(role);
            xmlFree(wboard);
            xmlFree(id);
            xmlFree(owner);
        }
        xmlXPathFreeObject(recipe_nodes);
    }
    xmlXPathFreeObject(recipeset_nodes);

    recipe_nodes = get_node_set(new_xml_doc_ptr, NULL,
                                (xmlChar*)"/job/recipeSet/recipe");

    if (recipe_nodes == NULL) {
        g_printerr("Unable to find any recipes in newly created xml.\n");

        g_hash_table_destroy(posroles);
        xmlFreeDoc(template_xml_doc_ptr);
        xmlFreeDoc(new_xml_doc_ptr);

        return NULL;
    }

    GSList *jobmembers = get_recipe_members(app_data, recipe_nodes);
    recipeset_nodes = get_node_set(new_xml_doc_ptr, NULL,
                                            (xmlChar*)"/job/recipeSet");
    for (guint rs = 0; rs < recipeset_nodes->nodesetval->nodeNr; rs++) {
        xmlNodePtr rsnode = recipeset_nodes->nodesetval->nodeTab[rs];
        xmlXPathObjectPtr rs_recipe_nodes = get_node_set(new_xml_doc_ptr,
                                                rsnode, (xmlChar*)"recipe");
        GSList *recipemembers = get_recipe_members(app_data, rs_recipe_nodes);
        GHashTable *rsroles = g_hash_table_lookup(posroles,
                                                  GINT_TO_POINTER(rs));
        for (guint i = 0; i < rs_recipe_nodes->nodesetval->nodeNr; i++) {
            xmlNodePtr rnode = rs_recipe_nodes->nodesetval->nodeTab[i];

            xmlXPathObjectPtr task_nodes = get_node_set(new_xml_doc_ptr,
                    rnode, (xmlChar*)"task");
            if (task_nodes) {
                for (guint j = 0; j < task_nodes->nodesetval->nodeNr; j++) {
                    gboolean tmptable = FALSE;
                    xmlNodePtr tnode = task_nodes->nodesetval->nodeTab[j];
                    xmlNodePtr t_params = first_child_with_name(tnode, "params",
                                                                FALSE);
                    if (t_params == NULL) {
                        t_params = xmlNewChild(tnode, NULL, (xmlChar*)"params",
                                                 NULL);
                    }

                    GHashTable *lroles = g_hash_table_lookup(rsroles,
                                            GINT_TO_POINTER(j + 1));
                    if (lroles == NULL) {
                        lroles = g_hash_table_lookup(rsroles, GINT_TO_POINTER(0));
                    } else {
                        lroles = merge_roles(lroles, g_hash_table_lookup(
                                                       rsroles,
                                                       GINT_TO_POINTER(0)));
                        tmptable = TRUE;
                    }
                    g_hash_table_foreach(lroles, (GHFunc)add_r_params,
                                             t_params);
                    add_r_params("JOB_MEMBERS", jobmembers, t_params);
                    add_r_params("RECIPE_MEMBERS", recipemembers, t_params);
                    if (tmptable) {
                        g_hash_table_destroy(lroles);
                    }
                }
            }
            xmlXPathFreeObject(task_nodes);
        }
        g_slist_free(recipemembers);
        xmlXPathFreeObject(rs_recipe_nodes);
    }

    g_hash_table_destroy(posroles);
    g_slist_free(jobmembers);

    // Write out our new job.
    gchar *filename = g_build_filename (run_dir, "job.xml", NULL);
    put_doc (new_xml_doc_ptr, filename);
    g_free (filename);
    xmlXPathFreeObject(recipeset_nodes);
    xmlXPathFreeObject(recipe_nodes);
    xmlFreeDoc(template_xml_doc_ptr);
    xmlFreeDoc(new_xml_doc_ptr);

    return run_dir;
}

static gboolean remove_extra_recipes(gchar *id, RecipeData *recipe_data,
                                     GSList *idlist)
{
    if (g_slist_find_custom(idlist, id, (GCompareFunc)g_strcmp0) == NULL) {
        return TRUE;
    }
    return FALSE;
}

static void
parse_new_job (AppData *app_data)
{
    gchar *filename = g_build_filename (app_data->run_dir, "job.xml", NULL);

    // get xmldoc
    app_data->xml_doc = get_doc(filename);
    if (!app_data->xml_doc) {
        g_printerr ("Unable to parse %s\n", filename);
        g_free (filename);
        xmlFreeDoc(app_data->xml_doc);
        return;
    }

    // find recipe nodes
    xmlXPathObjectPtr recipe_node_ptrs = get_node_set(app_data->xml_doc, NULL,
            (xmlChar*)"/job/recipeSet/recipe");
    if (!recipe_node_ptrs) {
        g_printerr ("No <recipe> element in %s\n", filename);
        g_free (filename);
        xmlXPathFreeObject (recipe_node_ptrs);
        xmlFreeDoc(app_data->xml_doc);
        return;
    }

    GSList *idlist = NULL;
    for (gint i = 0; i < recipe_node_ptrs->nodesetval->nodeNr; i++) {
        xmlNodePtr node = recipe_node_ptrs->nodesetval->nodeTab[i];
        xmlChar *recipe_id = xmlGetNoNsProp(node, (xmlChar *)"id");

        idlist = g_slist_prepend(idlist, g_strdup((gchar*)recipe_id));
        RecipeData *recipe_data = g_hash_table_lookup(app_data->recipes,
                                                      recipe_id);
        if (recipe_data == NULL) {
            g_printerr ("Unable to find matching recipe id:%s in job.\n", recipe_id);
            g_free (filename);
            xmlFree (recipe_id);
            g_slist_free_full(idlist, g_free);
            xmlXPathFreeObject (recipe_node_ptrs);
            xmlFreeDoc(app_data->xml_doc);
            return;
        }
        recipe_data->recipe_node_ptr = node;
        recipe_data->recipe_id = (gint)g_ascii_strtoll((gchar *)recipe_id,
                                                        NULL, 0);
        xmlFree (recipe_id);

        // find task nodes
        xmlXPathObjectPtr task_nodes = get_node_set(app_data->xml_doc, node,
                                                    (xmlChar*)"task");
        if (!task_nodes) {
            g_printerr ("No <task> element(s) in %s\n", filename);
            g_free (filename);
            g_slist_free_full(idlist, g_free);
            xmlXPathFreeObject (recipe_node_ptrs);
            xmlXPathFreeObject (task_nodes);
            xmlFreeDoc(app_data->xml_doc);
            return;
        }
        recipe_data->tasks = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   xmlFree, NULL);

        // record each task in a hash table
        parse_task_nodes(task_nodes->nodesetval, recipe_data->tasks);
        xmlXPathFreeObject (task_nodes);
    }
    g_hash_table_foreach_remove(app_data->recipes,
                                (GHRFunc)&remove_extra_recipes,
                                idlist);
    g_slist_free_full(idlist, g_free);
    xmlXPathFreeObject (recipe_node_ptrs);
    g_free (filename);
}

static gboolean
callback_parse_verbose (const gchar *option_name, const gchar *value,
        gpointer user_data, GError **error)
{
    AppData *app_data = (AppData *) user_data;
    app_data->verbose++;
    return TRUE;
}

void
pretty_results (gchar* run_dir)
{
    gchar* cmdline = NULL;
    gchar *bootstrap_src = "/usr/share/restraint/client/bootstrap/bootstrap.min.css";
    gchar *bootstrap = NULL;
    gchar* job2html = "/usr/share/restraint/client/job2html.xml";
    gchar* jobxml = NULL;
    gchar* std_out = NULL;
    gchar *contents = NULL;
    gsize length;
    gchar* std_err = NULL;
    gint exitstat;
    GError *gerror = NULL;
    gchar* results_filename = NULL;
    gint results_fd;
    gssize results_len;
    gboolean success = FALSE;

    success = g_file_get_contents (bootstrap_src,
                                   &contents,
                                   &length,
                                   &gerror);
    if (!success) {
        g_printerr ("Error opening %s for reading: %s\n",
                    bootstrap_src,
                    gerror->message);
        goto cleanup;
    }

    bootstrap = g_build_filename (run_dir, "bootstrap.min.css", NULL);
    success = g_file_set_contents (bootstrap,
                                   contents,
                                   length,
                                   &gerror);
    if (!success) {
        g_printerr ("Error opening %s for writing: %s\n",
                    bootstrap,
                    gerror->message);
        goto cleanup;
    }

    jobxml = g_build_filename (run_dir, "job.xml", NULL);
    cmdline = g_strdup_printf("xsltproc %s %s", job2html, jobxml);

    if (!g_spawn_command_line_sync(cmdline, &std_out, &std_err, &exitstat,
                                   &gerror)) {
        g_printerr("cannot spawn '%s': %s\n", cmdline, gerror->message);
        goto cleanup;
    }
    if (exitstat != 0) {
        g_printerr("xsltproc error: %s\n", std_err);
        goto cleanup;
    }

    results_filename = g_build_filename (run_dir, "index.html", NULL);
    results_fd = g_open(results_filename,
                        O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
    if (results_fd == -1) {
        g_printerr("Error opening %s for writing\n", results_filename);
        goto cleanup;
    }
    length = strlen(std_out);
    results_len = write(results_fd, std_out, length);
    if(!g_close(results_fd, &gerror)) {
        g_printerr("cannot close %s: %s\n", results_filename, gerror->message);
        goto cleanup;
    }
    if (results_len != length) {
        g_printerr("Error writing to %s\n", results_filename);
    }

cleanup:
    if (gerror != NULL)
        g_error_free(gerror);
    if (results_filename != NULL)
        g_free (results_filename);
    if (jobxml != NULL)
        g_free (jobxml);
    if (bootstrap != NULL)
        g_free (bootstrap);
    if (contents != NULL)
        g_free (contents);
    if (cmdline != NULL)
        g_free (cmdline);
    if (std_out != NULL)
        g_free (std_out);
    if (std_err != NULL)
        g_free (std_err);
}

static gchar **
parse_host (const gchar *connect_uri)
{
    GMatchInfo  *match_info;
    GRegex      *regex;
    gchar      **ssh_destination;

    g_return_val_if_fail (connect_uri != NULL, NULL);

    /* [user@]hostname[:port][/ssh_port] */
    regex = g_regex_new ("^(([\\w\\.\\-]+)@)?([\\w\\.\\-]+)((:\\d+)?(/\\d+)?)?$", 0, 0, NULL);

    if (g_regex_match (regex, connect_uri, 0, &match_info)) {
        gchar *deprecated;

        ssh_destination = g_malloc (sizeof (gchar *) * 3);
        ssh_destination[0] = g_match_info_fetch (match_info, 1);
        ssh_destination[1] = g_match_info_fetch (match_info, 3);
        ssh_destination[2] = NULL;

        /* g_match_info_fetch returns NULL if match_num is out of range.
           This is indication of a wrong regular expression, missing groups. */
        g_return_val_if_fail (ssh_destination[0] != NULL && ssh_destination[1] != NULL, NULL);

        /* [:port][/ssh_port] are deprecated since 0.2.0
            If present, warn about these values. */
        deprecated = g_match_info_fetch (match_info, 4);

        if (deprecated != NULL && strlen (deprecated) > 0)
            g_printerr ("The [:port][/ssh_port] host arguments are deprecated and ignored, "
                        "see help for reference\n");

        g_free (deprecated);
    } else {
        g_printerr ("Malformed host: %s, see help for reference\n", connect_uri);
        ssh_destination = NULL;
    }

    g_match_info_free (match_info);
    g_regex_unref (regex);

    return ssh_destination;
}

static gboolean
add_recipe_host (const gchar *value,
                 AppData     *app_data,
                 guint        recipe_id)
{
    gboolean   parse_result;
    gchar     *connect_uri;
    gchar     *id;
    gchar    **args;
    gchar    **ssh_destination;

    g_return_val_if_fail (value != NULL && strlen (value) > 0, FALSE);
    g_return_val_if_fail (app_data != NULL, FALSE);

    args = g_strsplit (value, "=", 2);

    if (g_strv_length (args) == 1) {
        id = g_strdup_printf ("%u", recipe_id);
        connect_uri = args[0];
    } else {
        id = args[0];
        connect_uri = args[1];
    }

    ssh_destination = parse_host (connect_uri);
    parse_result = ssh_destination != NULL;

    if (parse_result) {
        RecipeData *recipe_data;

        recipe_data = new_recipe_data (app_data, id);
        recipe_data->connect_uri = g_strjoinv (NULL, ssh_destination);
        recipe_data->rhost = ssh_destination[1];

        g_free (ssh_destination[0]);
        g_free (ssh_destination);
    }

    g_free (id);
    g_free (connect_uri);
    g_free (args);

    return parse_result;
}

static void recipe_init(gchar *id, RecipeData *recipe_data,
                        gpointer *user_data)
{
    // Request to run the recipe.
    g_idle_add_full(G_PRIORITY_LOW,
                    run_recipe_handler,
                    recipe_data,
                    NULL);
}

int main(int argc, char *argv[]) {
    gchar *job = NULL;
    gboolean novalid = FALSE;
    gchar **hostarr = NULL;
    gint timeout = 5;

    AppData *app_data = g_slice_new0 (AppData);
    app_data->rsh_cmd = NULL;
    app_data->restraint_path = "restraintd";
    app_data->restraint_port = 0;
    app_data->max_retries = CONN_RETRIES;

    init_result_hash (app_data);
    app_data->recipes = g_hash_table_new_full(g_str_hash, g_str_equal,
            g_free, (GDestroyNotify)&restraint_free_recipe_data);

    GOptionEntry entries[] = {
        { "job", 'j', 0, G_OPTION_ARG_STRING, &job,
            "Run job from file", "FILE" },
        { "run", 'r', 0, G_OPTION_ARG_STRING, &app_data->run_dir,
            "Continue interrupted job from DIR", "DIR" },
        { "novalidate", 'n', 0, G_OPTION_ARG_NONE, &novalid,
            "Do not perform xml validation", NULL },
        { "host", 't', 0, G_OPTION_ARG_STRING_ARRAY, &hostarr,
            "Set host for a recipe with specific id.",
            "<recipe_id>=[<user>@]<host>" },
        { "conn-retries", 'c', 0, G_OPTION_ARG_INT, &app_data->max_retries,
            "Specify the number of reconnection retries.", NULL},
        { "port", 'p', 0, G_OPTION_ARG_INT, &app_data->restraint_port,
            "Port for restraintd HTTP server", "PORT" },
        { "verbose", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
            callback_parse_verbose, "Increase verbosity, up to three times.",
            NULL },
        { "rsh", 'e', 0, G_OPTION_ARG_STRING, &app_data->rsh_cmd,
            "Command to use make remote connection [Default: ssh -o ServerAliveInterval=60 -o ServerAliveCountMax=5].",
            NULL },
        { "restraint-path", 0, 0, G_OPTION_ARG_STRING, &app_data->restraint_path,
            "specify the restraintd to run on the remote machine", NULL },
        { "timeout", 0, 0, G_OPTION_ARG_INT, &timeout,
            "Specify timeout in minutes when rsh option not used [Default: 5].", NULL },
        { NULL }
    };
    GOptionGroup *option_group = g_option_group_new("main",
            "Application Options", "Various application related options",
            app_data, NULL);
    GOptionContext *context = g_option_context_new(NULL);

    g_option_context_set_summary (context,
            "Test harness for Beaker. Runs tasks according to a job \n"
            "and collects their results.");
    g_option_group_add_entries(option_group, entries);
    g_option_context_set_main_group (context, option_group);

    gboolean parse_succeeded = g_option_context_parse(context, &argc, &argv,
            &app_data->error);
    g_option_context_free(context);

    /* -t, --host option parsing */
    if (hostarr != NULL) {
        guint recipe_id = 1;

        for (gchar **host = hostarr; *host != NULL; host++) {
            if (add_recipe_host (*host, app_data, recipe_id++))
                continue;

            if (app_data->error == NULL)
                g_set_error (&app_data->error,
                             RESTRAINT_ERROR,
                             RESTRAINT_CMDLINE_ERROR,
                             "Failed to add recipe host.");

            goto cleanup;
        }
    }

    if (job) {
        // if template job is passed in use it to generate our job
        app_data->run_dir = copy_job_as_template(job, novalid, app_data);
    }
    if (app_data->rsh_cmd == NULL) {
        app_data->rsh_cmd = g_strdup_printf("ssh -o ServerAliveInterval=60 -o ServerAliveCountMax=%d",
                                            timeout);
    }
    if (!parse_succeeded || !app_data->run_dir) {
        if (app_data->error == NULL) {
            g_set_error(&app_data->error, RESTRAINT_ERROR,
                        RESTRAINT_CMDLINE_ERROR,
                        "Failed to parse commandline.\n"
                        "Try %s --help", argv[0]);
        }
        goto cleanup;
    }

    // Read in run_dir/job.xml
    parse_new_job (app_data);

    // If all tasks are finished then quit.
    if (tasks_finished(app_data->xml_doc, NULL, (xmlChar *) "//task")) {
        g_printerr ("All tasks are finished\n");
        goto cleanup;
    }

    signal(SIGPIPE, SIG_IGN);

    g_hash_table_foreach(app_data->recipes, (GHFunc)&recipe_init, NULL);

    app_data->regexes = register_path (app_data->regexes,
                                       "/start$",
                                        recipe_start_cb);
    app_data->regexes = register_path (app_data->regexes,
                                       "/recipes/[[:alnum:]]+/tasks/[[:digit:]]+/status$",
                                       tasks_status_cb);
    app_data->regexes = register_path (app_data->regexes,
                                       "/recipes/[[:alnum:]]+/tasks/[[:digit:]]+/results/$",
                                       tasks_results_cb);
    app_data->regexes = register_path (app_data->regexes,
                                       "/recipes/[[:alnum:]]+/watchdog$",
                                       watchdog_cb);
    app_data->regexes = register_path (app_data->regexes,
                                       "/recipes/[[:alnum:]]+/tasks/[[:digit:]]+/logs/",
                                       tasks_logs_cb);
    app_data->regexes = register_path (app_data->regexes,
                                       "/recipes/[[:alnum:]]+/tasks/[[:digit:]]+/results/[[:digit:]]+/logs/",
                                       tasks_logs_cb);
    // Create and enter the main loop
    app_data->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app_data->loop);

    gchar *filename = g_build_filename (app_data->run_dir, "job.xml", NULL);
    put_doc (app_data->xml_doc, filename);
    g_free (filename);

    // We're done.
    xmlFreeDoc(app_data->xml_doc);
    xmlCleanupParser();

    // convert job.xml to index.html
    pretty_results(app_data->run_dir);

cleanup:

    g_strfreev (hostarr);
    g_free (job);

    gboolean success = app_data->error == NULL;

    if (!success) {
        g_printerr ("%s [%s, %d]\n",
                    app_data->error->message,
                    g_quark_to_string (app_data->error->domain),
                    app_data->error->code);
    }

    restraint_free_app_data (app_data);

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
