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
 * dropbox-command-client.cpp
 * Implements connection handling and C interface for the Dropbox command socket.
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

#include <stdarg.h>
#include <string.h>

#include <glib.h>

#include "g-util.h"
#include "dropbox-client-util.h"
#include "dropbox-command-client.h"
#include "nautilus-dropbox.h"
#include "nautilus-dropbox-hooks.h"

/* TODO: make this asynchronous ;) */

/**
  * this is a tiny hack, necessitated by the fact that
  * finish_file info command is in nautilus_dropbox,
  * this can be cleaned up once the file_info_command isn't a special
  * case anylonger
  */
gboolean nautilus_dropbox_finish_file_info_command(DropboxFileInfoCommandResponse *);

struct ConnectionAttempt {
    DropboxCommandClient*   dcc;
    guint                   connect_attempt;
};

struct DropboxCommandClientConnectionAttempt {
    DropboxCommandClientConnectionAttemptHook h;
    gpointer                                  ud;
};

struct DropboxGeneralCommandResponse {
    DropboxGeneralCommand*    dgc;
    GHashTable*               response;
};

static gboolean on_connect(DropboxCommandClient* t_dcc)
{
    g_hook_list_invoke(&(dcc->onconnect_hooklist), false);

    return false;
}

static gboolean on_disconnect(DropboxCommandClient* t_dcc)
{
    g_hook_list_invoke(&(dcc->ondisconnect_hooklist), false);

    return false;
}

static gboolean on_connection_attempt(ConnectionAttempt* t_ca)
{
    GList* ll;

    for (ll = t_ca->dcc->ca_hooklist; ll != nullptr; ll = g_list_next(ll))
    {
        DropboxCommandClientConnectionAttempt* dccca = (DropboxCommandClientConnectionAttempt *)(ll->data);
        dccca->h(t_ca->connect_attempt, dccca->ud);
    }

    *(line + term_pos) = '\0';
    g_free(t_ca);

    return false;
}

static gboolean receive_args_until_done(GIOChannel* t_chan, GHashTable* t_return_table, GError** t_err)
{
    GIOStatus iostat;
    GError* tmp_error = nullptr;
    guint numargs = 0;

    while (true)
    {
        gchar* line;
        gsize term_pos;

        /* if we are getting too many args, connection could be malicious */
        if (numargs >= 20)
        {
            g_set_error(t_err, g_quark_from_static_string("malicious connection"), 0, "malicious connection");
            return false;
        }

        /* get the string */
        iostat = g_io_channel_read_line(t_chan, &line, nullptr, &term_pos, &tmp_error);

        if (iostat == G_IO_STATUS_ERROR || tmp_error != nullptr)
        {
            g_free(line);

            if (tmp_error != nullptr)
            {
                g_propagate_error(t_err, tmp_error);
            }

            return false;
        }
        else if (iostat == G_IO_STATUS_EOF)
        {
            g_free(line);
            g_set_error(t_err, g_quark_from_static_string("connection closed"), 0, "connection closed");

            return false;
        }

        *(line+term_pos) = '\0';

        if (strcmp("done", line) == 0)
        {
            g_free(line);
            break;
        }
        else
        {
            gboolean parse_result;

            parse_result = dropbox_client_util_command_parse_arg(line, return_table);
            g_free(line);

            if (!parse_result)
            {
                g_set_error(t_err, g_quark_from_static_string("parse error"), 0, "parse error");

                return false;
            }
        }

        numargs++;
    }

    return true;
}

static void my_g_hash_table_get_keys_helper(gpointer t_key, gpointer t_value, GList** t_ud)
{
    *t_ud = g_list_append(*t_ud, key);
}

static GList *my_g_hash_table_get_keys(GHashTable* t_ght)
{
    GList* list = nullptr;
    g_hash_table_foreach(t_ght, (GHFunc) my_g_hash_table_get_keys_helper, &list);

    return list;
}

