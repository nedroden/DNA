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
 * nautilus-dropbox.cpp
 * Implements the Nautilus extension API for Dropbox. 
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

#ifdef HAVE_CONFIG_H
#include <config.h> /* for GETTEXT_PACKAGE */
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <glib-object.h>
#include <gtk/gtk.h>

#include <libnautilus-extension/nautilus-extension-types.h>
#include <libnautilus-extension/nautilus-menu-provider.h>
#include <libnautilus-extension/nautilus-info-provider.h>

#include "g-util.h"
#include "dropbox-command-client.h"
#include "nautilus-dropbox.h"
#include "nautilus-dropbox-hooks.h"

static char* emblems[] = {"dropbox-uptodate", "dropbox-syncing", "dropbox-unsyncable"};
gchar* DEFAULT_EMBLEM_PATHS[2] = { EMBLEMDIR , nullptr };

gboolean dropbox_use_nautilus_submenu_workaround;
gboolean dropbox_use_operation_in_progress_workaround;

static GType dropbox_type = 0;

/*
 * Simplifies a path by removing navigation elements such as '.' and '..'
 *
 * Arguments:
 * - path: input path to be canonicalized
 *
 * Returns:
 * Canonicalized path if input path is valid.
 * NULL otherwise.
 */
static gchar* canonicalize_path(gchar* t_path)
{
    int i, j = 0;
    gchar* toret = nullptr;
    gchar** cpy, elts;

    g_assert(t_path != nullptr);
    g_assert(t_path[0] == '/');

    elts = g_strsplit(t_path, "/", 0);
    cpy = g_new(gchar *, g_strv_length(elts) + 1);
    cpy[j++] = "/";

    for (i = 0; elts[i] != nullptr; i++)
    {
        if (strcmp(elts[i], "..") == 0)
        {
            if (j > 0)
            {
                j--;
            }
            else
            {
                // Input path has too many parent directory references and is invalid
                toret = nullptr;
                goto exit;
            }
        }
        else if (strcmp(elts[i], ".") != 0 && elts[i][0] != '\0')
        {
            cpy[j++] = elts[i];
        }
    }

    cpy[j] = nullptr;
    toret = g_build_filenamev(cpy);

    exit:
        g_free(cpy);
        g_strfreev(elts);

    return toret;
}

static void reset_file(NautilusFileInfo* t_file)
{
    debug("resetting file %p", (void *) t_file);
    nautilus_file_info_invalidate_extension_info(t_file);
}

gboolean reset_all_files(NautilusDropbox* t_cvs)
{
    /* Only run this on the main loop or you'll cause problems.
     * This works because you can call a function pointer with
     * more arguments than it takes */
    g_hash_table_foreach(t_cvs->obj2filename, (GHFunc) reset_file, nullptr);

    return false;
}


static void when_file_dies(NautilusDropbox* t_cvs, NautilusFileInfo* t_address)
{
    gchar* filename;

    filename = g_hash_table_lookup(t_cvs->obj2filename, t_address);

    /* we never got a change to view this file */
    if (filename != nullptr)
    {
        g_hash_table_remove(t_cvs->filename2obj, filename);
        g_hash_table_remove(t_cvs->obj2filename, t_address);
    }
}

