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
#include <glib/gstdio.h>
#include <errno.h>

#include <stdlib.h>
#include <string.h>
#include "errors.h"

#include "utils.h"
#include "cmd_utils.h"

/*
 * Like getenv, but checks for HARNESS_PREFIX
 */
const gchar *
rstrnt_getenv (const gchar *name)
{
    const gchar *prefix;
    const gchar *value;
    gchar       *long_name;

    prefix = g_getenv ("HARNESS_PREFIX");

    if (prefix == NULL || strlen (prefix) == 0)
        return g_getenv (name);

    long_name = g_strconcat (prefix, name, NULL);
    value = g_getenv (long_name);

    g_free (long_name);

    return value;
}

void get_env_vars_and_format_ServerData(ServerData *s_data)
{
    // See if the user has already defined the env vars.
    s_data->server_recipe = g_strdup(get_recipe_url());
    s_data->task_id = g_strdup(get_taskid());

}

/* alter_envvar_from_file()
 *
 * To allow 'C' code to read in and 'export|unset' environment variables
 * from the CMD_ENV_FILE_FORMAT.
 */
static void
alter_envvar_from_file(guint port, gboolean set, GError **error) {
    int i;
    g_autofree gchar *msgbuf = NULL;
    g_autofree gchar *filename = get_envvar_filename(port);

    if ((filename == NULL) || (!g_file_test(filename, G_FILE_TEST_EXISTS))) {
        g_set_error (error, RESTRAINT_ERROR,
                     RESTRAINT_MISSING_FILE,
                     "File %s not present",
                     (filename == NULL) ? "None" : filename);
        return;
    }
    if (!g_file_get_contents(filename, &msgbuf, NULL, error))
    {
        return;
    }
    gchar **myarr = g_strsplit(msgbuf, "\n", -1);

    for (i=0; myarr[i] != NULL; i++) {
        if (strlen(myarr[i]) != 0) {
            gchar **my_vars = g_strsplit(myarr[i], "=", 2);
            if ((my_vars[0] != NULL) && (strlen(my_vars[0]) != 0)) {
                if (set) {
                    g_setenv(my_vars[0], my_vars[1], TRUE);
                } else {
                    g_unsetenv(my_vars[0]);
                }
            }
            g_strfreev(my_vars);
        }
    }
    g_strfreev(myarr);
}

void
set_envvar_from_file(guint port, GError **error) {
    alter_envvar_from_file(port, TRUE, error);
}

void
unset_envvar_from_file(guint port, GError **error) {
    alter_envvar_from_file(port, FALSE, error);
}

void get_env_vars_from_file(ServerData *s_data, GError **error)
{
    // Set environment variables from file if it exists.
    set_envvar_from_file(s_data->port, error);
    // Fetch env vars
    get_env_vars_and_format_ServerData(s_data);

}

void
format_server_string(ServerData *s_data,
                  void (*format_server)(ServerData *s_data),
                  GError **error)
{

    if (s_data->port != 0) {
        get_env_vars_from_file(s_data, error);
    } else {
        get_env_vars_and_format_ServerData(s_data);
    }
    format_server(s_data);
}

void
clear_server_data(ServerData *s_data)
{
    if (s_data->server) {
        g_free(s_data->server);
    }
    if (s_data->server_recipe){
        g_free(s_data->server_recipe);
    }
    if (s_data->task_id){
        g_free(s_data->task_id);
    }
}

void cmd_usage(GOptionContext *context) {
    gchar *usage_str = g_option_context_get_help(context, FALSE, NULL);
    g_print("%s", usage_str);
    g_free(usage_str);
}