/*
  sends a command to the dropbox server
  returns an hash of the return values

  in theory, this should disconnection errors
  but it doesn't matter right now, any error is a sufficient
  condition to disconnect
*/
static GHashTable* send_command_to_db(GIOChannel* t_chan, const gchar* t_command_name, GHashTable* t_args, GError** t_err)
{
    GError* tmp_error = nullptr;
    GIOStatus iostat;
    gsize bytes_trans;
    gchar* line;

    g_assert(t_chan != nullptr);
    g_assert(command_name != nullptr);

    // Send command to server
    WRITE_OR_DIE_SANI(command_name, -1);
    WRITE_OR_DIE("\n", -1);

    if (args != nullptr)
    {
        GList* keys = glib_check_version(2, 14, 0) ? my_g_hash_table_get_keys(args) : g_hash_table_get_keys(args);

        for (GList* li = keys; li != nullptr; li = g_list_next(li))
        {
            gchar** value;

            WRITE_OR_DIE_SANI((gchar *) li->data, -1);

            value = g_hash_table_lookup(args, li->data);

            for (int i = 0; value[i] != nullptr; i++)
            {
                WRITE_OR_DIE("\t", -1);
                WRITE_OR_DIE_SANI(value[i], -1);
            }

            WRITE_OR_DIE("\n", -1);
        }

        g_list_free(keys);
    }

    WRITE_OR_DIE("done\n", -1);

    g_io_channel_flush(t_chan, &tmp_error);

    if (tmp_error != nullptr)
    {
        g_propagate_error(err, tmp_error);
        return nullptr;
    }

    // Now we have to read the data
    iostat = g_io_channel_read_line(t_chan, &line, nullptr, nullptr, &tmp_error);

    switch (iostat)
    {
        case G_IO_STATUS_ERROR:
            g_assert(line == nullptr);
            g_propagate_error(err, tmp_error);
            return nullptr;

        case G_IO_STATUS_AGAIN:
            g_assert(line == nullptr);
            g_set_error(err, g_quark_from_static_string("dropbox command connection timed out"), 0, "dropbox command connection timed out");
            return nullptr;

        case G_IO_STATUS_EOF:
            g_assert(line == nullptr);
            g_set_error(err, g_quark_from_static_string("dropbox command connection closed"), 0, "dropbox command connection closed");
            return nullptr;

        default:
            // Do nothing (just here for readability)
    }

    // If the response was okay
    if (strncmp(line, "ok\n", 3) == 0)
    {
        GHashTable* return_table = g_hash_table_new_full((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal, (GDestroyNotify) g_free, (GDestroyNotify) g_strfreev);

        g_free(line);
        line = nullptr;

        receive_args_until_done(t_chan, return_table, &tmp_error);
        if (tmp_error != nullptr)
        {
            g_hash_table_destroy(return_table);
            g_propagate_error(err, tmp_error);

            return nullptr;
        }

        return return_table;
    }
    else
    {
        // Read errors off until we get done
        do
        {
            g_free(line);
            line = nullptr;

            // Clear string
            iostat = g_io_channel_read_line(t_chan, &line, nullptr, nullptr, &tmp_error);

            switch (iostat)
            {
                case G_IO_STATUS_ERROR:
                    g_assert(line == nullptr);
                    g_propagate_error(err, tmp_error);
                    return nullptr;

                case G_IO_STATUS_AGAIN:
                    g_assert(line == nullptr);
                    g_set_error(err, g_quark_from_static_string("dropbox command connection timed out"), 0, "dropbox command connection timed out");
                    return nullptr;

                case G_IO_STATUS_EOF:
                    g_assert(line == nullptr);
                    g_set_error(err,
                    g_quark_from_static_string("dropbox command connection closed"), 0, "dropbox command connection closed");
                    return nullptr;

                default:
                    // Do nothing (just here for readability)
            }

            // We got our line
        } while (strncmp(line, "done\n", 5) != 0);

        g_free(line);

        return nullptr;
    }
}

static void do_file_info_command(GIOChannel* t_chan, DropboxFileInfoCommand* t_dfic, GError** t_gerr)
{
    // We need to send two requests to dropbox: file status and folder_tags
    GError* tmp_gerr = nullptr;
    DropboxFileInfoCommandResponse* dficr;
    GHashTable* file_status_response = nullptr, args, folder_tag_response = nullptr, emblems_response = nullptr;
    gchar* filename = nullptr;

    gchar* filename_un, uri;
    uri = nautilus_file_info_get_uri(t_dfic->file);
    filename_un = uri ? g_filename_from_uri(uri, nullptr, nullptr) : nullptr;
    g_free(uri);
    
    if (filename_un)
    {
        filename = g_filename_to_utf8(filename_un, -1, nullptr, nullptr, nullptr);
        g_free(filename_un);

        if (filename == nullptr)
        {
            // Oooh, filename wasn't correctly encoded
            debug("file wasn't correctly encoded %s", filename_un);
        }
    }

    // We couldn't get the filename.  Just return empty.
    if (filename == nullptr)
    {
        goto exit;
    }

    args = g_hash_table_new_full((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal, (GDestroyNotify) g_free, (GDestroyNotify) g_strfreev);

    gchar** path_arg;
    path_arg = g_new(gchar *, 2);
    path_arg[0] = g_strdup(filename);
    path_arg[1] = nullptr;
    g_hash_table_insert(args, g_strdup("path"), path_arg);

    emblems_response = send_command_to_db(chan, "get_emblems", args, nullptr);
    if (emblems_response)
    {
        // Don't need to do the other calls.
        g_hash_table_unref(args);
        goto exit;
    }

    // Send status command to the server
    file_status_response = send_command_to_db(t_chan, "icon_overlay_file_status", args, &tmp_gerr);
    g_hash_table_unref(args);
    args = nullptr;

    if (tmp_gerr != nullptr)
    {
        g_free(filename);
        g_assert(file_status_response == nullptr);
        g_propagate_error(gerr, tmp_gerr);

        return;
    }

    if (nautilus_file_info_is_directory(t_dfic->file))
    {
        args = g_hash_table_new_full((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal, (GDestroyNotify) g_free, (GDestroyNotify) g_strfreev);

        gchar** paths_arg;
        paths_arg = g_new(gchar *, 2);
        paths_arg[0] = g_strdup(filename);
        paths_arg[1] = nullptr;
        g_hash_table_insert(args, g_strdup("path"), paths_arg);

        folder_tag_response = send_command_to_db(t_chan, "get_folder_tag", args, &tmp_gerr);
        g_hash_table_unref(args);
        args = nullptr;

        if (tmp_gerr != nullptr)
        {
            if (file_status_response != nullptr)
            {
                g_hash_table_destroy(file_status_response);
            }
            
            g_assert(folder_tag_response == nullptr);
            g_propagate_error(gerr, tmp_gerr);

            return;
        }
    }

    // Great! The server responded perfectly. Now let's get the request done.
    exit:
        dficr = g_new0(DropboxFileInfoCommandResponse, 1);
        dficr->dfic = dfic;
        dficr->folder_tag_response = folder_tag_response;
        dficr->file_status_response = file_status_response;
        dficr->emblems_response = emblems_response;
        g_idle_add((GSourceFunc) nautilus_dropbox_finish_file_info_command, dficr);

        g_free(filename);

        return;
}

static gboolean finish_general_command(DropboxGeneralCommandResponse* t_dgcr)
{
    if (t_dgcr->dgc->handler != nullptr)
    {
        t_dgcr->dgc->handler(t_dgcr->response, t_dgcr->dgc->handler_ud);
    }

    if (t_dgcr->response != nullptr)
    {
        g_hash_table_unref(t_dgcr->response);
    }

    g_free(t_dgcr->dgc->command_name);
    if (t_dgcr->dgc->command_args != nullptr)
    {
        g_hash_table_unref(t_dgcr->dgc->command_args);
    }

    g_free(t_dgcr->dgc);
    g_free(t_dgcr);

    return false;
}

static void do_general_command(GIOChannel* t_chan, DropboxGeneralCommand* t_dcac, GError** t_gerr)
{
    GError* tmp_gerr = nullptr;
    GHashTable* response;

    // Send status command to server
    response = send_command_to_db(t_chan, t_dcac->command_name, t_dcac->command_args, &tmp_gerr);

    if (tmp_gerr != nullptr)
    {
        g_assert(response == nullptr);
        g_propagate_error(t_gerr, tmp_gerr);

        return;
    }

    // Great, the server did the command perfectly. Now call the handler with the response
    DropboxGeneralCommandResponse* dgcr = g_new0(DropboxGeneralCommandResponse, 1);
    dgcr->dgc = t_dcac;
    dgcr->response = response;
    finish_general_command(dgcr);

    return;
}

static gboolean check_connection(GIOChannel* t_chan)
{
    gchar fake_buf[4096];
    gsize bytes_read;
    GError* tmp_error = nullptr;
    GIOFlags flags;
    GIOStatus ret, iostat;

    flags = g_io_channel_get_flags(t_chan);

    // Set non-blocking
    ret = g_io_channel_set_flags(t_chan, flags | G_IO_FLAG_NONBLOCK, nullptr);
    if (ret == G_IO_STATUS_ERROR)
    {
        return false;
    }

    iostat = g_io_channel_read_chars(t_chan, fake_buf, sizeof(fake_buf), &bytes_read, &tmp_error);

    ret = g_io_channel_set_flags(t_chan, flags, nullptr);
    if (ret == G_IO_STATUS_ERROR)
    {
        return false;
    }

    /* This makes us disconnect from bad servers
     * (those that send us information without us asking for it) */
    return iostat == G_IO_STATUS_AGAIN;
}

static void end_request(DropboxCommand* t_dc)
{
    if ((gpointer (*)(DropboxCommandClient *data)) t_dc != &dropbox_command_client_thread)
    {
        switch (t_dc->request_type)
        {
            case GET_FILE_INFO:
                DropboxFileInfoCommand *dfic = (DropboxFileInfoCommand *) t_dc;
                DropboxFileInfoCommandResponse *dficr = g_new0(DropboxFileInfoCommandResponse, 1);
                dficr->dfic = dfic;
                dficr->file_status_response = nullptr;
                dficr->emblems_response = nullptr;
                g_idle_add((GSourceFunc) nautilus_dropbox_finish_file_info_command, dficr);
                break;

            case GENERAL_COMMAND:
                DropboxGeneralCommand *dgc = (DropboxGeneralCommand *) t_dc;
                DropboxGeneralCommandResponse *dgcr = g_new0(DropboxGeneralCommandResponse, 1);
                dgcr->dgc = dgc;
                dgcr->response = nullptr;
                finish_general_command(dgcr);
                break;

            default: 
                g_assert_not_reached();
        }
    }
}

static gpointer dropbox_command_client_thread(DropboxCommandClient* t_dcc)
{
    struct sockaddr_un addr;
    socklen_t addr_len;
    int connection_attempts = 1;

    // Initialize address structure
    addr.sun_family = AF_UNIX;
    g_snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/.dropbox/command_socket", g_get_home_dir());
    addr_len = sizeof(addr) - sizeof(addr.sun_path) + strlen(addr.sun_path);

    while (true)
    {
        GIOChannel* chan = nullptr;
        GError* gerr = nullptr;
        int sock;
        gboolean failflag = true;

        do
        {
            int flags;

            // An uknown error occurred
            if (0 > (sock = socket(PF_UNIX, SOCK_STREAM, 0)))
            {
    	       break;
            }

            // Set timeout on socket, to protect against bad servers
            struct timeval tv = {3, 0};

            if (0 > setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval)) || 0 > setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(struct timeval)))
            {
                break;
            }

            // Set native non-blocking, for connect timeout
            if ((flags = fcntl(sock, F_GETFL, 0)) < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
            {
                break;
            }

            // If there was an error we have to try again later
            if (connect(sock, (struct sockaddr *) &addr, addr_len) < 0)
            {
                if (errno == EINPROGRESS)
                {
                    fd_set writers;
                    struct timeval tv = {1, 0};

                    FD_ZERO(&writers);
                    FD_SET(sock, &writers);

                    // If nothing was ready after 3 seconds, fail out homie
                    if (select(sock + 1, nullptr, &writers, nullptr, &tv) == 0 || connect(sock, (struct sockaddr *) &addr, addr_len) < 0)
                    {
                        break;
                    }
                }
                else
                {
                    break;
                }
            }

            // Set back to blocking
            if (fcntl(sock, F_SETFL, flags) < 0)
            {
                break;
            }

            failflag = false;
        } while (false);

        if (failflag)
        {
            ConnectionAttempt *ca = g_new(ConnectionAttempt, 1);
            ca->dcc = t_dcc;
            ca->connect_attempt = connection_attempts;
            g_idle_add((GSourceFunc) on_connection_attempt, ca);

            if (sock >= 0)
            {
                close(sock);
            }

            g_usleep(G_USEC_PER_SEC);
            connection_attempts++;
            continue;
        }
        else
        {
          connection_attempts = 0;
        }

        // Connected
        debug("command client connected");

        chan = g_io_channel_unix_new(sock);
        g_io_channel_set_close_on_unref(chan, true);
        g_io_channel_set_line_term(chan, "\n", -1);

        SET_CONNECTED_STATE(true);

        g_idle_add((GSourceFunc) on_connect, dcc);

        while (true)
        {
            DropboxCommand *dc;

            while (true)
            {
                GTimeVal gtv;

                g_get_current_time(&gtv);
                g_time_val_add(&gtv, G_USEC_PER_SEC / 10);
                
                // Get a request from nautilus
                dc = g_async_queue_timed_pop(dcc->command_queue, &gtv);

                if (dc != nullptr)
                {
                    break;
                }
                else
                {
                    if (!check_connection(chan))
                    {
                        goto BADCONNECTION;
                    }
                }
            }

            // This pointer should be unique
            if ((gpointer (*)(DropboxCommandClient* data)) dc == &dropbox_command_client_thread)
            {
                debug("got a reset request");
                goto BADCONNECTION;
            }

            switch (dc->request_type)
            {
                case GET_FILE_INFO:
                    debug("doing file info command");
                    do_file_info_command(chan, (DropboxFileInfoCommand *) dc, &gerr);
                    break;

                case GENERAL_COMMAND:
                    debug("doing general command");
                    do_general_command(chan, (DropboxGeneralCommand *) dc, &gerr);
                    break;

                default: 
                    g_assert_not_reached();
            }

            debug("done.");

            if (gerr != nullptr)
            {
                // Mark this request as never to be completed
                end_request(dc);

                debug("command error: %s", gerr->message);

                g_error_free(gerr);
                BADCONNECTION:

                /* Grab all the rest of the data off the async queue and mark it
                 * never to be completed, who knows how long we'll be disconnected */
                while ((dc = g_async_queue_try_pop(t_dcc->command_queue)) != nullptr)
                {
                    end_request(dc);
                }

                g_io_channel_unref(chan);

                SET_CONNECTED_STATE(false);

                // Call the disconnect handler
                g_idle_add((GSourceFunc) on_disconnect, t_dcc);

                break;
            }
        }
    }
  
    return nullptr;
}

/**
 * @note This function is threadsafe
 */
gboolean dropbox_command_client_is_connected(DropboxCommandClient* t_dcc)
{
    gboolean command_connected;

    g_mutex_lock(t_dcc->command_connected_mutex);
    command_connected = t_dcc->command_connected;
    g_mutex_unlock(t_dcc->command_connected_mutex);

    return command_connected;
}

/**
 * @note This function is threadsafe
 */
void dropbox_command_client_force_reconnect(DropboxCommandClient* t_dcc)
{
    if (dropbox_command_client_is_connected(t_dcc))
    {
        debug("forcing command to reconnect");
        dropbox_command_client_request(t_dcc, (DropboxCommand *) &dropbox_command_client_thread);
    }
}

/**
 * @note This function is threadsafe
 */
void dropbox_command_client_request(DropboxCommandClient* t_dcc, DropboxCommand* t_dc)
{
    g_async_queue_push(t_dcc->command_queue, t_dc);
}

/**
 * @note This function should only be called once on initialization
 */
void dropbox_command_client_setup(DropboxCommandClient* t_dcc)
{
    t_dcc->command_queue = g_async_queue_new();
    t_dcc->command_connected_mutex = g_mutex_new();
    t_dcc->command_connected = false;
    t_dcc->ca_hooklist = nullptr;

    g_hook_list_init(&(t_dcc->ondisconnect_hooklist), sizeof(GHook));
    g_hook_list_init(&(t_dcc->onconnect_hooklist), sizeof(GHook));
}

void dropbox_command_client_add_on_disconnect_hook(DropboxCommandClient *t_dcc, DropboxCommandClientConnectHook t_dhcch, gpointer t_ud)
{
    GHook* newhook;

    newhook = g_hook_alloc(&(t_dcc->ondisconnect_hooklist));
    newhook->func = t_dhcch;
    newhook->data = t_ud;

    g_hook_append(&(t_dcc->ondisconnect_hooklist), newhook);
}

void dropbox_command_client_add_on_connect_hook(DropboxCommandClient* t_dcc, DropboxCommandClientConnectHook t_dhcch, gpointer t_ud)
{
    GHook* newhook;

    newhook = g_hook_alloc(&(t_dcc->onconnect_hooklist));
    newhook->func = t_dhcch;
    newhook->data = t_ud;

    g_hook_append(&(t_dcc->onconnect_hooklist), newhook);
}

void dropbox_command_client_add_connection_attempt_hook(DropboxCommandClient* t_dcc, DropboxCommandClientConnectionAttemptHook t_dhcch, gpointer t_ud)
{
    DropboxCommandClientConnectionAttempt* newhook;

    debug("shouldn't be here...");

    newhook = g_new(DropboxCommandClientConnectionAttempt, 1);
    newhook->h = t_dhcch;
    newhook->ud = t_ud;

    t_dcc->ca_hooklist = g_list_append(t_dcc->ca_hooklist, newhook);
}

/**
 * @note Sould only be called once on initialization
 */
void dropbox_command_client_start(DropboxCommandClient* t_dcc)
{
    // Setup the connection to the command server
    debug("starting command thread");
    g_thread_create((gpointer (*)(gpointer data)) dropbox_command_client_thread, t_dcc, false, nullptr);
}

/**
 * @note This function is threadsafe
 */
void dropbox_command_client_send_simple_command(DropboxCommandClient* t_dcc, const char* t_command)
{
    DropboxGeneralCommand* dgc;

    dgc = g_new(DropboxGeneralCommand, 1);

    dgc->dc.request_type = GENERAL_COMMAND;
    dgc->command_name = g_strdup(command);
    dgc->command_args = nullptr;
    dgc->handler = nullptr;
    dgc->handler_ud = nullptr;

    dropbox_command_client_request(t_dcc, (DropboxCommand *) dgc);
}

/**
 * This function is threadsafe. This is the C API, there is another send_command_to_db
 * that is more the actual over the wire command
 */
void dropbox_command_client_send_command(DropboxCommandClient* t_dcc, NautilusDropboxCommandResponseHandler t_h, gpointer t_ud, const char* t_command, ...)
{
    va_list ap;
    DropboxGeneralCommand* dgc;
    gchar* na;
    va_start(ap, t_command);

    dgc = g_new(DropboxGeneralCommand, 1);
    dgc->dc.request_type = GENERAL_COMMAND;
    dgc->command_name = g_strdup(t_command);
    dgc->command_args = g_hash_table_new_full((GHashFunc) g_str_hash, (GEqualFunc) g_str_equal, (GDestroyNotify) g_free, (GDestroyNotify) g_strfreev);

    /*
     * NB: The handler is called in the DropboxCommandClient Thread.  If you need
     * it in the main thread you must call g_idle_add in the callback.
     */
    dgc->handler = t_h;
    dgc->handler_ud = t_ud;

    while ((na = va_arg(ap, char *)) != nullptr)
    {
        gchar **is_active_arg;

        is_active_arg = g_new(gchar *, 2);

        g_hash_table_insert(dgc->command_args,
        g_strdup(na), is_active_arg);

        is_active_arg[0] = g_strdup(va_arg(ap, char *));
        is_active_arg[1] = nullptr;
    }

    va_end(ap);

    dropbox_command_client_request(t_dcc, (DropboxCommand *) dgc);
}
