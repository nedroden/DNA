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
 * nautilus-dropbox-hooks.cpp
 * Implements connection handling and C interface for the Dropbox hook socket.
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

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <cstring>

#include <glib.h>

#include "g-util.h"
#include "dropbox-client-util.h"
#include "nautilus-dropbox-hooks.h"
#include "dna-util.h"

struct HookData
{
    DropboxUpdateHook hook;
    gpointer ud;
};

static gboolean try_to_connect(NautilusDropboxHookserv* hookserv);

static gboolean handle_hook_server_input(GIOChannel* t_chan, GIOCondition t_cond, NautilusDropboxHookserv* t_hookserv)
{
    // debug_enter();

    /* We have some sweet macros defined that allow us to write this
     * async event handler like a microthread yeahh, watch out for context */
    if (t_hookserv->hhsi.line == 0)
    {
        while (true)
        {
            t_hookserv->hhsi.command_args = g_hash_table_new_full(
                (GHashFunc) g_str_hash,
                (GEqualFunc) g_str_equal,
                (GDestroyNotify) g_free,
                (GDestroyNotify) g_strfreev
            );

            t_hookserv->hhsi.numargs = 0;
            
            // Read the command name
            gchar* line;
            dna::cr_read_line(t_hookserv->hhsi.line, t_chan, line);
            t_hookserv->hhsi.command_name = dropbox_client_util_desanitize(line);
            g_free(line);

            // debug("got a hook name: %s", hookserv->hhsi.command_name);

            // Now read each arg line (until a certain limit) until we receive "done"
            while (true)
            {
                gchar* line;

                // If too many arguments, this connection seems malicious
                if (t_hookserv->hhsi.numargs >= 20)
                {
                    return false;
                }

                dna::cr_read_line(t_hookserv->hhsi.line, t_chan, line);

                if (strcmp("done", line) == 0)
                {
                    g_free(line);
                    break;
                }
                else
                {
                    gboolean parse_result;

                    parse_result =
                    dropbox_client_util_command_parse_arg(line, t_hookserv->hhsi.command_args);
                    g_free(line);

                    if (!parse_result)
                    {
                        debug("bad parse");
                        return false;
                    }
                }

                t_hookserv->hhsi.numargs++;
            }

            HookData* hd;
            hd = (HookData *)
            g_hash_table_lookup(t_hookserv->dispatch_table, t_hookserv->hhsi.command_name);

            if (hd != nullptr)
            {
                (hd->hook)(t_hookserv->hhsi.command_args, hd->ud);
            }
            
            g_free(t_hookserv->hhsi.command_name);
            g_hash_table_unref(t_hookserv->hhsi.command_args);
            t_hookserv->hhsi.command_name = nullptr;
            t_hookserv->hhsi.command_args = nullptr;
        }
    }

    return false;
}

static void watch_killer(NautilusDropboxHookserv* t_hookserv)
{
    debug("hook client disconnected");

    t_hookserv->connected = false;

    g_hook_list_invoke(&(t_hookserv->ondisconnect_hooklist), false);
  
    // We basically just have to free the memory allocated in the handle_hook_server_init ctx
    if (t_hookserv->hhsi.command_name != nullptr)
    {
        g_free(t_hookserv->hhsi.command_name);
        t_hookserv->hhsi.command_name = nullptr;
    }

    if (t_hookserv->hhsi.command_args != nullptr)
    {
        g_hash_table_unref(t_hookserv->hhsi.command_args);
        t_hookserv->hhsi.command_args = nullptr;
    }

    g_io_channel_unref(t_hookserv->chan);
    t_hookserv->chan = nullptr;
    t_hookserv->event_source = 0;
    t_hookserv->socket = 0;

    // lol we also have to start a new connection
    try_to_connect(t_hookserv);
}

