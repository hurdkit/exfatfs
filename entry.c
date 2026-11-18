/* entry.c - Directory entries, entry sets and file names.

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
#include <time.h>
#include "exfatfs.h"

/* Directories are read through a window of this many bytes.  It is a
   multiple of every sector size.  */
#define DIR_WINDOW	(16 * 4096)

static bool
in_use_secondary (uint8_t type)
{
  return (type & (EXFAT_TYPE_IN_USE | EXFAT_TYPE_SECONDARY))
    == (EXFAT_TYPE_IN_USE | EXFAT_TYPE_SECONDARY);
}

static uint16_t
checksum16 (uint16_t sum, const uint8_t *data, size_t len)
{
  for (size_t i = 0; i < len; i++)
    sum = (uint16_t) (sum << 15 | sum >> 1) + data[i];
  return sum;
}


/* Names.  */

/* Convert the LENGTH UTF-16 code units of NAME to UTF-8 in OUT, which
   has room for EXFAT_UTF8_NAME_MAX + 1 bytes, and return the length of
   the result.  An unpaired surrogate is encoded like any other code
   point, so that a name which holds one can still be looked up.  */
size_t
exfat_name_to_utf8 (const uint16_t *name, unsigned length, char *out)
{
  char *p = out;

  for (unsigned i = 0; i < length; i++)
    {
      uint32_t c = name[i];

      if (c >= 0xd800 && c < 0xdc00 && i + 1 < length
	  && name[i + 1] >= 0xdc00 && name[i + 1] < 0xe000)
	c = 0x10000 + ((c - 0xd800) << 10) + (name[++i] - 0xdc00);

      if (c < 0x80)
	*p++ = c;
      else if (c < 0x800)
	{
	  *p++ = 0xc0 | c >> 6;
	  *p++ = 0x80 | (c & 0x3f);
	}
      else if (c < 0x10000)
	{
	  *p++ = 0xe0 | c >> 12;
	  *p++ = 0x80 | (c >> 6 & 0x3f);
	  *p++ = 0x80 | (c & 0x3f);
	}
      else
	{
	  *p++ = 0xf0 | c >> 18;
	  *p++ = 0x80 | (c >> 12 & 0x3f);
	  *p++ = 0x80 | (c >> 6 & 0x3f);
	  *p++ = 0x80 | (c & 0x3f);
	}
    }
  *p = '\0';
  return p - out;
}

/* Prepare the UTF-8 string NAME for comparison with the names on the
   volume.  Return false if no file on the volume can have that name.  */
bool
exfat_name_key (const char *name, struct exfat_name_key *key)
{
  const unsigned char *p = (const unsigned char *) name;
  uint8_t bytes[2];

  key->length = 0;
  key->hash = 0;
  while (*p)
    {
      uint32_t c = *p++;
      int more;
      uint32_t min;
      uint16_t units[2];
      int n = 1;

      if (c < 0x80)
	more = 0, min = 0;
      else if ((c & 0xe0) == 0xc0)
	more = 1, min = 0x80, c &= 0x1f;
      else if ((c & 0xf0) == 0xe0)
	more = 2, min = 0x800, c &= 0x0f;
      else if ((c & 0xf8) == 0xf0)
	more = 3, min = 0x10000, c &= 0x07;
      else
	return false;
      while (more-- > 0)
	{
	  if ((*p & 0xc0) != 0x80)
	    return false;
	  c = c << 6 | (*p++ & 0x3f);
	}
      if (c < min || c > 0x10ffff)
	return false;

      if (c < 0x10000)
	units[0] = c;
      else
	{
	  units[0] = 0xd800 + ((c - 0x10000) >> 10);
	  units[1] = 0xdc00 + ((c - 0x10000) & 0x3ff);
	  n = 2;
	}
      if (key->length + n > EXFAT_NAME_MAX)
	return false;
      for (int i = 0; i < n; i++)
	{
	  uint16_t up = exfat_upcase[units[i]];

	  key->upcased[key->length++] = up;
	  bytes[0] = up & 0xff;
	  bytes[1] = up >> 8;
	  key->hash = checksum16 (key->hash, bytes, 2);
	}
    }
  return key->length > 0;
}

/* Return whether SET's name is KEY's, ignoring case.  */
bool
exfat_name_match (const struct exfat_set *set,
		  const struct exfat_name_key *key)
{
  if (set->name_length != key->length)
    return false;
  /* A set that failed validation has no hash to trust.  */
  if (!set->error && set->name_hash != key->hash)
    return false;
  for (unsigned i = 0; i < key->length; i++)
    if (exfat_upcase[set->name[i]] != key->upcased[i])
      return false;
  return true;
}


