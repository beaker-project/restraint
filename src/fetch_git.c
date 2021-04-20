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

#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <gio/gio.h>
#include <fcntl.h>
#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>

#include "fetch.h"
#include "fetch_git.h"

static gint
packet_length(const gchar *linelen)
{
    gint n;
    gint len = 0;

    for (n = 0; n < HDR_LEN_SIZE; n++) {
        gint val = g_ascii_xdigit_value(linelen[n]);
        if (val < 0)
            return -1;
        len <<= HDR_LEN_SIZE;
        len += val;
    }
    return len;
}

static gboolean
packet_read_line(GInputStream *istream, gchar *buffer, gsize size,
        gsize *size_out, GError **error)
{
    g_return_val_if_fail(istream != NULL, FALSE);
    g_return_val_if_fail(buffer != NULL, FALSE);
    g_return_val_if_fail(size_out != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    gint len;
    gsize bytes_read = 0;
    gchar linelen[HDR_LEN_SIZE] = {0};

    GError *tmp_error = NULL;
    gboolean read_succeeded = g_input_stream_read_all(istream, linelen,
            HDR_LEN_SIZE, &bytes_read, NULL, &tmp_error);
    if (!read_succeeded) {
        g_propagate_prefixed_error(error, tmp_error,
                "While reading line length from git daemon: ");
        return FALSE;
    }
    if (bytes_read < HDR_LEN_SIZE) {
        g_set_error(error, RESTRAINT_FETCH_ERROR,
                RESTRAINT_FETCH_GIT_HUP_ERROR,
                "The remote end hung up unexpectedly");
        return FALSE;
    }
    len = packet_length(linelen);
    if (len < 0) {
        g_set_error(error, RESTRAINT_FETCH_ERROR,
                RESTRAINT_FETCH_GIT_PROTOCOL_ERROR,
                "protocol error: bad line length character: %.4s", linelen);
        return FALSE;
    }
    if (!len) {
        *size_out = 0;
        return TRUE;
    }
    len -= 4;
    if (len < 0 || len >= size) {
        g_set_error(error, RESTRAINT_FETCH_ERROR,
                RESTRAINT_FETCH_GIT_PROTOCOL_ERROR,
                "protocol error: bad line length %d", len);
        return FALSE;
    }
    read_succeeded = g_input_stream_read_all(istream, buffer,
            len, &bytes_read, NULL, &tmp_error);
    if (!read_succeeded) {
        g_propagate_prefixed_error(error, tmp_error,
                "While reading line length from git daemon: ");
        return FALSE;
    }
    if (bytes_read < len) {
        g_set_error(error, RESTRAINT_FETCH_ERROR,
                RESTRAINT_FETCH_GIT_HUP_ERROR,
                "The remote end hung up unexpectedly");
        return FALSE;
    }
    buffer[len] = 0;
    *size_out = len;
    return TRUE;
}

static gboolean
packet_write(GOutputStream *ostream, GError **error,
        const gchar *fmt, ...)
{
    g_return_val_if_fail(ostream != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    va_list args;
    va_start(args, fmt);
    g_autofree gchar *buffer = NULL;
    gint n = g_vasprintf(&buffer, fmt, args);
    va_end(args);
    if (n < 0)
        return FALSE;
    n += HDR_LEN_SIZE;

    gchar linelen[HDR_LEN_SIZE + 1] = {0};
    if (g_snprintf(linelen, HDR_LEN_SIZE + 1,
            "%0" G_STRINGIFY(HDR_LEN_SIZE) "x", (guint) n) > HDR_LEN_SIZE) {
        g_set_error(error, RESTRAINT_FETCH_ERROR,
                RESTRAINT_FETCH_GIT_PROTOCOL_ERROR,
                "protocol error: impossibly long line (%d bytes)", n);
        return FALSE;
    }
    GError *tmp_error = NULL;
    gsize bytes_written = 0;
    gboolean write_succeeded = g_output_stream_write_all(ostream, linelen,
            HDR_LEN_SIZE, &bytes_written, NULL, &tmp_error);
    if (!write_succeeded) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    write_succeeded = g_output_stream_write_all(ostream, buffer,
            (gsize) n - HDR_LEN_SIZE, &bytes_written, NULL, &tmp_error);
    if (!write_succeeded) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    return TRUE;
}

static ssize_t
myread(struct archive *a, void *client_data, const void **abuf)
{
    FetchData *fetch_data = client_data;
    gint band;
    gsize len = 0;
    *abuf = fetch_data->buf + 1;

    GError *error = NULL;
    gboolean read_succeeded = packet_read_line(fetch_data->istream, fetch_data->buf,
            LARGE_PACKET_MAX, &len, &error);
    if (!read_succeeded) {
        archive_set_error(fetch_data->a, error->code, "%s", error->message);
        return -1;
    }
    if (len == 0)
        return 0;
    band = fetch_data->buf[0] & 0xff;
    len--;
    if (band == 2) {
        archive_set_error(fetch_data->a, 1, "Error from remote service: %s", fetch_data->buf + 1);
        return -1;
    } else if (band != 1) {
        archive_set_error(fetch_data->a, 1, "Received data over unrecognized side-band %d", band);
        return -1;
    }
    return len;
}

static gboolean
myopen(FetchData *fetch_data, GError **error)
{
    g_return_val_if_fail(fetch_data != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    gsize len;
    gint path_offset = 0, fragment_offset = 0;
    GError *tmp_error = NULL;

    fetch_data->client = g_socket_client_new();
    guint port = fetch_data->url->port != 0 ? fetch_data->url->port : GIT_PORT;
    fetch_data->connection = g_socket_client_connect_to_host(fetch_data->client,
                                         fetch_data->url->host,
                                         port,
                                         NULL,
                                         &tmp_error);
    if (tmp_error != NULL) {
        g_propagate_prefixed_error(error, tmp_error,
                "While connecting to %s:%u: ", fetch_data->url->host, port);
        goto error;
    }

    fetch_data->istream = g_io_stream_get_input_stream (G_IO_STREAM (fetch_data->connection));
    fetch_data->ostream = g_io_stream_get_output_stream (G_IO_STREAM (fetch_data->connection));

    // git-upload-pack
    // git ls-remote git://git2.engineering.redhat.com/users/bpeck/tests.git
    // git-upload-pack /users/bpeck/tests.git.host=git2.engineering.redhat.com.

    // path can't start with /~
    if (g_str_has_prefix (fetch_data->url->path + 1, "~"))
        path_offset = 1;
    // git archive paths can't start with a slash
    if (g_str_has_prefix (fetch_data->url->fragment, "/"))
        fragment_offset = 1;

    gboolean write_succeeded = packet_write(fetch_data->ostream, &tmp_error,
                 "git-upload-archive %s\0host=%s side-band side-band-64k\0",
                 fetch_data->url->path + path_offset, fetch_data->url->host);
    if (!write_succeeded) {
        g_propagate_prefixed_error(error, tmp_error,
                "While writing to %s: ", fetch_data->url->host);
        goto error;
    }
    write_succeeded = packet_write(fetch_data->ostream, &tmp_error, "argument %s:%s\0",
                           fetch_data->url->query == NULL ? GIT_BRANCH : fetch_data->url->query, 
                           fetch_data->url->fragment == NULL ? "" : fetch_data->url->fragment + fragment_offset);
    if (!write_succeeded) {
        g_propagate_prefixed_error(error, tmp_error,
                "While writing to %s: ", fetch_data->url->host);
        goto error;
    }
    write_succeeded = packet_write(fetch_data->ostream, &tmp_error, "");
    if (!write_succeeded) {
        g_propagate_prefixed_error(error, tmp_error,
                "While writing to %s: ", fetch_data->url->host);
        goto error;
    }
    gboolean read_succeeded = packet_read_line(fetch_data->istream, fetch_data->buf,
            sizeof(fetch_data->buf), &len, &tmp_error);
    if (!read_succeeded) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    if (!len) {
        g_set_error(error, RESTRAINT_FETCH_ERROR,
                RESTRAINT_FETCH_GIT_PROTOCOL_ERROR,
                "Expected ACK/NAK from git daemon, got EOF");
        goto error;
    }
    if (fetch_data->buf[len-1] == '\n')
        fetch_data->buf[--len] = 0;
    if (strcmp(fetch_data->buf, "ACK")) {
        if (len > 5 && g_str_has_prefix(fetch_data->buf, "NACK ")) {
            g_set_error(error, RESTRAINT_FETCH_ERROR,
                    RESTRAINT_FETCH_GIT_NAK_ERROR,
                    "Refused by remote git daemon: %s", fetch_data->buf);
            goto error;
        }
        if (len > 4 && g_str_has_prefix(fetch_data->buf, "ERR ")) {
            g_set_error(error, RESTRAINT_FETCH_ERROR,
                    RESTRAINT_FETCH_GIT_REMOTE_ERROR,
                    "Error from remote git daemon: %s", fetch_data->buf + 4);
            goto error;
        }
        g_set_error(error, RESTRAINT_FETCH_ERROR,
                RESTRAINT_FETCH_GIT_PROTOCOL_ERROR,
                "Expected ACK/NAK from git daemon");
        goto error;
    }

    read_succeeded = packet_read_line(fetch_data->istream, fetch_data->buf,
            sizeof(fetch_data->buf), &len, &tmp_error);
    if (!read_succeeded) {
        g_propagate_error(error, tmp_error);
        goto error;
    }
    if (len) {
        g_set_error(error, RESTRAINT_FETCH_ERROR,
                RESTRAINT_FETCH_GIT_PROTOCOL_ERROR,
                "Expected a flush");
        goto error;
    }

    return TRUE;

error:
    if (fetch_data->connection != NULL)
        g_object_unref(fetch_data->connection);
    if (fetch_data->client != NULL)
        g_object_unref(fetch_data->client);
    return FALSE;
}

static int
myclose(struct archive *a, void *client_data)
{
    FetchData *fetch_data = client_data;
    GError * error = NULL;

    g_io_stream_close(G_IO_STREAM (fetch_data->connection),
                      NULL,
                      &error);
    g_object_unref(fetch_data->client);
    g_object_unref(fetch_data->connection);
    if (error != NULL) {
        archive_set_error(fetch_data->a, error->code, "%s", error->message);
        return ARCHIVE_FATAL;
    }
    return ARCHIVE_OK;
}

static gboolean
archive_finish_callback (gpointer user_data)
{
    FetchData *fetch_data = (FetchData *) user_data;
    gint free_result;

    if (fetch_data == NULL) {
        g_warning("%s: fetch_data is NULL", __func__);
        return FALSE;
    }

    if (fetch_data->ext != NULL) {
        free_result = archive_write_free(fetch_data->ext);
        if (free_result != ARCHIVE_OK)
            g_warning("Failed to free archive_write_disk");
    }
    if (fetch_data->a != NULL) {
        free_result = archive_read_free(fetch_data->a);
        if (free_result != ARCHIVE_OK)
            g_warning("Failed to free archive_read");
    }

    if (fetch_data->finish_callback) {
        fetch_data->finish_callback (fetch_data->error,
                                     fetch_data->match_cnt,
                                     0,
                                     fetch_data->user_data);
    } else {
        g_clear_error(&fetch_data->error);
    }

    g_slice_free(FetchData, fetch_data);
    return FALSE;
}

static gboolean
git_archive_read_callback (gpointer user_data)
{
    FetchData *fetch_data = (FetchData *) user_data;

    gint r;
    struct archive_entry *entry;
    gchar *newPath = NULL;

    r = archive_read_next_header(fetch_data->a, &entry);
    if (r == ARCHIVE_EOF) {
        g_idle_add (archive_finish_callback, fetch_data);
        return FALSE;
    }

    if (r != ARCHIVE_OK) {
        g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, r,
                "archive_read_next_header failed: %s", archive_error_string(fetch_data->a));
        g_idle_add (archive_finish_callback, fetch_data);
        return FALSE;
    }

    if (fetch_data->archive_entry_callback) {
        fetch_data->archive_entry_callback (archive_entry_pathname (entry),
                                            fetch_data->user_data);
    }

    // Update pathname
    newPath = g_build_filename (fetch_data->base_path, archive_entry_pathname( entry ), NULL);
    archive_entry_set_pathname( entry, newPath );
    g_free(newPath);

    if (fetch_data->keepchanges == FALSE ||
            access(archive_entry_pathname(entry), F_OK) == -1) {
        r = archive_read_extract2(fetch_data->a, entry, fetch_data->ext);
        if (r != ARCHIVE_OK) {
            g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, r,
                    "archive_read_extract2 failed: %s", archive_error_string(fetch_data->ext));
            g_idle_add (archive_finish_callback, fetch_data);
            return FALSE;
        }

        fetch_data->match_cnt++;
    }

    return TRUE;
}

void
restraint_fetch_git (SoupURI *url,
                     const gchar *base_path,
                     gboolean keepchanges,
                     ArchiveEntryCallback archive_entry_callback,
                     FetchFinishCallback finish_callback,
                     gpointer user_data)
{
    g_return_if_fail(url != NULL);
    g_return_if_fail(base_path != NULL);

    FetchData *fetch_data = g_slice_new0(FetchData);
    fetch_data->archive_entry_callback = archive_entry_callback;
    fetch_data->finish_callback = finish_callback;
    fetch_data->user_data = user_data;
    fetch_data->url = url;
    fetch_data->base_path = base_path;
    fetch_data->keepchanges = keepchanges;

    GError *tmp_error = NULL;
    gint r;

    if (keepchanges == FALSE) {
        rmrf(base_path);
    }

    if (fetch_data->archive_entry_callback) {
        gchar *url_string = soup_uri_to_string(url, FALSE);
        gchar *entry = g_strdup_printf ("%s%s", url_string, base_path);
        fetch_data->archive_entry_callback (entry,
                                            fetch_data->user_data);
        g_free(entry);
        g_free(url_string);
    }

    fetch_data->a = archive_read_new();
    if (fetch_data->a == NULL) {
        g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, 0,
                "archive_read_new failed");
        g_idle_add (archive_finish_callback, fetch_data);
        return;
    }

    fetch_data->ext = archive_write_disk_new();
    if (fetch_data->ext == NULL) {
        g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, 0,
                "archive_write_disk_new failed");
        g_idle_add (archive_finish_callback, fetch_data);
        return;
    }
    archive_read_support_filter_all(fetch_data->a);
    archive_read_support_format_all(fetch_data->a);
    gboolean open_succeeded = myopen(fetch_data, &tmp_error);
    if (!open_succeeded) {
        g_propagate_error(&fetch_data->error, tmp_error);
        g_idle_add (archive_finish_callback, fetch_data);
        return;
    }
    r = archive_read_open(fetch_data->a, fetch_data, NULL, myread, myclose);
    if (r != ARCHIVE_OK) {
        g_set_error(&fetch_data->error, RESTRAINT_FETCH_LIBARCHIVE_ERROR, r,
                "archive_read_open failed: %s", archive_error_string(fetch_data->a));
        g_idle_add (archive_finish_callback, fetch_data);
        return;
    }
    g_idle_add (git_archive_read_callback, fetch_data);
}
