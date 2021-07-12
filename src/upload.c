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
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#define READ_BUFFER_SIZE 131072
static gchar input_buf[READ_BUFFER_SIZE];
static gssize offset = 0;

static gint
upload_chunk (SoupSession *session,
              GInputStream *in,
              guint64 filesize,
              guint64 bytes_left,
              SoupURI *result_log_uri,
              GError **error)
{
    SoupMessage *server_msg;
    gchar *range;
    gssize bytes_read;
    gint ret;
    GError *tmp_error = NULL;
    bytes_read = g_input_stream_read (
        in, input_buf, (bytes_left < READ_BUFFER_SIZE) ? bytes_left : READ_BUFFER_SIZE,
        NULL, &tmp_error);
    if (tmp_error) {
        g_propagate_error (error, tmp_error);
        return -1;
    }
    gchar* index = getenv("RSTRNT_INDEX");

    if (bytes_read > 0) {
        server_msg = soup_message_new_from_uri ("PUT", result_log_uri);
        range = g_strdup_printf ("bytes %" G_GSSIZE_FORMAT "-%" G_GSSIZE_FORMAT "/%" G_GUINT64_FORMAT,
                                 offset,
                                 offset + bytes_read - 1,
                                 filesize);
        offset += bytes_read;
        soup_message_headers_append (server_msg->request_headers, "Content-Range", range);
        soup_message_headers_append (server_msg->request_headers, "task-index", index);
        g_free (range);
        soup_message_set_request (server_msg, "text/plain", SOUP_MEMORY_COPY, input_buf, bytes_read);
        ret = soup_session_send_message (session, server_msg);
        if (SOUP_STATUS_IS_SUCCESSFUL (ret)) {
            return bytes_read;
        } else {
            g_set_error_literal (error, SOUP_HTTP_ERROR, server_msg->status_code,
                                     server_msg->reason_phrase);
            g_object_unref (server_msg);
            return -1;
        }
    }
    return 0;
}

gboolean
upload_file (SoupSession *session,
             gchar *filepath,
             gchar *filename,
             SoupURI *results_uri,
             GError **error)
{
    GFile *f = g_file_new_for_path (filepath);
    GFileInputStream *fis = NULL;
    GFileInfo *fileinfo = NULL;
    guint64 filesize, uploaded;
    GError *tmp_error = NULL;
    SoupURI *result_log_uri;
    gint ret;
    char *uri_estring_filename = NULL;

    fileinfo = g_file_query_info (f, "standard::*", G_FILE_QUERY_INFO_NONE,
                                  NULL, &tmp_error);
    if (tmp_error != NULL) {
        g_propagate_prefixed_error (error, tmp_error,
            "Error querying: %s ", filepath);
        g_object_unref (f);
        return FALSE;
    }

    filesize = g_file_info_get_attribute_uint64 (
                   fileinfo, G_FILE_ATTRIBUTE_STANDARD_SIZE);
    g_object_unref(fileinfo);

    /* get input stream */
    fis = g_file_read (f, NULL, &tmp_error);

    if (tmp_error != NULL) {
        g_propagate_prefixed_error (error, tmp_error,
            "Error opening: %s ", filepath);
        g_object_unref (f);
        return FALSE;
    }

    uri_estring_filename = g_uri_escape_string(filename, NULL, FALSE);
    result_log_uri = soup_uri_new_with_base (results_uri,
                                             uri_estring_filename);
    uploaded = 0;
    while (uploaded < filesize) {
        ret = upload_chunk (
            session, G_INPUT_STREAM (fis), filesize, (filesize - uploaded),
            result_log_uri, &tmp_error);
        if (ret < 0) {
            break;
        } else {
            uploaded += ret;
            // replace with callback
            g_print (".");
        }
    }

    g_free(uri_estring_filename);
    soup_uri_free (result_log_uri);
    g_object_unref(fis);
    g_object_unref (f);

    if (tmp_error) {
        g_propagate_prefixed_error (error, tmp_error,
            "Error uploading: %s ", filepath);
        return FALSE;
    }

    return uploaded == filesize;
}
