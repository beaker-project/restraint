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
#include <libsoup/soup.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>

#include "recipe.h"
#include "param.h"
#include "role.h"
#include "task.h"
#include "metadata.h"
#include "utils.h"
#include "config.h"
#include "xml.h"
#include "beaker_harness.h"

GThreadPool *pool;
guint test_round;

GQuark restraint_recipe_parse_error_quark(void) {
    return g_quark_from_static_string("restraint-recipe-parse-error-quark");
}

static gchar *get_attribute(xmlNode *node, void *attribute) {
    gchar *result;

    xmlChar *text = xmlGetNoNsProp(node, (xmlChar *)attribute);
    result = g_strdup((gchar *)text);
    xmlFree(text);
    return result;
}

static xmlNodePtr find_recipe(xmlDocPtr doc) {
    xmlXPathObjectPtr recipe_nodes;
    xmlNodePtr result = NULL;

    recipe_nodes = get_node_set (doc, NULL, (xmlChar *)"//recipe[@id]");
    if (recipe_nodes) {
        result = recipe_nodes->nodesetval->nodeTab[0];
        xmlXPathFreeObject (recipe_nodes);
        return result;
    }
    recipe_nodes = get_node_set (doc, NULL, (xmlChar *)"//guestrecipe");
    if (recipe_nodes) {
        result = recipe_nodes->nodesetval->nodeTab[0];
        xmlXPathFreeObject (recipe_nodes);
        return result;
    }
    return NULL;
}

#define unrecognised(message, ...) g_set_error(error, RESTRAINT_RECIPE_PARSE_ERROR, \
        RESTRAINT_RECIPE_PARSE_ERROR_UNRECOGNISED, \
        message, ##__VA_ARGS__)

