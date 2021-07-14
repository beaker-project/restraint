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

#include <fcntl.h>
#include <gio/gunixoutputstream.h>
#include "logging.h"
#include "task.h"
#include "beaker_harness.h"

/* FIX: Use /var/lib/restraint/logs instead. Needs SELinux policy
   updates. */
#ifndef LOG_MANAGER_DIR
#define LOG_MANAGER_DIR "/var/tmp/restraintd/logs"
#endif

struct _RstrntLogManager
{
    GObject parent_instance;

    GHashTable *logs;
};

G_DEFINE_TYPE (RstrntLogManager, rstrnt_log_manager, G_TYPE_OBJECT)

typedef struct
{
    GFile *file;
    GOutputStream *output_stream;
} RstrntLogData;

typedef struct
{
    RstrntLogData *log_data;
    GVariant *variant;
} RstrntLogWriterData;

typedef struct
{
    RstrntLogData *task_log_data;
    RstrntLogData *harness_log_data;

    GThreadPool *thread_pool;
} RstrntTaskLogData;

static void
rstrnt_log_manager_dispose (GObject *object)
{
    RstrntLogManager *self;

    self = RSTRNT_LOG_MANAGER (object);

    if (NULL != self->logs)
    {
        g_hash_table_destroy (self->logs);
        self->logs = NULL;
    }

    G_OBJECT_CLASS (rstrnt_log_manager_parent_class)->dispose (object);
}

static void
rstrnt_log_manager_class_init (RstrntLogManagerClass *klass)
{
    GObjectClass *object_class;

    object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = rstrnt_log_manager_dispose;
}

static void
rstrnt_log_data_destroy (gpointer data)
{
    RstrntLogData *log_data;

    log_data = data;

    g_clear_object (&log_data->output_stream);
    g_clear_object (&log_data->file);

    g_free (log_data);
}

static void
rstrnt_task_log_data_destroy (gpointer data)
{
    RstrntTaskLogData *task_log_data;

    task_log_data = data;

    rstrnt_log_data_destroy (task_log_data->task_log_data);
    rstrnt_log_data_destroy (task_log_data->harness_log_data);
    g_thread_pool_free (task_log_data->thread_pool, TRUE, TRUE);

    g_free (task_log_data);
}

static void
rstrnt_log_manager_init (RstrntLogManager *self)
{
    self->logs = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        NULL, rstrnt_task_log_data_destroy);
}

static RstrntLogData *
rstrnt_log_data_new (GFile   *file,
                     GError **error)
{
    RstrntLogData *data;
    g_autofree char *path = g_file_get_path(file);
    gint fd = open(path, O_CREAT | O_APPEND | O_WRONLY, 0666);

    data = g_new0 (RstrntLogData, 1);

    data->file = g_object_ref (file);
    data->output_stream = g_unix_output_stream_new(fd, TRUE);
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    if (NULL == data->output_stream)
    {
        rstrnt_log_data_destroy (data);

        return NULL;
    }

    return data;
}

static void
rstrnt_write_log_func (gpointer data,
                       gpointer user_data)
{
    g_autofree RstrntLogWriterData *writer_data = NULL;
    g_autoptr (GVariant) variant = NULL;
    const char *message;
    gsize message_length;
    GError *error = NULL;
    bool success;

    writer_data = data;
    variant = writer_data->variant;

    /* This is a sentinel */
    if (variant == NULL && writer_data->log_data == NULL) {
        g_debug ("%s(): Got data sentinel", __func__);

        return;
    }

    message = g_variant_get_fixed_array (variant, &message_length, sizeof (*message));
    success = g_output_stream_write_all (G_OUTPUT_STREAM (writer_data->log_data->output_stream),
                                         message, message_length,
                                         NULL, NULL, &error);

    if (!success)
    {
        g_warning ("%s(): Failed to write out log message: %s",
                   __func__, error->message);
    }
}

