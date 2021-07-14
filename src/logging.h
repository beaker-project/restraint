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

#ifndef _RESTRAINT_LOG_MANAGER_H
#define _RESTRAINT_LOG_MANAGER_H

#include <gio/gio.h>
#include <libsoup/soup.h>

#include <stdbool.h>

#define RSTRNT_TYPE_LOG_MANAGER rstrnt_log_manager_get_type ()

G_DECLARE_FINAL_TYPE (RstrntLogManager, rstrnt_log_manager, RSTRNT, LOG_MANAGER, GObject)

typedef enum
{
    RSTRNT_LOG_TYPE_TASK,
    RSTRNT_LOG_TYPE_HARNESS,
} RstrntLogType;

typedef struct RstrntServerAppData RstrntServerAppData;
typedef struct RstrntTask RstrntTask;


void              rstrnt_upload_logs              (const RstrntTask    *task,
                                                   RstrntServerAppData *app_data,
                                                   SoupSession         *session,
                                                   GCancellable        *cancellable);

void              rstrnt_log_bytes                (const RstrntTask    *task,
                                                   RstrntLogType        type,
                                                   const char          *message,
                                                   size_t               message_length);

void              rstrnt_log                      (const RstrntTask    *task,
                                                   RstrntLogType        type,
                                                   const char          *format,
                                                   ...) G_GNUC_PRINTF (3, 4);

void              rstrnt_close_logs               (const RstrntTask *task);

RstrntLogManager *rstrnt_log_manager_get_instance (void);

const gchar      *rstrnt_log_type_get_path        (RstrntLogType type);

gboolean          rstrnt_log_manager_enabled      (RstrntServerAppData *app_data);

#endif
