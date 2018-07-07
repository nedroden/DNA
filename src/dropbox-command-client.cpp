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

static void
do_file_info_command(GIOChannel* t_chan, DropboxFileInfoCommand* t_dfic, GError** t_gerr) {
  /* we need to send two requests to dropbox:
     file status, and folder_tags */
  GError *tmp_gerr = NULL;
  DropboxFileInfoCommandResponse *dficr;
  GHashTable *file_status_response = NULL, *args, *folder_tag_response = NULL, *emblems_response = NULL;
  gchar *filename = NULL;

  {
    gchar *filename_un, *uri;
    uri = nautilus_file_info_get_uri(dfic->file);
    filename_un = uri ? g_filename_from_uri(uri, NULL, NULL): NULL;
    g_free(uri);
    if (filename_un) {
      filename = g_filename_to_utf8(filename_un, -1, NULL, NULL, NULL);
      g_free(filename_un);
      if (filename == NULL) {
        /* oooh, filename wasn't correctly encoded. mark as  */
	debug("file wasn't correctly encoded %s", filename_un);
      }
    }
  }

  if (filename == NULL) {
    /* We couldn't get the filename.  Just return empty. */
    goto exit;
  }

  args = g_hash_table_new_full((GHashFunc) g_str_hash,
			       (GEqualFunc) g_str_equal,
			       (GDestroyNotify) g_free,
			       (GDestroyNotify) g_strfreev);
  {
    gchar **path_arg;
    path_arg = g_new(gchar *, 2);
    path_arg[0] = g_strdup(filename);
    path_arg[1] = NULL;
    g_hash_table_insert(args, g_strdup("path"), path_arg);
  }

  emblems_response = send_command_to_db(chan, "get_emblems", args, NULL);
  if (emblems_response) {
      /* Don't need to do the other calls. */
      g_hash_table_unref(args);
      goto exit;
  }

  /* send status command to server */
  file_status_response = send_command_to_db(chan, "icon_overlay_file_status",
					    args, &tmp_gerr);
  g_hash_table_unref(args);
  args = NULL;
  if (tmp_gerr != NULL) {
    g_free(filename);
    g_assert(file_status_response == NULL);
    g_propagate_error(gerr, tmp_gerr);
    return;
  }

  if (nautilus_file_info_is_directory(dfic->file)) {
    args = g_hash_table_new_full((GHashFunc) g_str_hash,
				 (GEqualFunc) g_str_equal,
				 (GDestroyNotify) g_free,
				 (GDestroyNotify) g_strfreev);
    {
      gchar **paths_arg;
      paths_arg = g_new(gchar *, 2);
      paths_arg[0] = g_strdup(filename);
      paths_arg[1] = NULL;
      g_hash_table_insert(args, g_strdup("path"), paths_arg);
    }
    
    folder_tag_response =
      send_command_to_db(chan, "get_folder_tag", args, &tmp_gerr);
    g_hash_table_unref(args);
    args = NULL;
    if (tmp_gerr != NULL) {
      if (file_status_response != NULL)
	g_hash_table_destroy(file_status_response);
      g_assert(folder_tag_response == NULL);
      g_propagate_error(gerr, tmp_gerr);
      return;
    }
  }
  
  /* great server responded perfectly,
     now let's get this request done,
     ...in the glib main loop */
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

static gboolean
finish_general_command(DropboxGeneralCommandResponse *dgcr) {
  if (dgcr->dgc->handler != NULL) {
    dgcr->dgc->handler(dgcr->response, dgcr->dgc->handler_ud);
  }
  
  if (dgcr->response != NULL) {
    g_hash_table_unref(dgcr->response);
  }

  g_free(dgcr->dgc->command_name);
  if (dgcr->dgc->command_args != NULL) {
    g_hash_table_unref(dgcr->dgc->command_args);
  }
  g_free(dgcr->dgc);
  g_free(dgcr);
  
  return FALSE;
}

static void
do_general_command(GIOChannel *chan, DropboxGeneralCommand *dcac,
		   GError **gerr) {
  GError *tmp_gerr = NULL;
  GHashTable *response;

  /* send status command to server */
  response = send_command_to_db(chan, dcac->command_name,
				dcac->command_args, &tmp_gerr);
  if (tmp_gerr != NULL) {
    g_assert(response == NULL);
    g_propagate_error(gerr, tmp_gerr);
    return;
  }

  /* great, the server did the command perfectly,
     now call the handler with the response */
  {
    DropboxGeneralCommandResponse *dgcr = g_new0(DropboxGeneralCommandResponse, 1);
    dgcr->dgc = dcac;
    dgcr->response = response;
    finish_general_command(dgcr);
  }
  
  return;
}

static gboolean
check_connection(GIOChannel *chan) {
  gchar fake_buf[4096];
  gsize bytes_read;
  GError *tmp_error = NULL;
  GIOFlags flags;
  GIOStatus ret, iostat;

  flags = g_io_channel_get_flags(chan);
  
  /* set non-blocking */
  ret = g_io_channel_set_flags(chan, flags | G_IO_FLAG_NONBLOCK, NULL);
  if (ret == G_IO_STATUS_ERROR) {
    return FALSE;
  }
  
  iostat = g_io_channel_read_chars(chan, fake_buf,
				   sizeof(fake_buf), 
				   &bytes_read, &tmp_error);

  ret = g_io_channel_set_flags(chan, flags, NULL);
  if (ret == G_IO_STATUS_ERROR) {
    return FALSE;
  }

  /* this makes us disconnect from bad servers
     (those that send us information without us asking for it) */
  return iostat == G_IO_STATUS_AGAIN;
}

static gpointer
dropbox_command_client_thread(DropboxCommandClient *data);

static void
end_request(DropboxCommand *dc) {
  if ((gpointer (*)(DropboxCommandClient *data)) dc != &dropbox_command_client_thread) {
    switch (dc->request_type) {
    case GET_FILE_INFO: {
      DropboxFileInfoCommand *dfic = (DropboxFileInfoCommand *) dc;
      DropboxFileInfoCommandResponse *dficr = g_new0(DropboxFileInfoCommandResponse, 1);
      dficr->dfic = dfic;
      dficr->file_status_response = NULL;
      dficr->emblems_response = NULL;
      g_idle_add((GSourceFunc) nautilus_dropbox_finish_file_info_command, dficr);
    }
      break;
    case GENERAL_COMMAND: {
      DropboxGeneralCommand *dgc = (DropboxGeneralCommand *) dc;
      DropboxGeneralCommandResponse *dgcr = g_new0(DropboxGeneralCommandResponse, 1);
      dgcr->dgc = dgc;
      dgcr->response = NULL;
      finish_general_command(dgcr);
    }
      break;
    default: 
      g_assert_not_reached();
      break;
    }
  }
}


static gpointer
dropbox_command_client_thread(DropboxCommandClient *dcc) {
  struct sockaddr_un addr;
  socklen_t addr_len;
  int connection_attempts = 1;

  /* intialize address structure */
  addr.sun_family = AF_UNIX;
  g_snprintf(addr.sun_path,
	     sizeof(addr.sun_path),
	     "%s/.dropbox/command_socket",
	     g_get_home_dir());
  addr_len = sizeof(addr) - sizeof(addr.sun_path) + strlen(addr.sun_path);

  while (1) {
    GIOChannel *chan = NULL;
    GError *gerr = NULL;
    int sock;
    gboolean failflag = TRUE;

    do {
      int flags;

      if (0 > (sock = socket(PF_UNIX, SOCK_STREAM, 0))) {
	/* WTF */
	break;
      }

      /* set timeout on socket, to protect against
	 bad servers */
      {
	struct timeval tv = {3, 0};
	if (0 > setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
			   &tv, sizeof(struct timeval)) ||
	    0 > setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
			   &tv, sizeof(struct timeval))) {
	  /* debug("setsockopt failed"); */
	  break;
	}
      }

      /* set native non-blocking, for connect timeout */
      {
	if ((flags = fcntl(sock, F_GETFL, 0)) < 0 ||
	    fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
	  /* debug("fcntl failed"); */
	  break;
	}
      }

      /* if there was an error we have to try again later */
      if (connect(sock, (struct sockaddr *) &addr, addr_len) < 0) {
	if (errno == EINPROGRESS) {
	  fd_set writers;
	  struct timeval tv = {1, 0};

	  FD_ZERO(&writers);
	  FD_SET(sock, &writers);

	  /* if nothing was ready after 3 seconds, fail out homie */
	  if (select(sock+1, NULL, &writers, NULL, &tv) == 0) {
	    /* debug("connection timeout"); */
	    break;
	  }

	  if (connect(sock, (struct sockaddr *) &addr, addr_len) < 0) {
	    /*	    debug("couldn't connect to command server after 1 second"); */
	    break;
	  }
	}
	/* errno != EINPROGRESS */
	else {
	  /*	  debug("bad connection"); */
	  break;
	}
      }

      /* set back to blocking */
      if (fcntl(sock, F_SETFL, flags) < 0) {
	/* debug("fcntl2 failed"); */
	break;
      }

      failflag = FALSE;
    } while (0);

    if (failflag) {
      ConnectionAttempt *ca = g_new(ConnectionAttempt, 1);
      ca->dcc = dcc;
      ca->connect_attempt = connection_attempts;
      g_idle_add((GSourceFunc) on_connection_attempt, ca);
      if (sock >= 0) {
	close(sock);
      }
      g_usleep(G_USEC_PER_SEC);
      connection_attempts++;
      continue;
    }
    else {
      connection_attempts = 0;
    }

    /* connected */
    debug("command client connected");

    chan = g_io_channel_unix_new(sock);
    g_io_channel_set_close_on_unref(chan, TRUE);
    g_io_channel_set_line_term(chan, "\n", -1);

#define SET_CONNECTED_STATE(s)     {			\
      g_mutex_lock(dcc->command_connected_mutex);	\
      dcc->command_connected = s;			\
      g_mutex_unlock(dcc->command_connected_mutex);	\
    }

    SET_CONNECTED_STATE(TRUE);

    g_idle_add((GSourceFunc) on_connect, dcc);

    while (1) {
      DropboxCommand *dc;

      while (1) {
	GTimeVal gtv;

	g_get_current_time(&gtv);
	g_time_val_add(&gtv, G_USEC_PER_SEC / 10);
	/* get a request from nautilus */
	dc = g_async_queue_timed_pop(dcc->command_queue, &gtv);
	if (dc != NULL) {
	  break;
	}
	else {
	  if (check_connection(chan) == FALSE) {
	    goto BADCONNECTION;
	  }
	}
      }

      /* this pointer should be unique */
      if ((gpointer (*)(DropboxCommandClient *data)) dc == &dropbox_command_client_thread) {
	debug("got a reset request");
	goto BADCONNECTION;
      }

      switch (dc->request_type) {
      case GET_FILE_INFO: {
	debug("doing file info command");
	do_file_info_command(chan, (DropboxFileInfoCommand *) dc, &gerr);
      }
	break;
      case GENERAL_COMMAND: {
	debug("doing general command");
	do_general_command(chan, (DropboxGeneralCommand *) dc, &gerr);
      }
	break;
      default: 
	g_assert_not_reached();
	break;
      }
      
      debug("done.");

      if (gerr != NULL) {
	//	debug("COMMAND ERROR*****************************");
	/* mark this request as never to be completed */
	end_request(dc);

	debug("command error: %s", gerr->message);
	
	g_error_free(gerr);
      BADCONNECTION:
	/* grab all the rest of the data off the async queue and mark it
	   never to be completed, who knows how long we'll be disconnected */
	while ((dc = g_async_queue_try_pop(dcc->command_queue)) != NULL) {
	  end_request(dc);
	}

	g_io_channel_unref(chan);

	SET_CONNECTED_STATE(FALSE);

	/* call the disconnect handler */
	g_idle_add((GSourceFunc) on_disconnect, dcc);

	break;
      }
    }
    