static RstrntTaskLogData *
rstrnt_task_log_data_new (const RstrntTask  *task,
                          GError           **error)
{
    g_autofree char *log_directory_path = NULL;
    g_autoptr (GFile) log_directory = NULL;
    g_autoptr (GFile) task_log_file = NULL;
    g_autoptr (GOutputStream) task_log_file_output_stream = NULL;
    g_autoptr (GFile) harness_log_file = NULL;
    g_autoptr (GOutputStream) harness_log_file_output_stream = NULL;
    RstrntTaskLogData *data;

    log_directory_path = g_build_path ("/", LOG_MANAGER_DIR, task->task_id, NULL);
    log_directory = g_file_new_for_path (log_directory_path);

    if (!g_file_make_directory_with_parents (log_directory, NULL, error))
    {
        if (NULL != error && G_IO_ERROR_EXISTS != (*error)->code)
        {
            return NULL;
        }

        g_clear_error (error);
    }

    task_log_file = g_file_get_child (log_directory, "task.log");
    harness_log_file = g_file_get_child (log_directory, "harness.log");
    data = g_new0 (RstrntTaskLogData, 1);

    data->task_log_data = rstrnt_log_data_new (task_log_file, error);
    if (NULL == data->task_log_data)
    {
        rstrnt_task_log_data_destroy (data);

        return NULL;
    }
    data->harness_log_data = rstrnt_log_data_new (harness_log_file, error);
    if (NULL == data->harness_log_data)
    {
        rstrnt_task_log_data_destroy (data);

        return NULL;
    }
    data->thread_pool = g_thread_pool_new (rstrnt_write_log_func, NULL, 1,
                                           FALSE, error);
    if (NULL == data->thread_pool)
    {
        rstrnt_task_log_data_destroy (data);

        return NULL;
    }

    return data;
}

static RstrntTaskLogData *
rstrnt_log_manager_get_task_data (RstrntLogManager  *self,
                                  const RstrntTask  *task,
                                  GError           **error)
{
    RstrntTaskLogData *data;

    data = g_hash_table_lookup (self->logs, task->task_id);
    if (NULL == data)
    {
        data = rstrnt_task_log_data_new (task, error);
        if (NULL != data)
        {
            (void) g_hash_table_insert (self->logs, task->task_id, data);
        }
    }

    return data;
}

static void
rstrnt_on_log_uploaded (SoupSession *session,
                        SoupMessage *msg,
                        gpointer     user_data)
{
    g_debug ("%s(): response code: %u", __func__, msg->status_code);

    g_mapped_file_unref (user_data);
}

static void
rstrnt_flush_log_data (const RstrntLogData *log_data,
                       GCancellable        *cancellable)
{
    GOutputStream      *stream;
    g_autofree gchar   *path = NULL;
    g_autoptr (GError)  error = NULL;

    stream = G_OUTPUT_STREAM (log_data->output_stream);

    if (g_output_stream_flush (stream, cancellable, &error)
        || G_IO_ERROR_CANCELLED == error->code)
        return;

    path = g_file_get_path (log_data->file);
    g_warning ("%s(): Failed to flush %s stream: %s", __func__, path, error->message);
}

static void
rstrnt_flush_logs (const RstrntTask *task,
                   GCancellable     *cancellable)
{
    RstrntLogManager *manager;
    RstrntTaskLogData *data;
    g_autoptr (GError) error = NULL;
    RstrntLogWriterData *sentinel;

    g_return_if_fail (NULL != task);

    manager = rstrnt_log_manager_get_instance ();
    data = rstrnt_log_manager_get_task_data (manager, task, &error);

    g_return_if_fail (NULL != data);

    /* Sentinel to make sure all logged data is done. Prevents the
       race condition where the last thread with data finishes after
       flushing the stream. */
    sentinel = g_new0 (RstrntLogWriterData, 1);
    (void) g_thread_pool_push (data->thread_pool, sentinel, NULL);

    while (g_thread_pool_unprocessed (data->thread_pool) > 0 &&
           g_thread_pool_get_num_threads (data->thread_pool) > 0)
    {
        g_usleep (G_USEC_PER_SEC / 4);
    }

    rstrnt_flush_log_data (data->task_log_data, cancellable);
    rstrnt_flush_log_data (data->harness_log_data, cancellable);
}

/*
 * Splits content, starting from offset, in SoupMessages of up to
 * chunk_len bytes.
 *
 * content must be non-NULL and content_len greater than 0.
 *
 * offset must be lesser than content_len.
 *
 * Memory in content must remain accessible until the last message in the
 * vector is freed.
 *
 * chunk_len must be greater than 0.
 *
 * Returns a SoupMessage vector of msgc elements. Caller must free the
 * vector and the SoupMessages.
 */