static void changed_cb(NautilusFileInfo* t_file, NautilusDropbox* t_cvs)
{
    // Check if this file's path has changed, if so update the hash and invalidate the file
    gchar* filename, pfilename;
    gchar* filename2;
    gchar* uri;

    uri = nautilus_file_info_get_uri(t_file);
    pfilename = g_filename_from_uri(uri, nullptr, nullptr);
    filename = pfilename ? canonicalize_path(pfilename) : nullptr;

    // Canonicalization will only null-out a non-null filename if it is invalid
    g_assert((pfilename == nullptr && filename == nullptr) || (pfilename != nullptr && filename != nullptr));

    filename2 =  g_hash_table_lookup(t_cvs->obj2filename, t_file);

    g_free(pfilename);
    g_free(uri);

    // If filename2 is NULL we've never seen this file in update_file_info
    if (filename2 == nullptr)
    {
        g_free(filename);
        return;
    }

    if (filename == nullptr)
    {
        // A file has moved to offline storage. Lets remove it from our tables.
        g_object_weak_unref(G_OBJECT(t_file), (GWeakNotify) when_file_dies, t_cvs);

        g_hash_table_remove(t_cvs->filename2obj, filename2);
        g_hash_table_remove(t_cvs->obj2filename, t_file);

        g_signal_handlers_disconnect_by_func(t_file, G_CALLBACK(changed_cb), t_cvs);
        reset_file(t_file);

        return;
    }

    /* This is a hack, because nautilus doesn't do this for us, for some reason
     * the file's path has changed */
    if (strcmp(filename, filename2) != 0)
    {
        debug("shifty old: %s, new %s", filename2, filename);

        // Gotta do this first, the call after this frees filename2
        g_hash_table_remove(t_cvs->filename2obj, filename2);
        g_hash_table_replace(t_cvs->obj2filename, t_file, g_strdup(filename));

        NautilusFileInfo *f2;

        // We shouldn't have another mapping from filename to an object
        f2 = g_hash_table_lookup(t_cvs->filename2obj, filename);

        if (f2 != nullptr)
        {
            // Lets fix it if it's true, just remove the mapping
            g_hash_table_remove(t_cvs->filename2obj, filename);
            g_hash_table_remove(t_cvs->obj2filename, f2);
        }

        g_hash_table_insert(t_cvs->filename2obj, g_strdup(filename), t_file);
        reset_file(t_file);
    }

    g_free(filename);
}

static NautilusOperationResult nautilus_dropbox_update_file_info(NautilusInfoProvider* t_provider, NautilusFileInfo* t_file, GClosure* t_update_complete, NautilusOperationHandle** t_handle)
{
    NautilusDropbox* cvs = NAUTILUS_DROPBOX(t_provider);

    // This code adds this file object to our two-way hash of file objects so we can shell touch these files later
    
    gchar* pfilename, uri;

    uri = nautilus_file_info_get_uri(file);
    pfilename = g_filename_from_uri(uri, nullptr, nullptr);
    g_free(uri);

    if (pfilename != nullptr)
    {
        int cmp = 0;
        gchar* stored_filename;
        gchar* filename;

        filename = canonicalize_path(pfilename);
        g_free(pfilename);

        if (filename == nullptr)
        {
            // pfilename path was invalid if canonicalize operation nulled it out
            return NAUTILUS_OPERATION_FAILED;
        }

        stored_filename = g_hash_table_lookup(cvs->obj2filename, t_file);

        // Don't worry about the dup checks, gcc is smart enough to optimize this GCSE ftw
        if ((stored_filename != nullptr && (cmp = strcmp(stored_filename, filename)) != 0) || stored_filename == nullptr)
        {
            if (stored_filename != nullptr && cmp != 0)
            {
                // This happens when the filename changes name on a file obj but changed_cb isn't called
                g_object_weak_unref(G_OBJECT(t_file), (GWeakNotify) when_file_dies, cvs);
                g_hash_table_remove(cvs->obj2filename, t_file);
                g_hash_table_remove(cvs->filename2obj, stored_filename);
                g_signal_handlers_disconnect_by_func(t_file, G_CALLBACK(changed_cb), cvs);
            }
            else if (stored_filename == nullptr)
            {
                NautilusFileInfo* f2;

                if ((f2 = g_hash_table_lookup(cvs->filename2obj, filename)) != nullptr)
                {
                    /* If the filename exists in the filename2obj hash
                     * but the file obj doesn't exist in the obj2filename hash:
                     *
                     * this happens when nautilus allocates another file object
                     * for a filename without first deleting the original file object
                     *
                     * just remove the association to the older file object, it's obsolete
                     */
                    g_object_weak_unref(G_OBJECT(f2), (GWeakNotify) when_file_dies, cvs);
                    g_signal_handlers_disconnect_by_func(f2, G_CALLBACK(changed_cb), cvs);
                    g_hash_table_remove(cvs->filename2obj, filename);
                    g_hash_table_remove(cvs->obj2filename, f2);
                }
            }

            // Too chatty (???)
            g_object_weak_ref(G_OBJECT(t_file), (GWeakNotify) when_file_dies, cvs);
            g_hash_table_insert(cvs->filename2obj, g_strdup(filename), t_file);
            g_hash_table_insert(cvs->obj2filename, t_file, g_strdup(filename));
            g_signal_connect(t_file, "changed", G_CALLBACK(changed_cb), cvs);
        }

        g_free(filename);
    }
    else
    {
        return NAUTILUS_OPERATION_COMPLETE;
    }

    if (!dropbox_client_is_connected(&(cvs->dc)) || nautilus_file_info_is_gone(t_file))
    {
        return NAUTILUS_OPERATION_COMPLETE;
    }

    DropboxFileInfoCommand* dfic = g_new0(DropboxFileInfoCommand, 1);

    dfic->cancelled = false;
    dfic->provider = t_provider;
    dfic->dc.request_type = GET_FILE_INFO;
    dfic->update_complete = g_closure_ref(t_update_complete);
    dfic->file = g_object_ref(t_file);

    dropbox_command_client_request(&(cvs->dc.dcc), (DropboxCommand *) dfic);

    *t_handle = (NautilusOperationHandle *) dfic;

    return dropbox_use_operation_in_progress_workaround ? NAUTILUS_OPERATION_COMPLETE : NAUTILUS_OPERATION_IN_PROGRESS;
}

