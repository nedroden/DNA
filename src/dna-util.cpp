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
 */

#include <glib.h>
#include "dna-util.h"

namespace dna
{
    namespace cr
    {
        void read_line(int t_pos, GIOChannel* t_chan, int t_where)
        {
            while (true)
            {
                gchar *__line;
                gsize __line_length, __newline_pos;
                GIOStatus __iostat;

                __iostat = g_io_channel_read_line(chan, &__line, &__line_length, &__newline_pos, nullptr);

                if (__iostat == G_IO_STATUS_AGAIN)
                {
                    CRYIELD(t_pos);
                }
                else if (__iostat == G_IO_STATUS_NORMAL)
                {
                    *(__line + __newline_pos) = '\0';
                    t_where = __line;
                    break;
                }
                else if (__iostat == G_IO_STATUS_EOF || __iostat == G_IO_STATUS_ERROR)
                {
                    return false;
                }
                else
                {
                    g_assert_not_reached();
                    return false;
                }
            }
        }
    }
}