/* Entry sets.  */

static struct timespec
decode_time (uint32_t stamp, uint8_t increment, uint8_t utc_offset)
{
  struct timespec ts = { 0, 0 };
  struct tm tm =
    {
      .tm_sec = (stamp & 0x1f) * 2,
      .tm_min = stamp >> 5 & 0x3f,
      .tm_hour = stamp >> 11 & 0x1f,
      .tm_mday = stamp >> 16 & 0x1f,
      .tm_mon = (int) (stamp >> 21 & 0x0f) - 1,
      .tm_year = (int) (stamp >> 25) + 80,
      .tm_isdst = -1,
    };

  if (utc_offset & 0x80)
    {
      /* A signed seven-bit count of quarter hours east of UTC.  */
      int quarters = utc_offset & 0x7f;

      if (quarters & 0x40)
	quarters -= 0x80;
      ts.tv_sec = timegm (&tm) - quarters * 15 * 60;
    }
  else
    /* Section 7.4.10.2: without a valid offset the time is in the
       local time zone of whoever reads it.  */
    ts.tv_sec = mktime (&tm);

  if (increment < 200)
    {
      ts.tv_sec += increment / 100;
      ts.tv_nsec = (increment % 100) * 10000000L;
    }
  return ts;
}

/* Validate the SET->count entries in SET->raw and fill in the rest of
   SET.  Return EIO if the set is not a usable file; SET->named then says
   whether its name could still be made out.  */
static error_t
parse_set (struct exfat_set *set)
{
  const uint8_t *file = set->raw;
  const uint8_t *stream = set->raw + EXFAT_ENTRY_SIZE;
  unsigned name_entries;
  uint64_t data_length, valid_length;

  set->named = false;
  set->error = EIO;

  if (set->count < 3 || stream[0] != EXFAT_TYPE_STREAM)
    return EIO;
  set->name_length = stream[3];
  name_entries = (set->name_length + 14) / 15;
  if (set->name_length == 0 || set->count < 2 + name_entries)
    return EIO;

  for (unsigned i = 0; i < set->name_length; i++)
    {
      const uint8_t *entry = set->raw + (2 + i / 15) * EXFAT_ENTRY_SIZE;
      uint16_t c;

      if (entry[0] != EXFAT_TYPE_NAME)
	return EIO;
      c = get_le16 (entry + 2 + (i % 15) * 2);
      /* The two characters no file name here can hold.  */
      if (c == 0 || c == '/')
	return EIO;
      set->name[i] = c;
    }
  if (set->name[0] == '.'
      && (set->name_length == 1
	  || (set->name_length == 2 && set->name[1] == '.')))
    return EIO;

  /* Whatever follows the names must be secondary entries that may be
     ignored; an unrecognized critical one makes the whole set
     unrecognized.  */
  for (unsigned i = 2 + name_entries; i < set->count; i++)
    {
      uint8_t type = set->raw[i * EXFAT_ENTRY_SIZE];

      if (!in_use_secondary (type) || !(type & EXFAT_TYPE_BENIGN))
	return EIO;
    }
  set->named = true;

  if (checksum16 (checksum16 (0, file, 2), file + 4,
		  set->count * EXFAT_ENTRY_SIZE - 4)
      != get_le16 (file + 2))
    return EIO;

  set->attributes = get_le16 (file + 4);
  set->contiguous = stream[1] & 2;
  set->name_hash = get_le16 (stream + 4);
  valid_length = get_le64 (stream + 8);
  set->first_cluster = get_le32 (stream + 20);
  data_length = get_le64 (stream + 24);

  if (!exfat_allocation_valid (set->first_cluster, set->contiguous,
			       data_length))
    return EIO;
  if (set->attributes & EXFAT_ATTR_DIRECTORY)
    {
      if (data_length % CLUSTER_SIZE != 0
	  || data_length > EXFAT_DIR_SIZE_MAX)
	return EIO;
      valid_length = data_length;
    }
  else if (valid_length > data_length)
    return EIO;
  set->data_length = data_length;
  set->valid_length = valid_length;

  set->create_time = decode_time (get_le32 (file + 8), file[20], file[22]);
  set->modify_time = decode_time (get_le32 (file + 12), file[21], file[23]);
  set->access_time = decode_time (get_le32 (file + 16), 0, file[24]);

  set->error = 0;
  return 0;
}