static void handle_shell_touch(GHashTable* t_args, NautilusDropbox* t_cvs)
{
    gchar** path;

    if ((path = g_hash_table_lookup(t_args, "path")) != nullptr && path[0][0] == '/')
    {
        NautilusFileInfo* file;
        gchar* filename;

        filename = canonicalize_path(path[0]);

        if (filename != nullptr)
        {
            debug("shell touch for %s", filename);

            file = g_hash_table_lookup(t_cvs->filename2obj, filename);

            if (file != nullptr)
            {
                debug("gonna reset %s", filename);
                reset_file(file);
            }

            g_free(filename);
        }
    }
}

gboolean nautilus_dropbox_finish_file_info_command(DropboxFileInfoCommandResponse* t_dficr)
{
    NautilusOperationResult result = NAUTILUS_OPERATION_FAILED;

    if (!t_dficr->dfic->cancelled)
    {
        gchar **status = nullptr;
        bool isdir = nautilus_file_info_is_directory(t_dficr->dfic->file);

        // If we have emblems, just use them.
        if (t_dficr->emblems_response != nullptr && (status = g_hash_table_lookup(t_dficr->emblems_response, "emblems")) != nullptr)
        {
            for (int i = 0; status[i] != nullptr; i++)
            {
                if (status[i][0])
                {
                    nautilus_file_info_add_emblem(t_dficr->dfic->file, status[i]);
                }
            }

            result = NAUTILUS_OPERATION_COMPLETE;
        }
        // If the file status command went okay
        else if ((t_dficr->file_status_response != nullptr && (status = g_hash_table_lookup(t_dficr->file_status_response, "status")) != nullptr) && ((isdir && t_dficr->folder_tag_response != nullptr) || !isdir))
        {
            gchar** tag = nullptr;

            // Set the tag emblem
            if (isdir && (tag = g_hash_table_lookup(t_dficr->folder_tag_response, "tag")) != nullptr)
            {
                if (strcmp("public", tag[0]) == 0)
                {
                    nautilus_file_info_add_emblem(t_dficr->dfic->file, "web");
                }
                else if (strcmp("shared", tag[0]) == 0)
                {
                    nautilus_file_info_add_emblem(t_dficr->dfic->file, "people");
                }
                else if (strcmp("photos", tag[0]) == 0)
                {
                    nautilus_file_info_add_emblem(t_dficr->dfic->file, "photos");
                }
                else if (strcmp("sandbox", tag[0]) == 0)
                {
                    nautilus_file_info_add_emblem(t_dficr->dfic->file, "star");
                }
            }

            // Set the status emblem
            int emblem_code = 0;

            if (strcmp("up to date", status[0]) == 0)
            {
                emblem_code = 1;
            }
            else if (strcmp("syncing", status[0]) == 0)
            {
                emblem_code = 2;
            }
            else if (strcmp("unsyncable", status[0]) == 0)
            {
                emblem_code = 3;
            }

            if (emblem_code > 0)
            {
                nautilus_file_info_add_emblem(t_dficr->dfic->file, emblems[emblem_code-1]);
            }

            result = NAUTILUS_OPERATION_COMPLETE;
        }
    }

    // Complete the info request
    if (!dropbox_use_operation_in_progress_workaround)
    {
        nautilus_info_provider_update_complete_invoke(t_dficr->dfic->update_complete, t_dficr->dfic->provider, (NautilusOperationHandle*) t_dficr->dfic, result);
    }

    // Destroy the objects we created
    if (t_dficr->file_status_response != nullptr)
    {
        g_hash_table_unref(t_dficr->file_status_response);
    }

    if (t_dficr->folder_tag_response != nullptr)
    {
        g_hash_table_unref(t_dficr->folder_tag_response);
    }

    if (t_dficr->emblems_response != nullptr)
    {
        g_hash_table_unref(t_dficr->emblems_response);
    }

    // Unref the objects we didn't create
    g_closure_unref(t_dficr->dfic->update_complete);
    g_object_unref(t_dficr->dfic->file);

    // Now free the structs
    g_free(t_dficr->dfic);
    g_free(t_dficr);

    return false;
}