static SoupMessage **
rstrnt_chunk_log (SoupURI    *uri,
                  const char *content,
                  goffset     offset,
                  gsize       content_len,
                  gsize       chunk_len,
                  int        *msgc)
{
    SoupMessage **msgv;
    SoupMessage **msg;
    gsize         content_left;

    g_return_val_if_fail (uri != NULL, NULL);
    g_return_val_if_fail (content != NULL && content_len > 0, NULL);
    g_return_val_if_fail (content_len > offset, NULL);
    g_return_val_if_fail (chunk_len > 0, NULL);
    g_return_val_if_fail (msgc != NULL, NULL);

    content_len -= offset;

    *msgc = content_len / chunk_len + (content_len % chunk_len > 0);
    msgv = g_malloc0 (sizeof (SoupMessage *) * *msgc);

    msg = msgv;
    content_left = content_len;

    while (content_left > 0) {
        gsize   msg_len;
        goffset start;
        goffset end;

        msg_len = content_left > chunk_len ? chunk_len : content_left;
        start = offset + content_len - content_left;
        end = start + msg_len - 1;

        *msg = soup_message_new_from_uri ("PUT", uri);

        /* With incremental uploads, the total length of the resource is
           unknown. */
        soup_message_headers_set_content_range ((*msg)->request_headers, start, end, -1);

        soup_message_headers_append ((*msg)->request_headers, "log-level", "2");
        soup_message_set_request (*msg, "text/plain", SOUP_MEMORY_TEMPORARY, &content[start], msg_len);

        content_left -= msg_len;
        msg++;
    }

    return msgv;
}

const gchar *
rstrnt_log_type_get_path (RstrntLogType type)
{
    switch (type) {
    case RSTRNT_LOG_TYPE_TASK:
        return LOG_PATH_TASK;

    case RSTRNT_LOG_TYPE_HARNESS:
        return LOG_PATH_HARNESS;

    default:
        g_warn_if_reached ();
    }

    return NULL;
}

static RstrntLogData *
rstrnt_task_log_get_data (RstrntTaskLogData *task_logs,
                          RstrntLogType type)
{
    switch (type) {
    case RSTRNT_LOG_TYPE_TASK:
        return task_logs->task_log_data;

    case RSTRNT_LOG_TYPE_HARNESS:
        return task_logs->harness_log_data;

    default:
        g_warn_if_reached ();
    }

    return NULL;
}

static void
rstrnt_upload_log (const RstrntTask    *task,
                   RstrntServerAppData *app_data,
                   SoupSession         *session,
                   GCancellable        *cancellable,
                   RstrntLogType        type)
{
    RstrntLogManager *manager;
    g_autoptr (GError) error = NULL;
    RstrntTaskLogData *data;
    g_autofree char *path = NULL;
    GMappedFile *file;
    g_autoptr (SoupURI) uri = NULL;
    g_autofree SoupMessage **msgv = NULL;
    int msgc = 0;
    const char *contents;
    size_t length;
    const gchar *log_path = NULL;
    RstrntLogData *log_data = NULL;
    goffset *offset;

    manager = rstrnt_log_manager_get_instance ();
    data = rstrnt_log_manager_get_task_data (manager, task, &error);

    g_return_if_fail (NULL != data);

    g_clear_error (&error);

    log_path = rstrnt_log_type_get_path (type);
    log_data = rstrnt_task_log_get_data (data, type);

    path = g_file_get_path (log_data->file);
    file = g_mapped_file_new (path, false, &error);

    if (NULL == file)
    {
        g_warning ("Task log file mapping failed: %s", error->message);

        g_return_if_reached ();
    }

    uri = soup_uri_new_with_base (task->task_uri, log_path);
    contents = g_mapped_file_get_contents (file);
    length = g_mapped_file_get_length (file);

    offset = restraint_task_get_offset ((Task *) task, log_path);

    /* The offset as used in task and config.conf indicates the range
       start for the next upload. It cannot be lesser than the current
       length of the file. */
    g_return_if_fail (length >= *offset);

    if (length == *offset) {
        g_debug ("%s(): task %s: %s: No content to upload",
                 __func__, task->task_id, log_path);

        g_mapped_file_unref (file);

        return;
    }

    msgv = rstrnt_chunk_log (uri, contents, *offset, length, BKR_MAX_CONTENT_LENGTH, &msgc);

    g_return_if_fail (msgv != NULL && msgc > 0);

    for (int i = 0; i < msgc - 1; i++)
        app_data->queue_message (session, msgv[i], NULL, NULL, cancellable, NULL);

    /* Last message sent sets the callback to cleanup the mapped file. */
    app_data->queue_message (session,
                             msgv[msgc - 1],
                             NULL,
                             rstrnt_on_log_uploaded,
                             cancellable,
                             file);

    /* Notice that the offset is updated in the task even if setting the
       offset in the config failed. The offset in the file is used only
       for task restarts.
       TODO: Instead of using the config file, the offset value could be
       obtained after a restart using a HEAD request to the log URL.
    */
    *offset = length;

    if (!task_config_set_offset (app_data->config_file, (Task *) task, log_path, *offset, &error)) {
        g_warning ("%s(): Failed to set offset in config for task %s: %s",
                   __func__, task->task_id, error->message);
    }
}

