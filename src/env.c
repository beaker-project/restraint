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

#include "cmd_utils.h"
#include "env.h"
#include "param.h"

static void get_recipe_members(GSList **hosts, GList *rolehosts)
{
    for (GList *t_param = rolehosts; t_param != NULL;
                    t_param = g_list_next(t_param)) {
      Param *param = t_param->data;
      if (param->value != NULL) {
          gchar **phosts = g_strsplit(param->value, " ", -1);
          for (gchar **chost = phosts; *chost != NULL; chost++) {
            if (g_slist_find_custom(*hosts, *chost,
                        (GCompareFunc)g_strcmp0) == NULL) {
              *hosts = g_slist_prepend(*hosts, g_strdup(*chost));
            }
          }
          g_strfreev(phosts);
      }
    }
}

static void
array_add (GPtrArray *array, const gchar *prefix, const gchar *variable, const gchar *value)
{
    if (value) {
        if (prefix) {
            g_ptr_array_add (array, g_strdup_printf ("%s%s=%s", prefix, variable, value));
        } else {
            g_ptr_array_add (array, g_strdup_printf ("%s=%s", variable, value));
        }
    }
}

struct env_remove_t {
    gchar *varname;
    GPtrArray *env;
};

static void remove_env_var(gchar *evar, struct env_remove_t *er)
{
    if (evar != NULL) {
        gchar **envval = g_strsplit(evar, "=", 2);
        if (g_strcmp0(envval[0], er->varname) == 0) {
            g_ptr_array_remove_fast(er->env, evar);
        }
        g_strfreev(envval);
    }
}

static void build_param_var(Param *param, GPtrArray *env) {
    struct env_remove_t er = { .varname = param->name, .env = env };
    g_ptr_array_foreach(env, (GFunc)remove_env_var, &er);
    g_ptr_array_add(env, g_strdup_printf("%s=%s", param->name, param->value));
}