static Param *parse_param(xmlNode *param_node, GError **error) {
    g_return_val_if_fail(param_node != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    Param *param = restraint_param_new();

    xmlChar *param_name = xmlGetNoNsProp(param_node, (xmlChar *)"name");
    if (param_name == NULL) {
        unrecognised("'param' element without 'name' attribute");
        goto error;
    }
    param->name = g_strdup((gchar *)param_name);
    xmlFree(param_name);

    xmlChar *param_value = xmlGetNoNsProp(param_node, (xmlChar *)"value");
    if (param_value == NULL) {
        unrecognised("'param' element without 'value' attribute");
        goto error;
    }
    param->value = g_strdup((gchar *)param_value);
    xmlFree(param_value);
    return param;

error:
    restraint_param_free(param);
    return NULL;
}

static GList *parse_params(xmlNode *params_node, GError **error) {

    GError *tmp_error = NULL;
    GList *params = NULL;
    xmlNode *child = params_node->children;
    while (child != NULL) {
        if (child->type == XML_ELEMENT_NODE &&
                g_strcmp0((gchar *)child->name, "param") == 0) {
            Param *param = parse_param(child, &tmp_error);
            /* Empty param element is an error */
            if (param == NULL) {
                g_propagate_error(error, tmp_error);
                goto error;
            }
            params = g_list_prepend(params, param);
        }
        child = child->next;
    }
    params = g_list_reverse(params);
    return params;
error:
    g_list_free_full(params, (GDestroyNotify) restraint_param_free);
    return NULL;
}

static GPtrArray *parse_role_system(xmlNode *system_node, GError **error) {
    GPtrArray *systems;
    systems = g_ptr_array_new_with_free_func((GDestroyNotify) g_free);

    xmlNode *child = system_node->children;
    while (child != NULL) {
        if (child->type == XML_ELEMENT_NODE &&
           g_strcmp0((gchar *)child->name, "system") == 0) {
            xmlChar *system_value = xmlGetNoNsProp(child, (xmlChar *)"value");
            if (system_value == NULL) {
                unrecognised("'system' element without 'value' attribute");
                goto error;
            }
            g_ptr_array_add(systems, g_strdup((gchar *)system_value));
            xmlFree(system_value);
        }
        child = child->next;
    }
    g_ptr_array_add(systems, NULL);
    return systems;
error:
    g_ptr_array_free(systems, TRUE);
    return NULL;
}

static Role *parse_role(xmlNode *role_node, GError **error) {
    g_return_val_if_fail(role_node != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    GError *tmp_error = NULL;

    Role *role = restraint_role_new();

    xmlChar *role_value = xmlGetNoNsProp(role_node, (xmlChar *)"value");
    if (role_value == NULL) {
        unrecognised("'role' element without 'value' attribute");
        goto error;
    }
    role->value = g_strdup((gchar *)role_value);
    xmlFree(role_value);
    GPtrArray *systems = parse_role_system(role_node, &tmp_error);
    if (systems == NULL) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    role->systems = g_strjoinv (" ", (gchar **) systems->pdata);
    g_ptr_array_free(systems, TRUE);
    return role;

error:
    restraint_role_free(role);
    return NULL;
}

static GList *parse_roles(xmlNode *roles_node, GError **error) {

    GError *tmp_error = NULL;
    GList *roles = NULL;
    xmlNode *child = roles_node->children;
    while (child != NULL) {
        if (child->type == XML_ELEMENT_NODE &&
                g_strcmp0((gchar *)child->name, "role") == 0) {
            Role *role = parse_role(child, &tmp_error);
            /* Empty role element is an error */
            if (role == NULL) {
                g_propagate_error(error, tmp_error);
                goto error;
            }
            roles = g_list_prepend(roles, role);
        }
        child = child->next;
    }
    roles = g_list_reverse(roles);
    return roles;
error:
    g_list_free_full(roles, (GDestroyNotify) restraint_role_free);
    return NULL;
}

static Task *parse_task(xmlNode *task_node, Recipe *recipe, GError **error) {
    g_return_val_if_fail(task_node != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    GError *tmp_error = NULL;

    Task *task = restraint_task_new();
    g_return_val_if_fail(task != NULL, NULL);

    task->recipe = recipe;
    xmlChar *task_name = xmlGetNoNsProp(task_node, (xmlChar *)"name");
    task->name = g_strdup((gchar *)task_name);
    xmlFree (task_name);

    xmlChar *keepchanges = xmlGetNoNsProp(task_node, (xmlChar *)"keepchanges");
    if (g_strcmp0((gchar *)keepchanges, "yes") == 0) {
        task->keepchanges = TRUE;
    }
    xmlFree(keepchanges);

    xmlChar *task_id = xmlGetNoNsProp(task_node, (xmlChar *)"id");
    if (task_id == NULL) {
        unrecognised("<task/> without id");
        goto error;
    }
    task->task_id = g_strdup((gchar *)task_id);
    xmlFree(task_id);

    gchar *suffix = g_strconcat("tasks/", task->task_id, "/", NULL);
    task->task_uri = soup_uri_new_with_base(recipe->recipe_uri, suffix);
    g_free(suffix);

    xmlNode *fetch = first_child_with_name(task_node, "fetch", FALSE);
    if (fetch != NULL) {
        task->fetch_method = TASK_FETCH_UNPACK;
        xmlChar *url = xmlGetNoNsProp(fetch, (xmlChar *)"url");
        if (url == NULL) {
            unrecognised("Task %s has 'fetch' element with 'url' attribute",
                    task->task_id);
            goto error;
        }
        task->fetch.url = soup_uri_new((char *)url);

        if (task->fetch.url == NULL) {
            unrecognised("'%s' from task %s is not a valid url", url,
                         task->task_id);
            xmlFree(url);
            goto error;
        }
        xmlFree(url);

        xmlChar *ssl_verify = xmlGetNoNsProp(fetch, (xmlChar *)"ssl_verify");
        if (ssl_verify == NULL || g_strcmp0((char*)ssl_verify, "off") != 0) {
            task->ssl_verify = TRUE;
        } else {
            task->ssl_verify = FALSE;
        }
        xmlFree(ssl_verify);

        task->path = g_build_filename(task->recipe->base_path,
                task->fetch.url->host,
                task->fetch.url->path,
                task->fetch.url->fragment,
                NULL);
    } else {
        task->fetch_method = TASK_FETCH_INSTALL_PACKAGE;
        xmlNode *rpm = first_child_with_name(task_node, "rpm", FALSE);
        if (rpm == NULL) {
            unrecognised("Task %s has neither 'url' attribute nor 'rpm' element",
                    task->task_id);
            goto error;
        }
        xmlChar *rpm_name = xmlGetNoNsProp(rpm, (xmlChar *)"name");
        if (rpm_name == NULL) {
            unrecognised("Task %s has 'rpm' element without 'name' attribute",
                    task->task_id);
            goto error;
        }
        task->fetch.package_name = g_strdup((gchar *)rpm_name);
        xmlFree(rpm_name);
        xmlChar *rpm_path = xmlGetNoNsProp(rpm, (xmlChar *)"path");
        if (rpm_path == NULL) {
            unrecognised("Task %s has 'rpm' element without 'path' attribute",
                    task->task_id);
            goto error;
        }
        task->path = g_strdup((gchar *)rpm_path);
        xmlFree(rpm_path);
    }

    xmlNode *params_node = first_child_with_name(task_node, "params", FALSE);
    if (params_node != NULL) {
        task->params = parse_params(params_node, &tmp_error);
        /* params could be empty, but if parsing causes an error then fail */
        if (tmp_error != NULL) {
            g_propagate_prefixed_error(error, tmp_error,
                    "Task %s has ", task->task_id);
            goto error;
        }
    }

    xmlNode *roles_node = first_child_with_name(task_node, "roles", FALSE);
    if (roles_node != NULL) {
        task->roles = parse_roles(roles_node, &tmp_error);
        /* roles could be empty, but if parsing causes an error then fail */
        if (tmp_error != NULL) {
            g_propagate_prefixed_error(error, tmp_error,
                    "Task %s has ", task->task_id);
            goto error;
        }
    }

    xmlChar *status = xmlGetNoNsProp(task_node, (xmlChar *)"status");
    if (g_strcmp0((gchar *)status, "Running") == 0) {
        // We can't rely on the server because it "starts" the first task 
        // If we pay attention to that then we won't install the task
        // Update watchdog or install dependencies.
        g_warning ("Ignoring Server Running state\n");
        //task->started = TRUE;
    } else if (g_strcmp0((gchar *)status, "Completed") == 0 ||
            g_strcmp0((gchar *)status, "Aborted") == 0 ||
            g_strcmp0((gchar *)status, "Cancelled") == 0) {
        task->started = TRUE;
        task->finished = TRUE;
    }
    xmlFree(status);

    return task;

error:
    restraint_task_free(task);
    return NULL;
}

void restraint_recipe_update_roles(Recipe *recipe, xmlDoc *doc, GError **error) {
    g_return_if_fail(recipe != NULL);
    g_return_if_fail(doc != NULL);
    g_return_if_fail(error == NULL || *error == NULL);

    GError *tmp_error = NULL;

    xmlNodePtr recipe_node = find_recipe(doc);
    if (recipe_node == NULL) {
        unrecognised("<recipe/> element not found");
        return;
    }

    xmlNode *child = recipe_node->children;
    GList *tasks = recipe->tasks;
    while (child != NULL) {
        if (child->type == XML_ELEMENT_NODE &&
                g_strcmp0((gchar *)child->name, "roles") == 0) {
            GList *roles = parse_roles(child, &tmp_error);
            if (tmp_error != NULL) {
                g_propagate_prefixed_error(error, tmp_error,
                        "Recipe %s has ", recipe->recipe_id);
                return;
            }
            g_list_free_full(recipe->roles, (GDestroyNotify) restraint_role_free);
            recipe->roles = roles;
        }
        if (child->type == XML_ELEMENT_NODE &&
                g_strcmp0((gchar *)child->name, "task") == 0) {
            g_return_if_fail(tasks != NULL);
            Task *task = tasks->data;
            xmlNode *roles_node = first_child_with_name(child, "roles", FALSE);
            if (roles_node != NULL) {
                GList *roles = parse_roles(roles_node, &tmp_error);
                if (tmp_error != NULL) {
                    g_propagate_prefixed_error(error, tmp_error,
                            "Task %s has ", task->task_id);
                    return;
                }
                g_list_free_full(task->roles, (GDestroyNotify) restraint_role_free);
                task->roles = roles;
            }
            tasks = tasks->next;
        }
        child = child->next;
    }

    // We should have reached the end of the tasks list. If we didn't it means
    // there were somehow too few <task/> nodes in the XML.
    g_warn_if_fail(!tasks);
}

void restraint_recipe_free(Recipe *recipe) {
    g_return_if_fail(recipe != NULL);
    g_free(recipe->recipe_id);
    g_free(recipe->job_id);
    g_free(recipe->recipe_set_id);
    g_free(recipe->osdistro);
    g_free(recipe->osmajor);
    g_free(recipe->osvariant);
    g_free(recipe->osarch);
    g_free(recipe->owner);
    g_free(recipe->base_path);
    soup_uri_free(recipe->recipe_uri);
    g_list_free_full(recipe->tasks, (GDestroyNotify) restraint_task_free);
    g_list_free_full(recipe->pre_tasks, (GDestroyNotify) restraint_task_free);
    g_list_free_full(recipe->parallel_tasks, (GDestroyNotify) restraint_task_free);
    g_list_free_full(recipe->post_tasks, (GDestroyNotify) restraint_task_free);
    g_list_free_full(recipe->params, (GDestroyNotify) restraint_param_free);
    g_list_free_full(recipe->roles, (GDestroyNotify) restraint_role_free);
    g_slice_free(Recipe, recipe);
}

static void parallel_opt_parse(Param *param, guint *type)
{
    if (g_strcmp0((gchar *)param->name, "RSTRNT_PARALLEL") == 0) {
        if (g_strcmp0((gchar *)param->value, "parallel") == 0)
            *type = TASK_PARALLEL;
        else if (g_strcmp0((gchar *)param->value, "post") == 0)
            *type = TASK_POST;
        else
            *type = TASK_PRE;
    }
}

static Recipe *
recipe_parse (xmlDoc *doc, SoupURI *recipe_uri, GError **error, gchar **cfg_file)
{
    g_return_val_if_fail(doc != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    GError *tmp_error = NULL;
    gint i = 0;
    Recipe *result = g_slice_new0(Recipe);

    xmlNode *job = xmlDocGetRootElement(doc);
    if (job == NULL) {
        unrecognised("Invalid XML Document");
        goto error;
    }

    xmlNodePtr recipe = find_recipe(doc);
    if (recipe == NULL) {
        unrecognised("<recipe/> element not found");
        goto error;
    }

    result->job_id = get_attribute(recipe, "job_id");
    result->recipe_set_id = get_attribute(recipe, "recipe_set_id");
    result->recipe_id = get_attribute(recipe, "id");
    result->osarch = get_attribute(recipe, "arch");
    result->osdistro = get_attribute(recipe, "distro");
    result->osmajor = get_attribute(recipe, "family");
    result->osvariant = get_attribute(recipe, "variant");
    result->owner = get_attribute(job, "owner");

    if (recipe_uri == NULL) {
        gchar *tmp_str;

        tmp_str = get_attribute (job, "checkpoint_file");
        *cfg_file = g_build_filename (VAR_LIB_PATH, tmp_str, NULL);
        g_free (tmp_str);

        // Hack to make soup_uri_new happy.
        tmp_str = g_strdup_printf ("http://localhost/recipes/%s/", result->recipe_id);
        recipe_uri = soup_uri_new (tmp_str);
        g_free (tmp_str);
    }
    result->recipe_uri = recipe_uri;

    // Gather the location in which to install tasks
    result->base_path = get_install_dir(INSTALL_CONFIG_FILE, &tmp_error);
    if (tmp_error != NULL) {
        g_warning("* Fail getting task install path. Using default. Error: %s",
                  tmp_error->message);
        g_clear_error(&tmp_error);
    }


    GList *tasks = NULL;
    GList *pre_tasks = NULL;
    GList *post_tasks = NULL;
    GList *parallel_tasks = NULL;
    guint parallel_type = TASK_PRE; //default pre tasks
    xmlNode *child = recipe->children;
    while (child != NULL) {
        if (child->type == XML_ELEMENT_NODE &&
                g_strcmp0((gchar *)child->name, "task") == 0) {
            Task *task = parse_task(child, result, &tmp_error);
            if (task == NULL) {
                g_propagate_error(error, tmp_error);
                goto error;
            }
            /* link task to recipe for additional attributes */
            task->recipe = result;
            task->order = i++*2;
            parallel_type = TASK_PRE;
            g_list_foreach(task->params, (GFunc) parallel_opt_parse, &parallel_type);
            if (parallel_type == TASK_PRE) {
                pre_tasks = g_list_prepend(pre_tasks, task);
                g_print("pre task %s added\n", task->task_id);
            }
            else if (parallel_type == TASK_POST) {
                post_tasks = g_list_prepend(post_tasks, task);
                g_print("post task %s added\n", task->task_id);
            }
            else {
                parallel_tasks = g_list_prepend(parallel_tasks, task);
                g_print("parallel task %s added\n", task->task_id);
            }
            tasks = g_list_prepend(tasks, task);
        } else
        if (child->type == XML_ELEMENT_NODE &&
                g_strcmp0((gchar *)child->name, "params") == 0) {
            result->params = parse_params(child, &tmp_error);
            /* params could be empty, but if parsing causes an error then fail */
            if (tmp_error != NULL) {
                g_propagate_prefixed_error(error, tmp_error,
                                 "Recipe %s has ", result->recipe_id);
                goto error;
            }
        } else
        if (child->type == XML_ELEMENT_NODE &&
                g_strcmp0((gchar *)child->name, "roles") == 0) {
            result->roles = parse_roles(child, &tmp_error);
            /* roles could be empty, but if parsing causes an error then fail */
            if (tmp_error != NULL) {
                g_propagate_prefixed_error(error, tmp_error,
                                 "Recipe %s has ", result->recipe_id);
                goto error;
            }
        }
        child = child->next;
    }
    tasks = g_list_reverse(tasks);
    parallel_tasks = g_list_reverse(parallel_tasks);
    pre_tasks = g_list_reverse(pre_tasks);
    post_tasks = g_list_reverse(post_tasks);

    result->tasks = tasks;
    result->pre_tasks = pre_tasks;
    result->post_tasks = post_tasks;
    result->parallel_tasks = parallel_tasks;
    return result;

error:
    restraint_recipe_free(result);
    return NULL;
}

static gboolean fetch_retry (gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    g_clear_error(&app_data->error);
    app_data->state = RECIPE_FETCH;

    return FALSE;
}

static void
fetch_completed(GError *error, xmlDoc *doc, gpointer user_data)
{
    AppData *app_data = (AppData *)user_data;

    if (error) {
        g_warn_if_fail(!doc);
        if (app_data->fetch_retries < RECIPE_FETCH_RETRIES) {
            g_print("* RETRY [%d]**:%s\n", ++app_data->fetch_retries,
                    error->message);
            g_timeout_add_seconds (RECIPE_FETCH_INTERVAL, fetch_retry, app_data);
        } else {
            g_propagate_prefixed_error(&app_data->error, error,
                    "While fetching recipe XML: ");
            /* Set us back to idle so we can accept a valid recipe */
            app_data->state = RECIPE_COMPLETE;
        }
        return;
    }

    g_warn_if_fail(!app_data->recipe_xmldoc);
    app_data->recipe_xmldoc = doc;
    app_data->state = RECIPE_PARSE;
}

void
restraint_recipe_parse_stream (GInputStream *stream, gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;

    restraint_xml_parse_from_stream(stream, app_data->recipe_url, fetch_completed, app_data);
}

void thread_loop_run(gpointer push_data, gpointer user_data)
{
	AppData *app_data = (AppData *) user_data;
	ThreadData *thrdata = g_slice_new0(ThreadData);
	thrdata->app_data = app_data;
	GSource *source;
	GMainContext *ctxt;
	GMainLoop *loop;
	static GMutex task_sync;

	source = g_idle_source_new();
	ctxt = g_main_context_new();
	g_source_attach(source, ctxt);
	thrdata->ctxt = ctxt;
	loop = g_main_loop_new(ctxt, FALSE);
	thrdata->loop = loop;

	g_mutex_lock(&task_sync);
	if (curr_task == NULL) {
		g_mutex_unlock(&task_sync);
		return;
        }

	thrdata->task = curr_task->data;
	curr_task = g_list_next(curr_task);
	g_mutex_unlock(&task_sync);

	g_source_set_callback(source, task_handler, thrdata, NULL);
	g_main_loop_run(loop);
}

void thread_loop_stop(gpointer user_data)
{
	ThreadData *thrdata = (ThreadData *) user_data;
	g_main_loop_quit(thrdata->loop);
}

static void restraint_multithread_push(gpointer user_data, GList *tasks, GThreadPool *pool, guint thr_num)
{
	guint i;
	AppData *app_data = (AppData *) user_data;

	curr_task = tasks;
	if (g_list_length(tasks) == 0)
	{
		if (test_round == 2)
			app_data->state = RECIPE_COMPLETE;
		else {
			app_data->state = RECIPE_RUN;
			test_round++;
		}
		return;
	} else
		app_data->state = RECIPE_RUNNING;

	for (i = 0; i < thr_num; i++) {
		g_thread_pool_push(pool, app_data, NULL);
	}
}

// /proc/loadavg's 1min, /proc/stat's iowait, /proc/meminfo's available
gboolean load_eval()
{
    GFile *file;
    GFileInputStream *fis;
    GError *tmp_err;
    gchar **result;
    gchar *total, *available;
    gchar buf[2048];
    gint i, j;
    gint64 sum = 0, curr_sum, delta_sum;
    gint64 idle, curr_idle, delta_idle;
    gint64 iowait, curr_iowait, delta_iowait;
    gfloat loadavg1min;
    guint64 tot;
    guint64 ava;
    gboolean overload = false;

    file = g_file_new_for_path("/proc/loadavg");
    fis = g_file_read(file, NULL, &tmp_err);
    g_input_stream_read(G_INPUT_STREAM(fis), buf, 1024, NULL, &tmp_err);
    result = g_strsplit_set(buf , " \n", -1);
    loadavg1min = g_ascii_strtod(*result, NULL);
    g_print("last 1min loadavg is %.2f, nproc is %lu\n", loadavg1min, nproc);
    if (loadavg1min - nproc > -2) // less than 2 cores available
    {
	    overload = true;
	    goto clean;
    }

    g_strfreev(result);
    g_input_stream_close(G_INPUT_STREAM(fis), NULL, NULL);
    g_object_unref(fis);
    g_object_unref(file);

    file = g_file_new_for_path("/proc/meminfo");
    fis = g_file_read(file, NULL, &tmp_err);
    g_input_stream_read(G_INPUT_STREAM(fis), buf, 1024, NULL, &tmp_err);
    result = g_strsplit_set(buf , ":\n", -1);
    total = *(result + 1);
    g_strstrip(total);
    total = *g_strsplit(total , " ", -1);
    available = *(result + 5);
    g_strstrip(available);
    available = *g_strsplit(available , " ", -1);
    tot = g_ascii_strtoull(total, NULL, BASE10);
    ava = g_ascii_strtoull(available, NULL, BASE10);

    gfloat percent = (gfloat)ava / (gfloat)tot;
    g_print("total mem is                    %lu\n", tot);
    g_print("available mem is                %lu\n", ava);
    g_print("available mem percent is        %.2f\n", percent);

    if ( percent < 0.4 || ava < 2097152) // available memory lower than 40% or less than 2G
	overload = true;

clean:
    g_strfreev(result);
    g_input_stream_close(G_INPUT_STREAM(fis), NULL, NULL);
    g_object_unref(fis);
    g_object_unref(file);

    if (overload)
        return true;

    for (i = 0; i < 4; i++) // read stat every second for a total of four times, so finally do it
    {

        file = g_file_new_for_path("/proc/stat");
        fis = g_file_read(file, NULL, NULL);
        g_input_stream_read(G_INPUT_STREAM(fis), buf, 1024, NULL, NULL);
        result = g_strsplit_set(buf , " \n", -1);
        result += 5;// 4th idle
        curr_idle = g_ascii_strtoull(*result, NULL, BASE10);
        result += 1;// 5th iowait
        curr_iowait = g_ascii_strtoull(*result, NULL, BASE10);

        result -= 6;
        g_strfreev(result);

        result = g_strsplit_set(buf, " \n", -1);
        result += 2;// 1st-10th user, nice, system, idle, iowait, irq, softirq, stral, guest, guest_nice
        curr_sum = 0;
        for (j = 0; j < 10; j ++)
            curr_sum += g_ascii_strtoull(*result++, NULL, BASE10);
        result -= 12;

        if (sum == 0)// initialization
            goto done;

        delta_iowait = curr_iowait - iowait;
        delta_sum = curr_sum - sum;
        delta_idle = curr_idle - idle;

        g_print("last1s cpu sum %ld\n", delta_sum);
        g_print("last1s iowait %ld\n", delta_iowait);
        g_print("last1s idle %ld\n", delta_idle);

        if ((gfloat)delta_iowait / (gfloat)delta_sum > 0.4) // iowait percent more than 40%
        {
	    overload = true;
	    goto clean;
        }
        if ((gfloat)delta_idle / (gfloat)delta_sum < 0.3) // idle/available cpu time less than 30%
	{
	    overload = true;
	    goto clean;
	}

done:
        iowait = curr_iowait;
        sum = curr_sum;
        idle = curr_idle;

        g_strfreev(result);
        g_input_stream_close(G_INPUT_STREAM(fis), NULL, NULL);
        g_object_unref(fis);
        g_object_unref(file);

        g_usleep(G_USEC_PER_SEC);
    }

    return false;
}

gboolean
recipe_handler (gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    SoupURI *recipe_uri = NULL;
    GString *message = g_string_new(NULL);
    gboolean result = TRUE;

    switch (app_data->state) {
        case RECIPE_FETCH:
            if (!app_data->stdin && recipe_wait_on_beaker (app_data->recipe_url, "* Recipe fetch"))
                break;

            g_string_printf(message, "* Fetching recipe: %s\n", app_data->recipe_url);
            app_data->state = RECIPE_FETCHING;
            restraint_xml_parse_from_url(soup_session, app_data->recipe_url,
                    fetch_completed, app_data);
            // fetch_completed callback will move us to the next state
            break;
        case RECIPE_PARSE:
            g_string_printf(message, "* Parsing recipe\n");
            if (app_data->recipe_url)
                recipe_uri = soup_uri_new(app_data->recipe_url);
            app_data->recipe = recipe_parse(app_data->recipe_xmldoc, recipe_uri,
                                            &app_data->error, &app_data->config_file);
            if (app_data->recipe && ! app_data->error) {
                app_data->tasks = app_data->recipe->tasks;
                app_data->pre_tasks = app_data->recipe->pre_tasks;
                app_data->post_tasks = app_data->recipe->post_tasks;
                app_data->parallel_tasks = app_data->recipe->parallel_tasks;
                app_data->state = RECIPE_RUN;
            } else {
                app_data->state = RECIPE_COMPLETE;
            }
            guint max_thr = nproc;
            if (max_thr > 32)
                max_thr = 32;
	    pool = g_thread_pool_new(thread_loop_run, app_data, max_thr, FALSE, NULL);
            break;
        case RECIPE_RUN:
            if (app_data->recipe_url) {
                restraint_config_set (app_data->config_file,
                                      "restraint",
                                      "recipe_url",
                                      NULL,
                                      G_TYPE_STRING,
                                      app_data->recipe_url);
            }

            if (test_round == 0)
	        restraint_multithread_push(app_data, app_data->pre_tasks, pool, 1);
            else if (test_round == 1)
	        restraint_multithread_push(app_data, app_data->parallel_tasks, pool, 32);
            else if (test_round == 2)
	        restraint_multithread_push(app_data, app_data->post_tasks, pool, 1);
            g_string_printf(message, "* Running recipe, %d\n", test_round);
            break;
        case RECIPE_RUNNING:
	    result = FALSE;
            break;
        case RECIPE_COMPLETE:
            if (app_data->error) {
                g_string_printf(message, "* WARNING **: %s\n", app_data->error->message);
            } else {
                g_string_printf(message, "* Finished recipe\n");
                if (app_data->close_message && app_data->message_data) {
                    app_data->close_message (app_data->message_data);
                }
            }
            if (app_data->recipe_xmldoc) {
                xmlFreeDoc(app_data->recipe_xmldoc);
                app_data->recipe_xmldoc = NULL;
            }
            // free current recipe
            if (app_data->recipe) {
              restraint_recipe_free(app_data->recipe);
              app_data->recipe = NULL;
              g_free (app_data->recipe_url);
              app_data->recipe_url = NULL;
            }

            // We are done. remove ourselves so we can run another recipe.
            app_data->state = RECIPE_IDLE;
            result = FALSE;
            if (app_data->io_handler_id != 0) {
                g_source_remove (app_data->io_handler_id);
                app_data->io_handler_id = 0;
            }
            g_cancellable_reset (app_data->cancellable);
            break;
        default:
            break;
    }

    // write message out to stderr
    if (message->len > 0)
        g_printerr ("%s", message->str);

    g_string_free(message, TRUE);
    return result;
}

void
recipe_handler_finish (gpointer user_data)
{
    AppData *app_data = (AppData *) user_data;
    ClientData *client_data = app_data->message_data;

    if (client_data) {
        if (app_data->error) {
         soup_message_set_status_full (client_data->client_msg,
                                       SOUP_STATUS_BAD_REQUEST,
                                       app_data->error->message);
        } else {
            soup_message_set_status (client_data->client_msg, SOUP_STATUS_OK);
        }
        soup_message_body_append (client_data->client_msg->response_body,
                                  SOUP_MEMORY_STATIC,
                                  "\r\n--cut-here\n", 13);
        soup_server_unpause_message (client_data->server, client_data->client_msg);
    }
    g_clear_error (&app_data->error);
}

/*
 * Waits for 60 seconds if Beaker's recipe health status is not GOOD.
 *
 * Returns TRUE if wait was done, FALSE otherwise.
 */
gboolean
recipe_wait_on_beaker (const gchar *recipe_url, const gchar *state_tag)
{
    g_return_val_if_fail (recipe_url != NULL, FALSE);
    g_return_val_if_fail (state_tag != NULL, FALSE);
    g_return_val_if_fail (BKR_ENV_EXISTS (), FALSE);

    if (BKR_RECIPE_IS_HEALTHY (recipe_url))
        return FALSE;

    g_printerr ("%s", state_tag);
    g_printerr (": Waiting on Beaker for 60 seconds\n");
    g_usleep (60 * G_USEC_PER_SEC);

    return TRUE;
}