#undef SET_CONNECTED_STATE
  }
  
  return NULL;
}

/* thread safe */
gboolean
dropbox_command_client_is_connected(DropboxCommandClient *dcc) {
  gboolean command_connected;

  g_mutex_lock(dcc->command_connected_mutex);
  command_connected = dcc->command_connected;
  g_mutex_unlock(dcc->command_connected_mutex);

  return command_connected;
}

/* thread safe */
void dropbox_command_client_force_reconnect(DropboxCommandClient *dcc) {
  if (dropbox_command_client_is_connected(dcc) == TRUE) {
    debug("forcing command to reconnect");
    dropbox_command_client_request(dcc, (DropboxCommand *) &dropbox_command_client_thread);
  }
}

/* thread safe */
void
dropbox_command_client_request(DropboxCommandClient *dcc, DropboxCommand *dc) {
  g_async_queue_push(dcc->command_queue, dc);
}

/* should only be called once on initialization */
void
dropbox_command_client_setup(DropboxCommandClient *dcc) {
  dcc->command_queue = g_async_queue_new();
  dcc->command_connected_mutex = g_mutex_new();
  dcc->command_connected = FALSE;
  dcc->ca_hooklist = NULL;

  g_hook_list_init(&(dcc->ondisconnect_hooklist), sizeof(GHook));
  g_hook_list_init(&(dcc->onconnect_hooklist), sizeof(GHook));
}