static void nautilus_dropbox_cancel_update(NautilusInfoProvider* t_provider, NautilusOperationHandle* t_handle) {
    DropboxFileInfoCommand* dfic = (DropboxFileInfoCommand *) t_handle;
    dfic->cancelled = true;
}

static void menu_item_cb(NautilusMenuItem* t_item, NautilusDropbox* t_cvs)
{
    gchar* verb;
    GList* files;
    DropboxGeneralCommand* dcac;

    dcac = g_new(DropboxGeneralCommand, 1);

    files = g_object_get_data(G_OBJECT(t_item), "nautilus_dropbox_files");
    verb = g_object_get_data(G_OBJECT(t_item), "nautilus_dropbox_verb");

    dcac->dc.request_type = GENERAL_COMMAND;

    // Build the argument list
    dcac->command_args = g_hash_table_new_full((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal, (GDestroyNotify) g_free, (GDestroyNotify) g_strfreev);

    gchar** arglist;
    guint i;

    arglist = g_new0(gchar *, g_list_length(files) + 1);

    for (GList* li = files, i = 0; li != nullptr; li = g_list_next(li))
    {
        char* uri = nautilus_file_info_get_uri(NAUTILUS_FILE_INFO(li->data));
        char* path = g_filename_from_uri(uri, nullptr, nullptr);

        g_free(uri);

        if (!path)
        {
            continue;
        }

        arglist[i] = path;
        i++;
    }

    g_hash_table_insert(dcac->command_args, g_strdup("paths"), arglist);

    gchar** arglist;
    arglist = g_new(gchar *, 2);
    arglist[0] = g_strdup(verb);
    arglist[1] = nullptr;
    g_hash_table_insert(dcac->command_args, g_strdup("verb"), arglist);

    dcac->command_name = g_strdup("icon_overlay_context_action");
    dcac->handler = nullptr;
    dcac->handler_ud = nullptr;

    dropbox_command_client_request(&(t_cvs->dc.dcc), (DropboxCommand *) dcac);
}

static char from_hex(gchar t_ch)
{
    return isdigit(ch) ? t_ch - '0' : tolower(t_ch) - 'a' + 10;
}

// decode in --> out, but dont fill more than n chars into out
// returns len of out if thing went well, -1 if n wasn't big enough
// can be used in place (whoa!)
int GhettoURLDecode(gchar* t_out, gchar* t_in, int t_n)
{
    char* out_initial;

    for(out_initial = out; out - out_initial < n && *in != '\0'; out++)
    {
        if (*in == '%')
        {
            if ((in[1] != '\0') && (in[2] != '\0'))
            {
                *out = from_hex(in[1]) << 4 | from_hex(in[2]);
                in += 3;
            }
            else
            {
                // Input string isn't well-formed
                return -1;
            }
        }
        else
        {
            *out = *in;
            in++;
        }
    }

    if (out - out_initial < n)
    {
        *out = '\0';
        return out - out_initial;
    }

    return -1;
}

static int nautilus_dropbox_parse_menu(gchar** t_options, NautilusMenu* t_menu, GString* t_old_action_string, GList* t_toret, NautilusMenuProvider* t_provider, GList* t_files)
{
    int ret = 0;

    for (int i = 0; t_options[i] != nullptr; i++)
    {
        gchar** option_info = g_strsplit(t_options[i], "~", 3);

        // If this is a valid string
        if (option_info[0] == nullptr || option_info[1] == nullptr || option_info[2] == nullptr || option_info[3] != nullptr)
        {
            g_strfreev(option_info);
            continue;
        }

        gchar* item_name = option_info[0];
        gchar* item_inner = option_info[1];
        gchar* verb = option_info[2];

        GhettoURLDecode(item_name, item_name, strlen(item_name));
        GhettoURLDecode(verb, verb, strlen(verb));
        GhettoURLDecode(item_inner, item_inner, strlen(item_inner));

        /* If the inner section has a menu in it then we create a submenu.  The verb will be ignored.
         * Otherwise add the verb to our map and add the menu item to the list. */
        if (strchr(item_inner, '~') != nullptr)
        {
            GString* new_action_string = g_string_new(t_old_action_string->str);
            gchar** suboptions = g_strsplit(item_inner, "|", -1);
            NautilusMenuItem* item;
            NautilusMenu* submenu = nautilus_menu_new();

            g_string_append(new_action_string, item_name);
            g_string_append(new_action_string, "::");

            ret += nautilus_dropbox_parse_menu(suboptions, submenu, new_action_string, t_toret, t_provider, t_files);

            item = nautilus_menu_item_new(new_action_string->str, item_name, "", nullptr);
            nautilus_menu_item_set_submenu(item, submenu);
            nautilus_menu_append_item(t_menu, item);

            g_strfreev(suboptions);
            g_object_unref(item);
            g_object_unref(submenu);
            g_string_free(new_action_string, true);
        }
        else
        {
            NautilusMenuItem* item;
            GString* new_action_string = g_string_new(t_old_action_string->str);
            bool grayed_out = false;

            g_string_append(new_action_string, verb);

            if (item_name[0] == '!')
            {
                item_name++;
                grayed_out = true;
            }

            item = nautilus_menu_item_new(new_action_string->str, item_name, item_inner, nullptr);

            nautilus_menu_append_item(t_menu, item);

            // Add the file metadata to this item
            g_object_set_data_full(G_OBJECT(item), "nautilus_dropbox_files", nautilus_file_info_list_copy(t_files), (GDestroyNotify) nautilus_file_info_list_free);

            // Add the verb metadata
            g_object_set_data_full(G_OBJECT(item), "nautilus_dropbox_verb", g_strdup(verb), (GDestroyNotify) g_free);
            g_signal_connect(item, "activate", G_CALLBACK (menu_item_cb), t_provider);

            if (grayed_out)
            {
                GValue sensitive = { 0 };
                g_value_init(&sensitive, G_TYPE_BOOLEAN);
                g_value_set_boolean (&sensitive, false);
                g_object_set_property(G_OBJECT(item), "sensitive", &sensitive);
            }

            /* Taken from nautilus-file-repairer (http://repairer.kldp.net/):
             * this code is a workaround for a bug of nautilus
             * See: http://bugzilla.gnome.org/show_bug.cgi?id=508878 */
            if (dropbox_use_nautilus_submenu_workaround)
            {
                t_toret = g_list_append(t_toret, item);
            }

            g_object_unref(item);
            g_string_free(new_action_string, true);
            ret++;
        }

        g_strfreev(option_info);
    }

    return ret;
}

static void get_file_items_callback(GHashTable* t_response, gpointer t_ud)
{
    GAsyncQueue* reply_queue = t_ud;

    /* Queue_push doesn't accept NULL as a value so we create an empty hash table
     * if we got no response. */
    g_async_queue_push(reply_queue, t_response ? g_hash_table_ref(t_response) : g_hash_table_new((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal));

    g_async_queue_unref(reply_queue);
}


static GList* nautilus_dropbox_get_file_items(NautilusMenuProvider* t_provider, GtkWidget* t_window, GList* t_files)
{
    /*
     * 1. Convert files to filenames.
     */
    int file_count = g_list_length(t_files);

    if (file_count < 1)
    {
        return nullptr;
    }

    gchar** paths = g_new0(gchar *, file_count + 1);
    int i = 0;
    GList* elem;

    for (elem = t_files; elem; elem = elem->next, i++)
    {
        gchar* uri = nautilus_file_info_get_uri(elem->data);
        gchar* filename_un = uri ? g_filename_from_uri(uri, nullptr, nullptr) : nullptr;
        gchar* filename = filename_un ? g_filename_to_utf8(filename_un, -1, nullptr, nullptr, nullptr) : nullptr;

        g_free(uri);
        g_free(filename_un);

        // Oooh, filename wasn't correctly encoded, or isn't a local file.
        if (filename == nullptr)
        {
            g_strfreev(paths);
            return nullptr;
        }

        paths[i] = filename;
    }

    GAsyncQueue* reply_queue = g_async_queue_new_full((GDestroyNotify) g_hash_table_unref);

    /*
     * 2. Create a DropboxGeneralCommand to call "icon_overlay_context_options"
     */
    DropboxGeneralCommand* dgc = g_new0(DropboxGeneralCommand, 1);
    dgc->dc.request_type = GENERAL_COMMAND;
    dgc->command_name = g_strdup("icon_overlay_context_options");
    dgc->command_args = g_hash_table_new_full((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal, (GDestroyNotify) g_free, (GDestroyNotify) g_strfreev);
    
    g_hash_table_insert(dgc->command_args, g_strdup("paths"), paths);

    dgc->handler = get_file_items_callback;
    dgc->handler_ud = g_async_queue_ref(reply_queue);

    /*
     * 3. Queue it up for the helper thread to run it.
     */
    NautilusDropbox* cvs = NAUTILUS_DROPBOX(t_provider);
    dropbox_command_client_request(&(cvs->dc.dcc), (DropboxCommand *) dgc);

    GTimeVal gtv;

    /*
     * 4. We have to block until it's done because nautilus expects a reply.  But we will
     * only block for 50 ms for a reply.
     */

    g_get_current_time(&gtv);
    g_time_val_add(&gtv, 50000);

    GHashTable* context_options_response = g_async_queue_timed_pop(reply_queue, &gtv);
    g_async_queue_unref(reply_queue);

    if (!context_options_response)
    {
        return nullptr;
    }

    /*
     * 5. Parse the reply.
     */

    char** options = g_hash_table_lookup(context_options_response, "options");
    GList* toret = nullptr;

    if (options && *options && **options)
    {
        // Build the menu
        NautilusMenuItem* root_item;
        NautilusMenu* root_menu;

        root_menu = nautilus_menu_new();
        root_item = nautilus_menu_item_new("NautilusDropbox::root_item", "Dropbox", "Dropbox Options", "dropbox");

        toret = g_list_append(toret, root_item);
        GString* action_string = g_string_new("NautilusDropbox::");

        if (!nautilus_dropbox_parse_menu(options, root_menu, action_string, toret, t_provider, files))
        {
            g_object_unref(toret);
            toret = nullptr;
        }

        nautilus_menu_item_set_submenu(root_item, root_menu);

        g_string_free(action_string, true);
        g_object_unref(root_menu);
    }

    g_hash_table_unref(context_options_response);

    return toret;
}

gboolean add_emblem_paths(GHashTable* t_emblem_paths_response)
{
    // Only run this on the main loop or you'll cause problems.
    if (!t_emblem_paths_response)
    {
        return false;
    }

    gchar** emblem_paths_list;

    GtkIconTheme* theme = gtk_icon_theme_get_default();

    if (t_emblem_paths_response && (emblem_paths_list = g_hash_table_lookup(t_emblem_paths_response, "path")))
    {
        for (int i = 0; emblem_paths_list[i] != nullptr; i++)
        {
            if (emblem_paths_list[i][0])
            {
                gtk_icon_theme_append_search_path(theme, emblem_paths_list[i]);
            }
        }
    }
    g_hash_table_unref(t_emblem_paths_response);

    return false;
}

gboolean remove_emblem_paths(GHashTable* t_emblem_paths_response)
{
    // Only run this on the main loop or you'll cause problems.
    if (!t_emblem_paths_response)
    {
        return false;
    }

    gchar** emblem_paths_list = g_hash_table_lookup(t_emblem_paths_response, "path");
    
    if (!emblem_paths_list)
    {
        goto exit;
    }

    // We need to remove the old paths.
    GtkIconTheme* icon_theme = gtk_icon_theme_get_default();
    gchar** paths;
    gint path_count;

    gtk_icon_theme_get_search_path(icon_theme, &paths, &path_count);

    gint i, j, out = 0;
    bool found = false;

    for (i = 0; i < path_count; i++)
    {
        bool keep = true;

        for (j = 0; emblem_paths_list[j] != NULL; j++)
        {
            if (emblem_paths_list[j][0])
            {
                if (!g_strcmp0(paths[i], emblem_paths_list[j]))
                {
                    found = true;
                    keep = false;
                    g_free(paths[i]);

                    break;
                }
            }
        }

        if (keep)
        {
            paths[out] = paths[i];
            out++;
        }
    }

    // If we found one we need to reset the path to accomodate the changes
    if (found)
    {
        paths[out] = nullptr; /* Clear the last one */
        gtk_icon_theme_set_search_path(icon_theme, (const gchar **) paths, out);
    }

    g_strfreev(paths);

    exit:
        g_hash_table_unref(t_emblem_paths_response);
        return false;
}

void get_emblem_paths_cb(GHashTable *t_emblem_paths_response, NautilusDropbox *t_cvs)
{
    if (!t_emblem_paths_response)
    {
        t_emblem_paths_response = g_hash_table_new((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal);
        g_hash_table_insert(t_emblem_paths_response, "path", DEFAULT_EMBLEM_PATHS);
    }
    else
    {
        // Increase the ref so that finish_general_command doesn't delete it.
        g_hash_table_ref(t_emblem_paths_response);
    }

    g_mutex_lock(t_cvs->emblem_paths_mutex);

    if (t_cvs->emblem_paths)
    {
        g_idle_add((GSourceFunc) remove_emblem_paths, t_cvs->emblem_paths);
        t_cvs->emblem_paths = nullptr;
    }

    t_cvs->emblem_paths = t_emblem_paths_response;
    g_mutex_unlock(t_cvs->emblem_paths_mutex);

    g_idle_add((GSourceFunc) add_emblem_paths, g_hash_table_ref(t_emblem_paths_response));
    g_idle_add((GSourceFunc) reset_all_files, t_cvs);
}

static void on_connect(NautilusDropbox* t_cvs)
{
    reset_all_files(t_cvs);
    dropbox_command_client_send_command(&(t_cvs->dc.dcc), (NautilusDropboxCommandResponseHandler) get_emblem_paths_cb, t_cvs, "get_emblem_paths", nullptr);
}

static void on_disconnect(NautilusDropbox* t_cvs)
{
    reset_all_files(t_cvs);

    g_mutex_lock(t_cvs->emblem_paths_mutex);

    // This call will free the data too.
    g_idle_add((GSourceFunc) remove_emblem_paths, t_cvs->emblem_paths);
    t_cvs->emblem_paths = nullptr;
    g_mutex_unlock(t_cvs->emblem_paths_mutex);
}


static void nautilus_dropbox_menu_provider_iface_init (NautilusMenuProviderIface* t_iface)
{
    t_iface->get_file_items = nautilus_dropbox_get_file_items;
}

static void nautilus_dropbox_info_provider_iface_init (NautilusInfoProviderIface* t_iface) {
    t_iface->update_file_info = nautilus_dropbox_update_file_info;
    t_iface->cancel_update = nautilus_dropbox_cancel_update;
}

static void nautilus_dropbox_instance_init (NautilusDropbox* t_cvs)
{
    t_cvs->filename2obj = g_hash_table_new_full((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal, (GDestroyNotify) g_free, (GDestroyNotify) nullptr);
    t_cvs->obj2filename = g_hash_table_new_full((GHashFunc) g_direct_hash, (GEqualFunc) g_direct_equal, (GDestroyNotify) nullptr, (GDestroyNotify) g_free);
    t_cvs->emblem_paths_mutex = g_mutex_new();
    t_cvs->emblem_paths = nullptr;

    // Setup the connection object
    dropbox_client_setup(&(t_cvs->dc));

    // Our hooks
    nautilus_dropbox_hooks_add(&(t_cvs->dc.hookserv), "shell_touch", (DropboxUpdateHook) handle_shell_touch, t_cvs);

    // Add connection handlers
    dropbox_client_add_on_connect_hook(&(t_cvs->dc), (DropboxClientConnectHook) on_connect, t_cvs);
    dropbox_client_add_on_disconnect_hook(&(t_cvs->dc), (DropboxClientConnectHook) on_disconnect, t_cvs);

    // Now start the connection
    debug("about to start client connection");
    dropbox_client_start(&(t_cvs->dc));
}

static void nautilus_dropbox_class_init (NautilusDropboxClass* t_class)
{

}

static void nautilus_dropbox_class_finalize (NautilusDropboxClass* t_class)
{

}

GType nautilus_dropbox_get_type()
{
    return dropbox_type;
}

void nautilus_dropbox_register_type (GTypeModule* t_module) {
    static const GTypeInfo info = {
        sizeof (NautilusDropboxClass),
        (GBaseInitFunc) nullptr,
        (GBaseFinalizeFunc) nullptr,
        (GClassInitFunc) nautilus_dropbox_class_init,
        (GClassFinalizeFunc) nautilus_dropbox_class_finalize,
        nullptr,
        sizeof (NautilusDropbox),
        0,
        (GInstanceInitFunc) nautilus_dropbox_instance_init,
    };

    static const GInterfaceInfo menu_provider_iface_info = {
        (GInterfaceInitFunc) nautilus_dropbox_menu_provider_iface_init,
        nullptr,
        nullptr
    };

    static const GInterfaceInfo info_provider_iface_info = {
        (GInterfaceInitFunc) nautilus_dropbox_info_provider_iface_init,
        nullptr,
        nullptr
    };

    dropbox_type = g_type_module_register_type(module, G_TYPE_OBJECT, "NautilusDropbox", &info, 0);

    g_type_module_add_interface(module, dropbox_type, NAUTILUS_TYPE_MENU_PROVIDER, &menu_provider_iface_info);
    g_type_module_add_interface(module, dropbox_type, NAUTILUS_TYPE_INFO_PROVIDER, &info_provider_iface_info);
}
