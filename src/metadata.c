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

#include <stdio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include "metadata.h"
#include "process.h"
#include "errors.h"
#include "utils.h"
#include "param.h"

typedef struct _MetadataData {
    char *path;
    char *osmajor;
    MetaData **metadata;
    GCancellable *cancellable;
    GIOFunc io_callback;
    metadata_cb finish_cb;
    void *user_data;
} MetadataData;

void
restraint_metadata_free (MetaData *metadata)
{
    if (metadata) {
        g_free(metadata->name);
        g_free(metadata->entry_point);
        g_slist_free_full (metadata->dependencies, g_free);
        g_slist_free_full (metadata->softdependencies, g_free);
        g_slist_free_full (metadata->repodeps, g_free);
        g_slist_free_full (metadata->envvars, (GDestroyNotify)restraint_param_free);
        g_slice_free (MetaData, metadata);
    }
}

MetaData *
restraint_parse_metadata (gchar *filename,
                          gchar *locale,
                          GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    GKeyFile *keyfile;
    GKeyFileFlags flags;
    GError *tmp_error = NULL;
    MetaData *metadata = g_slice_new0 (MetaData);

    /* Create a new GKeyFile object and a bitwise list of flags. */
    keyfile = g_key_file_new();
    /* This is not really needed since I don't envision writing the file back out */
    flags = G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS;

    /* Load the GKeyFile from metadata or return. */
    if (!g_key_file_load_from_file(keyfile, filename, flags, &tmp_error)) {
        g_propagate_error(error, tmp_error);
        goto error;
    }

    metadata->name = g_key_file_get_locale_string(keyfile,
                                                  "General",
                                                  "name",
                                                  locale,
                                                  &tmp_error);
    if (tmp_error != NULL && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    // metadata->name is present
    if (tmp_error == NULL) {
        metadata->name = g_strstrip(metadata->name);
    }
    g_clear_error (&tmp_error);

    gchar *key_entry_point = g_key_file_get_locale_string(keyfile,
                                                      "restraint",
                                                      "entry_point",
                                                      locale,
                                                      &tmp_error);
    if (tmp_error && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    g_clear_error (&tmp_error);
    if (key_entry_point != NULL) {
        metadata->entry_point = key_entry_point;
    }

    gchar *max_time = g_key_file_get_locale_string (keyfile,
                                                    "restraint",
                                                    "max_time",
                                                    locale,
                                                    &tmp_error);
    if (tmp_error && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    g_clear_error (&tmp_error);
    if (max_time != NULL) {
        gint64 time = parse_time_string(max_time, &tmp_error);
        g_free(max_time);
        if (tmp_error) {
            g_propagate_error(error, tmp_error);
            goto error;
        }
        // If max_time is set it's because we read it from our run data
        metadata->max_time = time;
    }

    gsize length;
    gchar **dependencies = g_key_file_get_locale_string_list(keyfile,
                                                            "restraint",
                                                            "dependencies",
                                                            locale,
                                                            &length,
                                                            &tmp_error);
    gchar **dependency = dependencies;
    if (dependency) {
      while (*dependency) {
        metadata->dependencies = g_slist_prepend (metadata->dependencies, g_strdup(*dependency));
        dependency++;
      }
      g_strfreev (dependencies);
    }
    else
      metadata->dependencies = NULL;

    if (tmp_error && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    g_clear_error (&tmp_error);

    gchar **softdependencies = g_key_file_get_locale_string_list(keyfile,
                                                                 "restraint",
                                                                 "softDependencies",
                                                                 locale,
                                                                 &length,
                                                                 &tmp_error);
    gchar **softdependency = softdependencies;
    if (softdependency) {
      while (*softdependency) {
        metadata->softdependencies = g_slist_prepend (metadata->softdependencies, g_strdup(*softdependency));
        softdependency++;
      }
      g_strfreev (softdependencies);
    }
    else
      metadata->softdependencies = NULL;

    if (tmp_error && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    g_clear_error (&tmp_error);

    gchar **repodeps = g_key_file_get_locale_string_list(keyfile,
                                                         "restraint",
                                                         "repoRequires",
                                                         locale,
                                                         &length,
                                                         &tmp_error);
    gchar **repodep = repodeps;
    if (repodep) {
      while (*repodep) {
        metadata->repodeps = g_slist_prepend (metadata->repodeps, g_strdup(*repodep));
        repodep++;
      }
      g_strfreev (repodeps);
    }
    else
      metadata->repodeps = NULL;

    if (tmp_error && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    g_clear_error (&tmp_error);

    gchar **envvars = g_key_file_get_locale_string_list(keyfile,
                                                       "restraint",
                                                       "environment",
                                                       locale,
                                                       &length,
                                                       &tmp_error);
    gchar **envvar = envvars;
    if (envvar) {
        while (*envvar) {
            gchar **split = g_strsplit(*envvar, "=", 2);
            if(split[0] != NULL && split[1] != NULL){
                Param *p = restraint_param_new();
                p->name = g_strdup(split[0]);
                p->value = g_strdup(split[1]);
                metadata->envvars = g_slist_prepend(metadata->envvars, p);
            }
            g_strfreev(split);
            envvar++;
        }
        g_strfreev(envvars);
    }
    else {
        metadata->envvars = NULL;
    }
    if (tmp_error && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }

    g_clear_error (&tmp_error);

    metadata->nolocalwatchdog = g_key_file_get_boolean (keyfile,
                                                    "restraint",
                                                    "no_localwatchdog",
                                                    &tmp_error);

    if (tmp_error && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    g_clear_error (&tmp_error);

    metadata->use_pty = g_key_file_get_boolean (keyfile,
                                                "restraint",
                                                "use_pty",
                                                &tmp_error);

    if (tmp_error && tmp_error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    g_clear_error (&tmp_error);

    g_key_file_free(keyfile);

    return metadata;

error:
    restraint_metadata_free(metadata);
    g_key_file_free(keyfile);
    return NULL;
}

static void parse_line(MetaData *metadata,
                       const gchar *line,
                       gsize length,
                       GError **error) {

    g_return_if_fail(metadata != NULL);
    g_return_if_fail(error == NULL || *error == NULL);

    GError *tmp_error = NULL;
    gchar **key_value = g_strsplit (line,":",2);
    gchar *key = g_ascii_strup(g_strstrip(key_value[0]), -1);
    gchar *value = g_strdup(g_strstrip(key_value[1]));
    g_strfreev(key_value);

    if (g_strcmp0("TESTTIME",key) == 0) {
        guint64 time = parse_time_string(value, &tmp_error);
        if (tmp_error) {
            g_free(key);
            g_free(value);
            g_propagate_error(error, tmp_error);
            return;
        }
        metadata->max_time = time;
    } else if(g_strcmp0("NAME", key) == 0) {
        metadata->name = g_strdup(g_strstrip(value));
    } else if(g_strcmp0("USE_PTY", key) == 0) {
        gchar *uvalue = g_ascii_strup(value, -1);
        if (g_strcmp0("TRUE", uvalue) == 0) {
            metadata->use_pty = TRUE;
        } else {
            metadata->use_pty = FALSE;
        }
        g_free(uvalue);
    } else if(g_strcmp0("REQUIRES", key) == 0 ||
              g_strcmp0("RHTSREQUIRES", key) == 0) {
        gchar **dependencies = g_strsplit_set (value,", ", -1);
        gchar **dependency = dependencies;
        while (*dependency) {
            if (g_strcmp0 (*dependency, "") != 0) {
                metadata->dependencies = g_slist_prepend (metadata->dependencies, g_strdup(*dependency));
            }
            dependency++;
        }
        // We only want to free the array not the values that it's pointing to
        g_strfreev (dependencies);
    } else  if(g_strcmp0("REPOREQUIRES", key) == 0) {
        gchar **repodeps = g_strsplit_set (value,", ", -1);
        gchar **repodep = repodeps;
        while (*repodep) {
            if (g_strcmp0 (*repodep, "") != 0) {
                metadata->repodeps = g_slist_prepend (metadata->repodeps, g_strdup(*repodep));
            }
            repodep++;
        }
        // We only want to free the array not the values that it's pointing to
        g_strfreev (repodeps);
    } else if(g_strcmp0("ENVIRONMENT", key) == 0) {
        gchar **envvar = g_strsplit_set (value, "= ", 2);
        if(envvar[0] != NULL && envvar[1] != NULL){
            Param *p = restraint_param_new();
            p->name = g_strdup(envvar[0]);
            p->value = g_strdup(envvar[1]);
            metadata->envvars = g_slist_prepend(metadata->envvars, p);
        }
        g_strfreev(envvar);
    }
    g_free(key);
    g_free(value);
}

static void flush_parse_buffer(MetaData *metadata, GString *parse_buffer, GError **error) {
    g_return_if_fail(metadata != NULL);
    g_return_if_fail(error == NULL || *error == NULL);

    GError *tmp_error = NULL;

    if (parse_buffer->len > 0) {
        parse_line(metadata, parse_buffer->str, parse_buffer->len, &tmp_error);
        g_string_erase(parse_buffer, 0, -1);
        if (tmp_error) {
            g_propagate_error(error, tmp_error);
            return;
        }
    }
}

static void file_parse_data(MetaData *metadata,
                       GString *parse_buffer,
                       const gchar *data,
                       gsize length,
                       GError **error) {
    g_return_if_fail(metadata != NULL);
    g_return_if_fail(error == NULL || *error == NULL);

    GError *parse_error;
    gsize i;

    g_return_if_fail(data != NULL || length == 0);

    parse_error = NULL;
    i = 0;
    while (i < length) {
        if (data[i] == '\n') {
            /* remove old style ascii termintaor */
            if (parse_buffer->len > 0
                && (parse_buffer->str[parse_buffer->len - 1]
                    == '\r'))
                g_string_erase(parse_buffer, parse_buffer->len - 1, 1);
            /* When a newline is encountered flush the parse buffer so that the
             * line can be parsed.  Completely blank lines are skipped.
             */
            if (parse_buffer->len > 0)
                flush_parse_buffer(metadata, parse_buffer, &parse_error);
            if (parse_error) {
                g_propagate_error(error, parse_error);
                return;
            }
            i++;
        } else {
            const gchar *start_of_line;
            const gchar *end_of_line;
            gsize line_length;

            start_of_line = data + i;
            end_of_line = memchr(start_of_line, '\n', length - i);

            if (end_of_line == NULL)
                end_of_line = data + length;

            line_length = end_of_line - start_of_line;

            g_string_append_len(parse_buffer, start_of_line, line_length);
            i += line_length;
        }
    }
}

static gboolean parse_testinfo_from_fd(MetaData *metadata,
                                       gint fd,
                                       GError **error) {
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    GError *tmp_error = NULL;
    gssize bytes_read;
    struct stat stat_buf;
    gchar read_buf[4096];
    g_autoptr (GString) parse_buffer = g_string_sized_new(128);

    if (fstat(fd, &stat_buf) < 0) {
        g_set_error_literal(error, G_FILE_ERROR,
                            g_file_error_from_errno(errno),
                            g_strerror(errno));
        return FALSE;
    }

    if (!S_ISREG (stat_buf.st_mode)) {
        g_set_error_literal (error, G_FILE_ERROR,
                             RESTRAINT_OPEN,
                             "Not a regular file");
      return FALSE;
    }

    do {
        bytes_read = read(fd, read_buf, 4096);

        if (bytes_read == 0) /* End of File */
            break;

        if (bytes_read < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;

            g_set_error_literal(error, G_FILE_ERROR,
                                g_file_error_from_errno(errno),
                                g_strerror(errno));
            return FALSE;
        }
        file_parse_data(metadata,
                        parse_buffer,
                        read_buf,
                        bytes_read,
                        &tmp_error);
    } while (!tmp_error);

    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }

    flush_parse_buffer(metadata, parse_buffer, &tmp_error);
    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    return TRUE;
}

MetaData *
restraint_parse_testinfo(gchar *filename,
                         GError **error)
{
    g_return_val_if_fail(filename != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    GError *tmp_error = NULL;
    gint fd;

    fd = g_open(filename, O_RDONLY, 0);

    if (fd == -1) {
        g_set_error_literal(error, G_FILE_ERROR,
                            g_file_error_from_errno(errno),
                            g_strerror(errno));
        return NULL;
    }

    MetaData *metadata = g_slice_new0(MetaData);
    parse_testinfo_from_fd(metadata, fd, &tmp_error);
    close(fd);

    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        restraint_metadata_free(metadata);
        return NULL;
    }
    return metadata;
}

gboolean
mktinfo_io_callback (GIOChannel *io, GIOCondition condition, gpointer user_data) {
    MetadataData *mtdata = (MetadataData*)user_data;
    return mtdata->io_callback(io, condition, mtdata->user_data);
}

static void mktinfo_cb(gint pid_result, gboolean localwatchdog,
                       gpointer user_data, GError *error)
{
    MetadataData *mtdata = (MetadataData*)user_data;

    gchar *testinfo_file = g_build_filename(mtdata->path, "testinfo.desc",
                                            NULL);
    g_cancellable_set_error_if_cancelled(mtdata->cancellable, &error);
    if (error || !file_exists(testinfo_file)) {
        mtdata->finish_cb(mtdata->user_data, error);
    } else {
        restraint_get_metadata(mtdata->path, mtdata->osmajor, mtdata->metadata,
                               mtdata->cancellable, mtdata->finish_cb,
                               mtdata->io_callback, mtdata->user_data);
    }
    g_free (testinfo_file);
    g_slice_free(MetadataData, mtdata);
}

gboolean restraint_get_metadata(char *path, char *osmajor, MetaData **metadata,
                                GCancellable *cancellable,
                                metadata_cb finish_cb,
                                GIOFunc io_callback, void *user_data)
{
    GError *error = NULL;
    gboolean ret = TRUE;
    gchar *metadata_file = g_build_filename(path, "metadata", NULL);
    gchar *testinfo_file = g_build_filename(path, "testinfo.desc", NULL);

    if (file_exists(metadata_file)) {
        ret = FALSE;
        *metadata = restraint_parse_metadata(metadata_file, osmajor, &error);
        finish_cb(user_data, error);
    } else if (file_exists(testinfo_file)) {
        ret = TRUE;
        *metadata = restraint_parse_testinfo(testinfo_file, &error);
        finish_cb(user_data, error);
    } else {
        ret = TRUE;

        const gchar *command = "make testinfo.desc";
        MetadataData *mtdata = g_slice_new0(MetadataData);
        mtdata->path = path;
        mtdata->osmajor = osmajor;
        mtdata->metadata = metadata;
        mtdata->cancellable = cancellable;
        mtdata->io_callback = io_callback;
        mtdata->finish_cb = finish_cb;
        mtdata->user_data = user_data;

        process_run(command, NULL, path, FALSE, 0,
                    NULL, mktinfo_io_callback, mktinfo_cb,
                    NULL, 0, FALSE, cancellable, mtdata);
    }

    g_free (testinfo_file);
    g_free (metadata_file);
    return ret;
}