void
dropbox_command_client_add_on_disconnect_hook(DropboxCommandClient *dcc,
					      DropboxCommandClientConnectHook dhcch,
					      gpointer ud) {
  GHook *newhook;
  
  newhook = g_hook_alloc(&(dcc->ondisconnect_hooklist));
  newhook->func = dhcch;
  newhook->data = ud;
  
  g_hook_append(&(dcc->ondisconnect_hooklist), newhook);
}

void
dropbox_command_client_add_on_connect_hook(DropboxCommandClient *dcc,
					   DropboxCommandClientConnectHook dhcch,
					   gpointer ud) {
  GHook *newhook;
  
  newhook = g_hook_alloc(&(dcc->onconnect_hooklist));
  newhook->func = dhcch;
  newhook->data = ud;
  
  g_hook_append(&(dcc->onconnect_hooklist), newhook);
}

void
dropbox_command_client_add_connection_attempt_hook(DropboxCommandClient *dcc,
						   DropboxCommandClientConnectionAttemptHook dhcch,
						   gpointer ud) {
  DropboxCommandClientConnectionAttempt *newhook;

  debug("shouldn't be here...");
  
  newhook = g_new(DropboxCommandClientConnectionAttempt, 1);
  newhook->h = dhcch;
  newhook->ud = ud;

  dcc->ca_hooklist = g_list_append(dcc->ca_hooklist, newhook);
}

