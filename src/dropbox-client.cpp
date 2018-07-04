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
 * dropbox-client.cpp
 * Implements connection handling and C interface for interfacing with the Dropbox daemon.
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

#include <glib.h>

#include "g-util.h"
#include "dropbox-command-client.h"
#include "nautilus-dropbox-hooks.h"
#include "dropbox-client.h"

static void hook_on_connect(DropboxClient* t_dc)
{
    t_dc->hook_connect_called = true;

    if (t_dc->command_connect_called)
    {
        debug("client connection");
        g_hook_list_invoke(&(t_dc->onconnect_hooklist), false);
        
        // Reset flags
        t_dc->hook_connect_called = t_dc->command_connect_called = false;
    }
}

static void command_on_connect(DropboxClient* t_dc)
{
    t_dc->command_connect_called = true;

    if (t_dc->hook_connect_called)
    {
        debug("client connection");
        g_hook_list_invoke(&(t_dc->onconnect_hooklist), false);
        
        // Reset flags
        t_dc->hook_connect_called = t_dc->command_connect_called = false;
    }
}

static void command_on_disconnect(DropboxClient* t_dc)
{
    t_dc->command_disconnect_called = true;

    if (t_dc->hook_disconnect_called)
    {
        debug("client disconnect");
        g_hook_list_invoke(&(t_dc->ondisconnect_hooklist), false);
        
        // Reset flags
        t_dc->hook_disconnect_called = t_dc->command_disconnect_called = false;
    }
    else
    {
        nautilus_dropbox_hooks_force_reconnect(&(t_dc->hookserv));
    }
}

static void hook_on_disconnect(DropboxClient* t_dc)
{
    t_dc->hook_disconnect_called = true;

    if (t_dc->command_disconnect_called)
    {
        debug("client disconnect");
        g_hook_list_invoke(&(t_dc->ondisconnect_hooklist), false);
        
        // Reset flags
        t_dc->hook_disconnect_called = t_dc->command_disconnect_called = false;
    }
    else
    {
        dropbox_command_client_force_reconnect(&(t_dc->dcc));
    }
}

gboolean dropbox_client_is_connected(DropboxClient* t_dc)
{
    return (dropbox_command_client_is_connected(&(t_dc->dcc)) && nautilus_dropbox_hooks_is_connected(&(t_dc->hookserv)));
}

void dropbox_client_force_reconnect(DropboxClient* t_dc)
{
    if (dropbox_client_is_connected(t_dc))
    {
        debug("forcing client to reconnect");
        dropbox_command_client_force_reconnect(&(t_dc->dcc));
        nautilus_dropbox_hooks_force_reconnect(&(t_dc->hookserv));
    }
}

/* should only be called once on initialization */
void dropbox_client_setup(DropboxClient* t_dc)
{
    nautilus_dropbox_hooks_setup(&(t_dc->hookserv));
    dropbox_command_client_setup(&(t_dc->dcc));

    g_hook_list_init(&(t_dc->ondisconnect_hooklist), sizeof(GHook));
    g_hook_list_init(&(t_dc->onconnect_hooklist), sizeof(GHook));

    t_dc->hook_disconnect_called = t_dc->command_disconnect_called = false;
    t_dc->hook_connect_called = t_dc->command_connect_called = false;

    nautilus_dropbox_hooks_add_on_connect_hook(&(t_dc->hookserv), (DropboxHookClientConnectHook) hook_on_connect, t_dc);
    dropbox_command_client_add_on_connect_hook(&(t_dc->dcc), (DropboxCommandClientConnectHook) command_on_connect, t_dc);
    nautilus_dropbox_hooks_add_on_disconnect_hook(&(t_dc->hookserv), (DropboxHookClientConnectHook) hook_on_disconnect, t_dc);
    dropbox_command_client_add_on_disconnect_hook(&(t_dc->dcc), (DropboxCommandClientConnectHook) command_on_disconnect, t_dc);
}

/* not thread safe */
void dropbox_client_add_on_disconnect_hook(DropboxClient* t_dc, DropboxClientConnectHook t_dhcch, gpointer t_ud)
{
    GHook* newhook;

    newhook = g_hook_alloc(&(t_dc->ondisconnect_hooklist));
    newhook->func = dhcch;
    newhook->data = ud;

    g_hook_append(&(t_dc->ondisconnect_hooklist), newhook);
}

/* not thread safe */
void dropbox_client_add_on_connect_hook(DropboxClient *t_dc, DropboxClientConnectHook t_dhcch, gpointer t_ud) {
    GHook* newhook;

    newhook = g_hook_alloc(&(t_dc->onconnect_hooklist));
    newhook->func = t_dhcch;
    newhook->data = t_ud;

    g_hook_append(&(t_dc->onconnect_hooklist), newhook);
}

/* not thread safe */
void dropbox_client_add_connection_attempt_hook(DropboxClient *t_dc, DropboxClientConnectionAttemptHook t_dhcch, gpointer t_ud)
{
    debug("shouldn't be here...");

    dropbox_command_client_add_connection_attempt_hook(&(t_dc->dcc), t_dhcch, t_ud);
}

/* should only be called once on initialization */
void dropbox_client_start(DropboxClient *t_dc)
{
    debug("starting connections");
    nautilus_dropbox_hooks_start(&(t_dc->hookserv));
    dropbox_command_client_start(&(t_dc->dcc));
}
