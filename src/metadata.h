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

#ifndef _RESTRAINT_METADATA_H
#define _RESTRAINT_METADATA_H

#include <gio/gio.h>

typedef struct {
    gchar *name;
    /* entry_point, defaults to make run */
    gchar *entry_point;
    /* List of dependencies */
    GSList *dependencies;
    /* List of soft or optional dependencies */
    GSList *softdependencies;
    /* List of repository dependencies */
    GSList *repodeps;
    /* List of environment variables, encoded as 'Param' objects */
    GSList *envvars;
    /* maximum time task is allowed to run before being killed */
    gint64 max_time;
    /* The task can request that no localwatchdog be used. Used for reservesys task */
    gboolean nolocalwatchdog;
    /* Use pty when running task */
    gboolean use_pty;
    /* List of conflict keywords */
    GSList *conflict_kywds;
} MetaData;

typedef void (*metadata_cb) (gpointer user_data, GError *error);

MetaData* restraint_parse_metadata (gchar *filename, gchar *locale, GError **error);
MetaData* restraint_parse_testinfo (gchar *filename, GError **error);
void restraint_metadata_free (MetaData *metadata);
gboolean restraint_get_metadata(char *path, char *osmajor, MetaData **metadata,
                                GCancellable *cancellable,
                                metadata_cb finish_cb, GIOFunc io_callback,
                                void *user_data);
#endif
