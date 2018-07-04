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
 * dropbox-client.h
 * Header file for dropbox-client.c
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

#ifndef DROPBOX_CLIENT_H
#define DROPBOX_CLIENT_H

#include <glib.h>
#include "dropbox-command-client.h"
#include "nautilus-dropbox-hooks.h"

G_BEGIN_DECLS

struct DropboxClient {
    DropboxCommandClient dcc;
    NautilusDropboxHookserv hookserv;
    GHookList onconnect_hooklist;
    GHookList ondisconnect_hooklist;
    gboolean hook_connect_called;
    gboolean command_connect_called;
    gboolean hook_disconnect_called;
    gboolean command_disconnect_called;
};

typedef void (*DropboxClientConnectionAttemptHook)(guint, gpointer);
typedef GHookFunc DropboxClientConnectHook;

void dropbox_client_setup(DropboxClient* t_dc);
void dropbox_client_start(DropboxClient* t_dc);

gboolean dropbox_client_is_connected(DropboxClient* t_dc);
void dropbox_client_force_reconnect(DropboxClient* t_dc);

void dropbox_client_add_on_connect_hook(DropboxClient* t_dc, DropboxClientConnectHook t_dhcch, gpointer t_ud);
void dropbox_client_add_on_disconnect_hook(DropboxClient* t_dc, DropboxClientConnectHook t_dhcch, gpointer t_ud);
void dropbox_client_add_connection_attempt_hook(DropboxClient* t_dc, DropboxClientConnectionAttemptHook t_dhcch, gpointer t_ud);

G_END_DECLS

#endif