/* Read into SET the entry set whose File entry is at byte OFFSET of the
   volume, in a directory that is CONTIGUOUS or not.  This needs nothing
   else from the directory, so a node can be read back by its inode
   number alone.  */
error_t
exfat_set_read_at (off_t offset, bool contiguous, struct exfat_set *set)
{
  uint8_t sector[4096];
  off_t at = offset;

  if (offset < vol.heap_offset || offset % EXFAT_ENTRY_SIZE != 0
      || !cluster_valid (EXFAT_FIRST_CLUSTER
			 + ((offset - vol.heap_offset) >> vol.cluster_shift)))
    return EIO;

  set->offset = offset;
  set->count = 1;
  for (unsigned i = 0; i < set->count; i++)
    {
      error_t err;
      const uint8_t *entry = sector + (at & (SECTOR_SIZE - 1));

      err = exfat_disk_read (at & ~(off_t) (SECTOR_SIZE - 1), sector,
			     SECTOR_SIZE);
      if (err)
	return err;
      if (i == 0)
	{
	  if (entry[0] != EXFAT_TYPE_FILE)
	    return EIO;
	  set->count = 1 + entry[1];
	}
      else if (!in_use_secondary (entry[0]))
	return EIO;
      memcpy (set->raw + i * EXFAT_ENTRY_SIZE, entry, EXFAT_ENTRY_SIZE);

      at += EXFAT_ENTRY_SIZE;
      if (((at - vol.heap_offset) & (CLUSTER_SIZE - 1)) == 0
	  && i + 1 < set->count)
	{
	  cluster_t cluster = EXFAT_FIRST_CLUSTER
	    + ((at - EXFAT_ENTRY_SIZE - vol.heap_offset)
	       >> vol.cluster_shift);
	  cluster_t next;

	  if (contiguous)
	    next = cluster + 1;
	  else
	    {
	      err = exfat_fat_next (cluster, &next);
	      if (err)
		return err;
	    }
	  if (!cluster_valid (next))
	    return EIO;
	  at = cluster_offset (next);
	}
    }
  return parse_set (set);
}


/* Directories.  */

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

/* Read the next File entry set at or after DIR->pos into SET and move
   DIR->pos past it.  Set *FOUND to false at the end of the directory.
   A set that fails validation is returned if its name is known, with
   SET->error set, and skipped otherwise.  */
error_t
exfat_dir_next_set (struct exfat_dir *dir, struct exfat_set *set,
		    bool *found)
{
  for (;;)
    {
      error_t err;
      const uint8_t *entry;
      off_t run;
      unsigned n;

      err = exfat_dir_next_primary (dir, &entry);
      if (err)
	return err;
      if (!entry)
	{
	  *found = false;
	  return 0;
	}

      if (entry[0] != EXFAT_TYPE_FILE)
	{
	  dir->pos += EXFAT_ENTRY_SIZE;
	  if (entry[0] & EXFAT_TYPE_BENIGN)
	    continue;
	  if (dir->root && (entry[0] == EXFAT_TYPE_BITMAP
			    || entry[0] == EXFAT_TYPE_UPCASE
			    || entry[0] == EXFAT_TYPE_LABEL))
	    continue;
	  /* Section 8.2: any other critical primary entry makes the
	     directory invalid.  */
	  return EIO;
	}

      set->count = 1 + entry[1];
      memcpy (set->raw, entry, EXFAT_ENTRY_SIZE);
      err = exfat_stream_map (dir->stream, dir->pos, &set->offset, &run);
      if (err)
	return err;

      for (n = 1; n < set->count; n++)
	{
	  off_t pos = dir->pos + n * EXFAT_ENTRY_SIZE;

	  if (pos + EXFAT_ENTRY_SIZE > dir->stream->length)
	    break;
	  err = dir_entry (dir, pos, &entry);
	  if (err)
	    return err;
	  if (!in_use_secondary (entry[0]))
	    break;
	  memcpy (set->raw + n * EXFAT_ENTRY_SIZE, entry, EXFAT_ENTRY_SIZE);
	}
      /* A set cut short ends where the entry that cut it starts; that
	 entry may begin a good set.  */
      dir->pos += n * EXFAT_ENTRY_SIZE;
      if (n < set->count)
	continue;

      if (parse_set (set) && !set->named)
	continue;
      *found = true;
      return 0;
    }
}