void build_env(gchar *restraint_url, guint port, Task *task, AppData *app_data) {
    GPtrArray *env = g_ptr_array_new_with_free_func (g_free);
    GError *error = NULL;

    if (task->metadata != NULL) {
        g_slist_foreach(task->metadata->envvars, (GFunc) build_param_var, env);
    }
    g_list_foreach(task->recipe->roles, (GFunc) build_param_var, env);
    g_list_foreach(task->roles, (GFunc) build_param_var, env);

    GSList *rmembers = NULL;
    get_recipe_members(&rmembers, task->recipe->roles);
    get_recipe_members(&rmembers, task->roles);

    gchar **hostarr = g_new0(gchar *, g_slist_length(rmembers) + 1);
    gchar **p = hostarr;
    gchar *hoststr = NULL;
    gchar *index = NULL;

    for (GSList *host = rmembers; host != NULL; host = g_slist_next(host)) {
      *p++ = host->data;
    }
    hoststr = g_strjoinv(" ", hostarr);
    array_add(env, NULL, "RECIPE_MEMBERS", hoststr);
    g_free(hoststr);
    g_strfreev(hostarr);
    g_slist_free(rmembers);

    gchar *prefix = ENV_PREFIX;
    if (task->rhts_compat == TRUE) {
        array_add (env, NULL, "RESULT_SERVER", "LEGACY");
        array_add (env, NULL, "SUBMITTER", task->recipe->owner);
        array_add (env, NULL, "JOBID", task->recipe->job_id);
        array_add (env, NULL, "RECIPESETID", task->recipe->recipe_set_id);
        array_add (env, NULL, "RECIPEID", task->recipe->recipe_id);
        array_add (env, NULL, "RECIPETESTID", task->task_id);
        array_add (env, NULL, "TASKID", task->task_id);
        array_add (env, NULL, "DISTRO", task->recipe->osdistro);
        array_add (env, NULL, "VARIANT", task->recipe->osvariant);
        array_add (env, NULL, "FAMILY", task->recipe->osmajor);
        array_add (env, NULL, "ARCH", task->recipe->osarch);
        array_add (env, NULL, "TESTNAME", task->name);
        array_add (env, NULL, "TESTPATH", task->path);
        g_ptr_array_add(env, g_strdup_printf("MAXTIME=%" G_GINT64_FORMAT, task->remaining_time));
        g_ptr_array_add(env, g_strdup_printf("REBOOTCOUNT=%" G_GUINT64_FORMAT, task->reboots));
        g_ptr_array_add(env, g_strdup_printf("TASKORDER=%d", task->order));
    }
    // beakerlib checks TESTID to run BEAKERLIB_COMMAND_REPORT_RESULT, so
    // export TESTID even if not in rhts_compat mode
    array_add (env, NULL, "TESTID", task->task_id);
    g_ptr_array_add(env, g_strdup_printf("HARNESS_PREFIX=%s", ENV_PREFIX));
    gchar *recipe_url = g_strdup_printf ("%s/recipes/%s", restraint_url, task->recipe->recipe_id);
    array_add (env, prefix, "RECIPE_URL", recipe_url);
    g_free (recipe_url);
    array_add (env, prefix, "OWNER", task->recipe->owner);
    array_add (env, prefix, "JOBID", task->recipe->job_id);
    array_add (env, prefix, "RECIPESETID", task->recipe->recipe_set_id);
    array_add (env, prefix, "RECIPEID", task->recipe->recipe_id);
    array_add (env, prefix, "TASKID", task->task_id);
    array_add (env, prefix, "OSDISTRO", task->recipe->osdistro);
    array_add (env, prefix, "OSMAJOR", task->recipe->osmajor);
    array_add (env, prefix, "OSVARIANT", task->recipe->osvariant);
    array_add (env, prefix, "OSARCH", task->recipe->osarch);
    array_add (env, prefix, "TASKNAME", task->name);
    array_add (env, prefix, "TASKPATH", task->path);
    g_ptr_array_add(env, g_strdup_printf("%sMAXTIME=%" G_GINT64_FORMAT, prefix, task->remaining_time));
    g_ptr_array_add(env, g_strdup_printf("%sREBOOTCOUNT=%" G_GUINT64_FORMAT, prefix, task->reboots));
    //g_ptr_array_add(env, g_strdup_printf("%sLAB_CONTROLLER=", prefix));
    g_ptr_array_add(env, g_strdup_printf("%sTASKORDER=%d", prefix, task->order));
    // HOME, LANG and TERM can be overriden by user by passing it as recipe or task params.
    g_ptr_array_add(env, g_strdup_printf("HOME=/root"));
    g_ptr_array_add(env, g_strdup_printf("TERM=vt100"));
    g_ptr_array_add(env, g_strdup_printf("LANG=en_US.UTF-8"));
    g_ptr_array_add(env, g_strdup_printf("PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin"));

    //index of task in the list.
    index = g_strdup_printf("%d", g_list_index(app_data->tasks, task));
    array_add (env, prefix, "INDEX", index);

    // Override with recipe level params
    g_list_foreach(task->recipe->params, (GFunc) build_param_var, env);
    // Override with task level params
    g_list_foreach(task->params, (GFunc) build_param_var, env);
    // Leave four NULL slots for PLUGIN variables.
    g_ptr_array_add(env, NULL);
    g_ptr_array_add(env, NULL);
    g_ptr_array_add(env, NULL);
    g_ptr_array_add(env, NULL);
    // This terminates the array
    g_ptr_array_add(env, NULL);
    task->env = env;
    // To make it easy for user's to run restraintd commands, these environment variables
    // need to be made conveniently available to user.
    update_env_file(ENV_PREFIX, restraint_url, task->recipe->recipe_id, task->task_id,
                    port, &error);
    if (error) {
        g_printerr("%s [%s, %d]\n", error->message,
                g_quark_to_string(error->domain), error->code);
        g_clear_error(&error);
    }


}