/* should only be called once on initialization */
void
dropbox_command_client_start(DropboxCommandClient *dcc) {
  /* setup the connect to the command server */
  debug("starting command thread");
  g_thread_create((gpointer (*)(gpointer data)) dropbox_command_client_thread,
		  dcc, FALSE, NULL);
}

/* thread safe */
void dropbox_command_client_send_simple_command(DropboxCommandClient *dcc, 
					 const char *command) {
  DropboxGeneralCommand *dgc;
  
  dgc = g_new(DropboxGeneralCommand, 1);
  
  dgc->dc.request_type = GENERAL_COMMAND;
  dgc->command_name = g_strdup(command);
  dgc->command_args = NULL;
  dgc->handler = NULL;
  dgc->handler_ud = NULL;
  
  dropbox_command_client_request(dcc, (DropboxCommand *) dgc);
}

/* thread safe */
/* this is the C API, there is another send_command_to_db
   that is more the actual over the wire command */
void dropbox_command_client_send_command(DropboxCommandClient *dcc, 
					 NautilusDropboxCommandResponseHandler h,
					 gpointer ud,
					 const char *command, ...) {
  va_list ap;
  DropboxGeneralCommand *dgc;
  gchar *na;
  va_start(ap, command);

  dgc = g_new(DropboxGeneralCommand, 1);
  dgc->dc.request_type = GENERAL_COMMAND;
  dgc->command_name = g_strdup(command);
  dgc->command_args = g_hash_table_new_full((GHashFunc) g_str_hash,
					    (GEqualFunc) g_str_equal,
					    (GDestroyNotify) g_free,
					    (GDestroyNotify) g_strfreev);
  /*
   * NB: The handler is called in the DropboxCommandClient Thread.  If you need
   * it in the main thread you must call g_idle_add in the callback.
   */
  dgc->handler = h;
  dgc->handler_ud = ud;

  while ((na = va_arg(ap, char *)) != NULL) {
    gchar **is_active_arg;

    is_active_arg = g_new(gchar *, 2);

    g_hash_table_insert(dgc->command_args,
			g_strdup(na), is_active_arg);

    is_active_arg[0] = g_strdup(va_arg(ap, char *));
    is_active_arg[1] = NULL;
  }
  va_end(ap);

  dropbox_command_client_request(dcc, (DropboxCommand *) dgc);
}
