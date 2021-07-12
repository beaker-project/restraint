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

#ifndef _RESTRAINT_SERVER_H
#define _RESTRAINT_SERVER_H

#include <libxml/tree.h>

#define VAR_LIB_PATH "/var/lib/restraint"
#define PLUGIN_SCRIPT "/usr/share/restraint/plugins/run_plugins"
#define TASK_PLUGIN_SCRIPT "/usr/share/restraint/plugins/run_task_plugins"
#define PLUGIN_DIR "/usr/share/restraint/plugins"

#define LOG_UPLOAD_INTERVAL 15  /* Seconds */
#define LOG_UPLOAD_MIN_INTERVAL 3  /* Seconds */
#define LOG_UPLOAD_MAX_INTERVAL 60  /* Seconds */

typedef enum {
  ABORTED_NONE,
  ABORTED_RECIPE,
  ABORTED_TASK,
} StateAborted;

typedef struct RstrntServerAppData {
  RecipeSetupState state;
  guint port;
  guint recipe_handler_id;
  guint task_handler_id;
  gchar *recipe_url;
  xmlDoc *recipe_xmldoc;
  Recipe *recipe;
  GList *tasks;
  GList *pre_tasks;
  GList *post_tasks;
  GList *parallel_tasks;
  GList *waiting_tasks; // list of waiting tasks *
  GError *error;
  gchar *config_file;
  gchar *restraint_url;
  GCancellable *cancellable;
  QueueMessage queue_message;
  CloseMessage close_message;
  gpointer message_data;
  guint finished_handler_id;
  guint io_handler_id;
  GIOChannel *io_chan;
  StateAborted aborted;
  guint fetch_retries;
  gboolean stdin;
  guint last_signal;
  guint uploader_source_id; /* Event source ID for log uploader */
  guint uploader_interval; /* In seconds. 0 disables the log manager */
} AppData;

#endif
