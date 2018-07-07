/*
 *
 * DNA - Dropbox for Nautilus on Arch
 * Copyright (C) 2018 Robert Monden
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * BASED ON nautilus-dropbox:
 * ------------------------------------------------------------------------
 * Copyright 2008 Evenflow, Inc.
 *
 * dropbox-command-client.h
 * Header file for nautilus-dropbox-command.c
 *
 * This file is part of nautilus-dropbox.
 *
 * nautilus-dropbox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nautilus-dropbox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nautilus-dropbox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef DROPBOX_COMMAND_CLIENT_H
#define DROPBOX_COMMAND_CLIENT_H

#include <libnautilus-extension/nautilus-info-provider.h>
#include <libnautilus-extension/nautilus-file-info.h>

G_BEGIN_DECLS

/* command structs */
enum NautilusDropboxRequestType {
    GET_FILE_INFO, GENERAL_COMMAND
};

struct DropboxCommand {
    NautilusDropboxRequestType  request_type;
};

struct DropboxFileInfoCommand {
    DropboxCommand          dc;
    NautilusInfoProvider*   provider;
    GClosure*               update_complete;
    NautilusFileInfo*       file;
    gboolean                cancelled;
};

struct DropboxFileInfoCommandResponse {
    DropboxFileInfoCommand*     dfic;
    GHashTable*                 file_status_response;
    GHashTable*                 folder_tag_response;
    GHashTable*                 emblems_response;
};

typedef void (*NautilusDropboxCommandResponseHandler)(GHashTable *, gpointer);

struct DropboxGeneralCommand {
    DropboxCommand                          dc;
    gchar*                                  command_name;
    GHashTable*                             command_args;
    NautilusDropboxCommandResponseHandler   handler;
    gpointer                                handler_ud;
};

typedef void (*DropboxCommandClientConnectionAttemptHook)(guint, gpointer);
typedef GHookFunc DropboxCommandClientConnectHook;

struct DropboxCommandClient {
    GMutex*         command_connected_mutex;
    gboolean        command_connected;
    GAsyncQueue*    command_queue; 
    GList*          ca_hooklist;
    GHookList       onconnect_hooklist;
    GHookList       ondisconnect_hooklist;
};

gboolean dropbox_command_client_is_connected(DropboxCommandClient* t_dcc);

void dropbox_command_client_force_reconnect(DropboxCommandClient* t_dcc);
void dropbox_command_client_request(DropboxCommandClient* t_dcc, DropboxCommand* t_dc);
void dropbox_command_client_setup(DropboxCommandClient* t_dcc);
void dropbox_command_client_start(DropboxCommandClient* t_dcc);

void dropbox_command_client_send_simple_command(DropboxCommandClient* t_dcc, const char* t_command);
void dropbox_command_client_send_command(DropboxCommandClient* t_dcc, NautilusDropboxCommandResponseHandler t_h, gpointer t_ud, const char* t_command, ...);

void dropbox_command_client_add_on_connect_hook(DropboxCommandClient* t_dcc, DropboxCommandClientConnectHook t_dhcch, gpointer t_ud);
void dropbox_command_client_add_on_disconnect_hook(DropboxCommandClient* t_dcc, DropboxCommandClientConnectHook t_dhcch, gpointer t_ud);
void dropbox_command_client_add_connection_attempt_hook(DropboxCommandClient* t_dcc, DropboxCommandClientConnectionAttemptHook t_dhcch, gpointer t_ud);

static gpointer dropbox_command_client_thread(DropboxCommandClient* t_data);

/**
 * Todo: clean up this mess
 */
#define WRITE_OR_DIE_SANI(s,l) {                    \
    gchar *sani_s;                          \
    sani_s = dropbox_client_util_sanitize(s);               \
    iostat = g_io_channel_write_chars(chan, sani_s,l, &bytes_trans, \
                      &tmp_error);          \
    g_free(sani_s);                         \
    if (iostat == G_IO_STATUS_ERROR ||                  \
    iostat == G_IO_STATUS_AGAIN) {                  \
      if (tmp_error != NULL) {                      \
    g_propagate_error(err, tmp_error);              \
      }                                 \
      return NULL;                          \
    }                                   \
  }
  
#define WRITE_OR_DIE(s,l) {                     \
    iostat = g_io_channel_write_chars(chan, s,l, &bytes_trans,      \
                      &tmp_error);          \
    if (iostat == G_IO_STATUS_ERROR ||                  \
    iostat == G_IO_STATUS_AGAIN) {                  \
      if (tmp_error != NULL) {                      \
    g_propagate_error(err, tmp_error);              \
      }                                 \
      return NULL;                          \
    }                                   \
  }

#define SET_CONNECTED_STATE(s)     {      \
      g_mutex_lock(dcc->command_connected_mutex); \
      dcc->command_connected = s;     \
      g_mutex_unlock(dcc->command_connected_mutex); \
    }

G_END_DECLS

#endif
