/* entry.c - Directory entries.

   Copyright (C) 2026 Free Software Foundation, Inc.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include <stdlib.h>
#include <string.h>
#include "exfatfs.h"

/* Directories are read through a window of this many bytes.  It is a
   multiple of every sector size.  */
#define DIR_WINDOW	(16 * 4096)

error_t
exfat_dir_open (struct exfat_dir *dir, struct exfat_stream *stream,
		bool root)
{
  dir->stream = stream;
  dir->root = root;
  dir->pos = 0;
  dir->window_start = 0;
  dir->window_length = 0;
  dir->window = malloc (DIR_WINDOW);
  return dir->window ? 0 : ENOMEM;
}

void
exfat_dir_close (struct exfat_dir *dir)
{
  free (dir->window);
}

/* Point *ENTRY to the directory entry at offset POS of DIR, which must
   be inside the directory.  The pointer is good until the next call.  */
static error_t
dir_entry (struct exfat_dir *dir, off_t pos, const uint8_t **entry)
{
  if (pos < dir->window_start
      || pos >= dir->window_start + (off_t) dir->window_length)
    {
      error_t err;
      off_t start = pos - pos % DIR_WINDOW;
      size_t len = DIR_WINDOW;

      if (start + (off_t) len > round_cluster (dir->stream->length))
	len = round_cluster (dir->stream->length) - start;
      dir->window_length = 0;
      err = exfat_stream_read (dir->stream, start, dir->window, len);
      if (err)
	return err;
      dir->window_start = start;
      dir->window_length = len;
    }
  *entry = dir->window + (pos - dir->window_start);
  return 0;
}

/* Move DIR->pos to the next primary entry that is in use, at or after
   DIR->pos, and point *ENTRY to it.  At the end of the directory, set
   *ENTRY to null.  */
error_t
exfat_dir_next_primary (struct exfat_dir *dir, const uint8_t **entry)
{
  for (;; dir->pos += EXFAT_ENTRY_SIZE)
    {
      error_t err;
      const uint8_t *e;

      if (dir->pos + EXFAT_ENTRY_SIZE > dir->stream->length)
	break;
      err = dir_entry (dir, dir->pos, &e);
      if (err)
	return err;
      if (e[0] == 0)
	/* Every entry from here on is unused as well.  */
	break;
      /* An entry that is not in use, or a secondary entry that no set
	 claims.  */
      if (!(e[0] & EXFAT_TYPE_IN_USE) || (e[0] & EXFAT_TYPE_SECONDARY))
	continue;
      *entry = e;
      return 0;
    }
  *entry = NULL;
  return 0;
}
