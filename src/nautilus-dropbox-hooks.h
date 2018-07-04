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
 * nautilus-dropbox-hooks.h
 * Header file for nautilus-dropbox-hooks.c
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

#ifndef NAUTILUS_DROPBOX_HOOKS_H
#define NAUTILUS_DROPBOX_HOOKS_H

#include <glib.h>

G_BEGIN_DECLS

typedef void (*DropboxUpdateHook)(GHashTable *, gpointer);
typedef void (*DropboxHookClientConnectHook)(gpointer);

struct NautilusDropboxHookserv
{
    GIOChannel* chan;
    int socket;

    struct hhsi
    {
        int line;
        gchar *command_name;
        GHashTable *command_args;
        int numargs;
    };

    gboolean connected;
    guint event_source;
    GHashTable* dispatch_table;
    GHookList ondisconnect_hooklist;
    GHookList onconnect_hooklist;
};

void nautilus_dropbox_hooks_setup(NautilusDropboxHookserv *);
void nautilus_dropbox_hooks_start(NautilusDropboxHookserv *);

gboolean nautilus_dropbox_hooks_is_connected(NautilusDropboxHookserv *);
gboolean nautilus_dropbox_hooks_force_reconnect(NautilusDropboxHookserv *);

void nautilus_dropbox_hooks_add(NautilusDropboxHookserv* t_ndhs, const gchar* t_hook_name, DropboxUpdateHook t_hook, gpointer t_ud);
void nautilus_dropbox_hooks_add_on_disconnect_hook(NautilusDropboxHookserv* t_hookserv, DropboxHookClientConnectHook t_dhcch, gpointer t_ud);
void nautilus_dropbox_hooks_add_on_connect_hook(NautilusDropboxHookserv* t_hookserv, DropboxHookClientConnectHook t_dhcch, gpointer t_ud);

G_END_DECLS

#endif