void
rstrnt_upload_logs (const RstrntTask    *task,
                    RstrntServerAppData *app_data,
                    SoupSession         *session,
                    GCancellable        *cancellable)
{
    g_autoptr (GTask) flush_task = NULL;

    g_return_if_fail (NULL != task);
    g_return_if_fail (NULL != app_data);
    g_return_if_fail (SOUP_IS_SESSION (session));

    rstrnt_flush_logs (task, cancellable);

    rstrnt_upload_log (task, app_data, session, cancellable, RSTRNT_LOG_TYPE_TASK);
    rstrnt_upload_log (task, app_data, session, cancellable, RSTRNT_LOG_TYPE_HARNESS);
}

void
rstrnt_close_logs (const RstrntTask *task)
{
    RstrntLogManager *manager;
    
    g_return_if_fail (NULL != task);
    manager = rstrnt_log_manager_get_instance ();

    g_hash_table_remove(manager->logs, task->task_id);
}

static void
rstrnt_log_manager_append_to_log (RstrntLogManager    *self,
                                  const RstrntTask    *task,
                                  RstrntLogType        type,
                                  const char          *message,
                                  size_t               message_length)
{
    g_autoptr (GError) error = NULL;
    RstrntTaskLogData *data;
    RstrntLogWriterData *writer_data;

    data = rstrnt_log_manager_get_task_data (self, task, &error);
    if (NULL == data)
    {
        g_return_if_reached ();
    }
    writer_data = g_new0 (RstrntLogWriterData, 1);

    writer_data->log_data = rstrnt_task_log_get_data (data, type);

    writer_data->variant = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                      message, message_length,
                                                      sizeof(*message));

    (void) g_thread_pool_push (data->thread_pool, writer_data, NULL);
}

void
rstrnt_log_bytes (const RstrntTask *task,
                  RstrntLogType     type,
                  const char       *message,
                  size_t            message_length)
{
    RstrntLogManager *manager;

    g_return_if_fail (NULL != task);
    g_return_if_fail (NULL != message);

    manager = rstrnt_log_manager_get_instance ();

    rstrnt_log_manager_append_to_log (manager, task, type,
                                      message, message_length);
}

void
rstrnt_log (const RstrntTask *task,
            RstrntLogType     type,
            const char       *format,
            ...)
{
    va_list args;
    g_autofree char *message = NULL;
    size_t message_length;
    RstrntLogManager *manager;

    g_return_if_fail (NULL != task);
    g_return_if_fail (NULL != format);

    va_start (args, format);

    message = g_strdup_vprintf (format, args);

    va_end (args);

    message_length = strlen (message);
    manager = rstrnt_log_manager_get_instance ();

    rstrnt_log_manager_append_to_log (manager, task, type,
                                      message, message_length);
}

static gpointer
rstrnt_log_manager_create_instance (gpointer data)
{
    static RstrntLogManager *instance;

    (void) data;

    instance = g_object_new (RSTRNT_TYPE_LOG_MANAGER, NULL);

    return instance;
}

RstrntLogManager *
rstrnt_log_manager_get_instance (void)
{
    static GOnce once = G_ONCE_INIT;

    g_once (&once, rstrnt_log_manager_create_instance, NULL);

    return once.retval;
}

gboolean
rstrnt_log_manager_enabled (RstrntServerAppData *app_data)
{
    g_assert (NULL != app_data);

    return !app_data->stdin && app_data->uploader_interval > 0;
}