static gboolean try_to_connect(NautilusDropboxHookserv* t_hookserv)
{
    // Create socket
    t_hookserv->socket = socket(PF_UNIX, SOCK_STREAM, 0);
  
    // Set native non-blocking, for connect timeout */
    int flags;

    if ((flags = fcntl(t_hookserv->socket, F_GETFL, 0)) < 0)
    {
        goto FAIL_CLEANUP;
    }

    if (fcntl(t_hookserv->socket, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        goto FAIL_CLEANUP;
    }

    // Connect to server, might fail of course
    struct sockaddr_un addr;
    socklen_t addr_len;

    // Intialize address structure
    addr.sun_family = AF_UNIX;
    g_snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/.dropbox/iface_socket", g_get_home_dir());
    addr_len = sizeof(addr) - sizeof(addr.sun_path) + strlen(addr.sun_path);

    // If there was an error we have to try again later
    if (connect(t_hookserv->socket, (struct sockaddr *) &addr, addr_len) < 0)
    {
        if (errno == EINPROGRESS)
        {
            fd_set writers;
            struct timeval tv = {1, 0};

            FD_ZERO(&writers);
            FD_SET(t_hookserv->socket, &writers);

            // If nothing was ready after 3 seconds, fail out homie
            if (select(t_hookserv->socket + 1, nullptr, &writers, nullptr, &tv) == 0)
            {
                goto FAIL_CLEANUP;
            }

            if (connect(t_hookserv->socket, (struct sockaddr *) &addr, addr_len) < 0)
            {
                debug("couldn't connect to hook server after 1 second");
                goto FAIL_CLEANUP;
            }
        }
        else
        {
            goto FAIL_CLEANUP;
        }
    }

    // No idea why this code is here.
    if (false)
    {
        FAIL_CLEANUP:
        close(t_hookserv->socket);
        g_timeout_add_seconds(1, (GSourceFunc) try_to_connect, t_hookserv);

        return false;
    }

    /* great we connected!, let's create the channel and wait on it */
    t_hookserv->chan = g_io_channel_unix_new(t_hookserv->socket);
    g_io_channel_set_line_term(t_hookserv->chan, "\n", -1);
    g_io_channel_set_close_on_unref(t_hookserv->chan, true);

    // Set non-blocking ;) (again just in case)
    GIOFlags flags;
    GIOStatus iostat;
    
    flags = g_io_channel_get_flags(t_hookserv->chan);
    iostat = g_io_channel_set_flags(t_hookserv->chan, flags | G_IO_FLAG_NONBLOCK, nullptr);

    if (iostat == G_IO_STATUS_ERROR)
    {
        g_io_channel_unref(t_hookserv->chan);
        g_timeout_add_seconds(1, (GSourceFunc) try_to_connect, t_hookserv);

        return false;
    }

    t_hookserv->hhsi.line = 0;
    t_hookserv->hhsi.command_args = nullptr;
    t_hookserv->hhsi.command_name = nullptr;
    
    t_hookserv->event_source = g_io_add_watch_full(
        t_hookserv->chan,
        G_PRIORITY_DEFAULT,
        G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
        (GIOFunc) handle_hook_server_input, t_hookserv,
        (GDestroyNotify) watch_killer
    );

    debug("hook client connected");
    t_hookserv->connected = true;
    g_hook_list_invoke(&(t_hookserv->onconnect_hooklist), false);

    return false;
}

/**
 * Should only be called in glib main loop
 * returns a gboolean because it is a GSourceFunc
 */
gboolean nautilus_dropbox_hooks_force_reconnect(NautilusDropboxHookserv* t_hookserv)
{
    debug_enter();

    if (t_hookserv->connected == false)
    {
        return false;
    }

    debug("forcing hook to reconnect");

    g_assert(t_hookserv->event_source >= 0);

    if (t_hookserv->event_source > 0)
    {
        g_source_remove(t_hookserv->event_source);
    }
    else if (t_hookserv->event_source == 0)
    {
        debug("event source was zero!");
    }

    return false;
}

gboolean nautilus_dropbox_hooks_is_connected(NautilusDropboxHookserv* t_hookserv)
{
    return t_hookserv->connected;
}

void nautilus_dropbox_hooks_setup(NautilusDropboxHookserv* t_hookserv)
{
    t_hookserv->dispatch_table = g_hash_table_new_full((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal, g_free, g_free);
    t_hookserv->connected = false;

    g_hook_list_init(&(t_hookserv->ondisconnect_hooklist), sizeof(GHook));
    g_hook_list_init(&(t_hookserv->onconnect_hooklist), sizeof(GHook));
}

void nautilus_dropbox_hooks_add_on_disconnect_hook(NautilusDropboxHookserv* t_hookserv, DropboxHookClientConnectHook t_dhcch, gpointer t_ud)
{
    GHook* newhook;

    newhook = g_hook_alloc(&(t_hookserv->ondisconnect_hooklist));
    newhook->func = t_dhcch;
    newhook->data = t_ud;

    g_hook_append(&(t_hookserv->ondisconnect_hooklist), newhook);
}

void nautilus_dropbox_hooks_add_on_connect_hook(NautilusDropboxHookserv* t_hookserv, DropboxHookClientConnectHook t_dhcch, gpointer t_ud)
{
    GHook* newhook;

    newhook = g_hook_alloc(&(t_hookserv->onconnect_hooklist));
    newhook->func = t_dhcch;
    newhook->data = t_ud;

    g_hook_append(&(t_hookserv->onconnect_hooklist), newhook);
}

void nautilus_dropbox_hooks_add(NautilusDropboxHookserv* t_hookserv, const gchar* t_hook_name, DropboxUpdateHook t_hook, gpointer t_ud) {
    HookData* hd;
    hd = g_new(HookData, 1);
    hd->hook = t_hook;
    hd->ud = t_ud;
    g_hash_table_insert(t_hookserv->dispatch_table, g_strdup(t_hook_name), hd);
}

void nautilus_dropbox_hooks_start(NautilusDropboxHookserv* t_hookserv)
{
    try_to_connect(t_hookserv);
}
