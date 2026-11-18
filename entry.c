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
#include <error.h>
#include <string.h>
#include <time.h>
#include <assert-backtrace.h>
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

	  key->name[key->length] = units[i];
	  key->upcased[key->length++] = up;
	  bytes[0] = up & 0xff;
	  bytes[1] = up >> 8;
	  key->hash = checksum16 (key->hash, bytes, 2);
	}
    }
  return key->length > 0;
}

/* Set *KEY to the name of SET, which was read and found to have
   one.  */
void
exfat_name_key_of_set (const struct exfat_set *set,
		       struct exfat_name_key *key)
{
  uint8_t bytes[2];

  key->length = set->name_length;
  key->hash = 0;
  for (unsigned i = 0; i < key->length; i++)
    {
      key->name[i] = set->name[i];
      key->upcased[i] = exfat_upcase[set->name[i]];
      bytes[0] = key->upcased[i] & 0xff;
      bytes[1] = key->upcased[i] >> 8;
      key->hash = checksum16 (key->hash, bytes, 2);
    }
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

/* Return whether SET's name is KEY's, case and all.  */
bool
exfat_name_exact (const struct exfat_set *set,
		  const struct exfat_name_key *key)
{
  return set->name_length == key->length
    && memcmp (set->name, key->name, key->length * sizeof *key->name) == 0;
}

/* Return whether a file may be given the name in KEY.  Table 35 keeps
   the control codes and these characters out of file names.  */
bool
exfat_name_creatable (const struct exfat_name_key *key)
{
  for (unsigned i = 0; i < key->length; i++)
    if (key->name[i] < 0x20
	|| (key->name[i] < 0x80 && strchr ("\"*/:<>?\\|", key->name[i])))
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

/* The stamp of the time TS, in UTC, and the count of 10 ms that goes
   with it.  */
static void
encode_time (struct timespec ts, uint32_t *stamp, uint8_t *increment)
{
  /* What the seven bits of the year can hold: 1980 to 2107.  */
  const time_t first = 315532800, last = 4354819199;
  time_t sec = ts.tv_sec < first ? first : ts.tv_sec > last ? last : ts.tv_sec;
  struct tm tm;

  gmtime_r (&sec, &tm);
  *stamp = (uint32_t) (tm.tm_year - 80) << 25 | (tm.tm_mon + 1) << 21
    | tm.tm_mday << 16 | tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2;
  *increment = (tm.tm_sec & 1) * 100 + ts.tv_nsec / 10000000;
}

/* Record the time TS at STAMP, with its count of 10 ms at INCREMENT if
   that is not null and its offset from UTC at UTC_OFFSET, unless what
   is there already stands for the same time: a time written in local
   time is another time to its writer once it is rewritten in UTC.  */
static void
put_time (struct timespec ts, uint8_t *stamp, uint8_t *increment,
	  uint8_t *utc_offset)
{
  uint32_t value;
  uint8_t tens;
  struct timespec old, new;

  encode_time (ts, &value, &tens);
  if (!increment)
    tens = 0;
  old = decode_time (get_le32 (stamp), increment ? *increment : 0,
		     *utc_offset);
  new = decode_time (value, tens, 0x80);
  if (old.tv_sec == new.tv_sec && old.tv_nsec == new.tv_nsec)
    return;

  put_le32 (stamp, value);
  if (increment)
    *increment = tens;
  /* Section 7.4.10: the offset is valid, and zero.  */
  *utc_offset = 0x80;
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
   else from the directory, so a node can be read back from what
   exfat_inode_location says of it.  */
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

/* Set AT[0] to AT[COUNT - 1] to the volume offsets of the COUNT
   entries that start at OFFSET, in a directory that is CONTIGUOUS or
   not.  */
static error_t
entry_positions (off_t offset, bool contiguous, unsigned count, off_t *at)
{
  if (offset < vol.heap_offset || offset % EXFAT_ENTRY_SIZE != 0
      || !cluster_valid (EXFAT_FIRST_CLUSTER
			 + ((offset - vol.heap_offset) >> vol.cluster_shift)))
    return EIO;

  for (unsigned i = 0; i < count; i++)
    {
      at[i] = offset;
      offset += EXFAT_ENTRY_SIZE;
      if (((offset - vol.heap_offset) & (CLUSTER_SIZE - 1)) == 0
	  && i + 1 < count)
	{
	  cluster_t cluster = EXFAT_FIRST_CLUSTER
	    + ((at[i] - vol.heap_offset) >> vol.cluster_shift);
	  cluster_t next;

	  if (contiguous)
	    next = cluster + 1;
	  else
	    {
	      error_t err = exfat_fat_next (cluster, &next);

	      if (err)
		return err;
	    }
	  if (!cluster_valid (next))
	    return EIO;
	  offset = cluster_offset (next);
	}
    }
  return 0;
}

/* Read or write the COUNT entries in RAW at the volume offsets in AT.
   Entries that follow one another on the disk go in one request, and
   a piece that already holds what is to be written is not written.
   With LAST_FIRST, the pieces are written from the last to the
   first.  Only the store can make this fail.  */
static error_t
transfer_entries (const off_t *at, uint8_t *raw, unsigned count, bool write,
		  bool last_first)
{
  error_t err = 0;
  uint8_t buf[EXFAT_SET_MAX * EXFAT_ENTRY_SIZE + 2 * 4096];
  unsigned done = 0;

  while (!err && done < count)
    {
      unsigned first, last;
      off_t start, end;
      uint8_t *data;
      size_t size;

      if (last_first)
	{
	  last = count - 1 - done;
	  for (first = last;
	       first > 0 && at[first - 1] + EXFAT_ENTRY_SIZE == at[first];
	       first--)
	    ;
	}
      else
	{
	  first = done;
	  for (last = first;
	       last + 1 < count && at[last] + EXFAT_ENTRY_SIZE == at[last + 1];
	       last++)
	    ;
	}

      start = at[first] & ~(off_t) (SECTOR_SIZE - 1);
      end = (at[last] + EXFAT_ENTRY_SIZE + SECTOR_SIZE - 1)
	& ~(off_t) (SECTOR_SIZE - 1);
      data = buf + (at[first] - start);
      size = (last - first + 1) * EXFAT_ENTRY_SIZE;

      err = exfat_disk_read (start, buf, end - start);
      if (!err && !write)
	memcpy (raw + first * EXFAT_ENTRY_SIZE, data, size);
      else if (!err && memcmp (data, raw + first * EXFAT_ENTRY_SIZE, size))
	{
	  memcpy (data, raw + first * EXFAT_ENTRY_SIZE, size);
	  err = exfat_disk_write (start, buf, end - start);
	}
      done += last - first + 1;
    }
  return err;
}

/* Write the entries of SET where exfat_set_read_at would read them
   from.

   A set that lies in one run of sectors is written with one request.
   One that a cluster boundary splits takes two, and has a wrong
   checksum if the translator dies between them, when the change is in
   both pieces; there is nothing to be done about that without a
   journal.  */
error_t
exfat_set_write_at (off_t offset, bool contiguous,
		    const struct exfat_set *set)
{
  off_t at[EXFAT_SET_MAX];
  error_t err = entry_positions (offset, contiguous, set->count, at);

  if (err)
    return err;
  return transfer_entries (at, (uint8_t *) set->raw, set->count, true, false);
}

/* Make CHANGE one to the COUNT entries that start at OFFSET, in a
   directory that is CONTIGUOUS or not, which so far leaves them as
   they are.  */
error_t
exfat_set_change_at (struct exfat_set_change *change, off_t offset,
		     bool contiguous, unsigned count)
{
  error_t err = entry_positions (offset, contiguous, count, change->at);

  if (!err)
    err = transfer_entries (change->at, change->before, count, false, false);
  if (!err)
    {
      change->count = count;
      memcpy (change->after, change->before, count * EXFAT_ENTRY_SIZE);
    }
  return err;
}

/* Make CHANGE one to the first COUNT of the entries that PREVIOUS
   changes, to be made after it.  */
void
exfat_set_change_follow (struct exfat_set_change *change,
			 const struct exfat_set_change *previous,
			 unsigned count)
{
  assert_backtrace (count <= previous->count);
  change->count = count;
  memcpy (change->at, previous->at, count * sizeof *change->at);
  memcpy (change->before, previous->after, count * EXFAT_ENTRY_SIZE);
  memcpy (change->after, previous->after, count * EXFAT_ENTRY_SIZE);
}

/* Have CHANGE leave its entries holding SET, or holding no set if SET
   is null.  Entries that SET has no use for are left unused.  */
void
exfat_set_change_put (struct exfat_set_change *change,
		      const struct exfat_set *set)
{
  unsigned used = set ? set->count : 0;

  assert_backtrace (used <= change->count);
  if (set)
    memcpy (change->after, set->raw, used * EXFAT_ENTRY_SIZE);
  for (unsigned i = used; i < change->count; i++)
    change->after[i * EXFAT_ENTRY_SIZE] &= ~EXFAT_TYPE_IN_USE;
}

/* Return whether one request to the store makes CHANGE: whether its
   entries follow one another on the disk.  */
bool
exfat_set_change_in_one_piece (const struct exfat_set_change *change)
{
  for (unsigned i = 1; i < change->count; i++)
    if (change->at[i] != change->at[i - 1] + EXFAT_ENTRY_SIZE)
      return false;
  return true;
}

/* Return whether none of the entries of CHANGE is in use before
   it.  */
bool
exfat_set_change_into_unused (const struct exfat_set_change *change)
{
  for (unsigned i = 0; i < change->count; i++)
    if (change->before[i * EXFAT_ENTRY_SIZE] & EXFAT_TYPE_IN_USE)
      return false;
  return true;
}

/* Write what CHANGE leaves in its entries, or with UNDO what they
   held before it.  The File entry is what makes the others a set, so
   the piece that holds it is written last if this puts it in use, and
   first otherwise: where one request does not do, the entries are a
   whole set or secondary entries that no set claims, and never a set
   with a part missing.  */
static error_t
change_write (const struct exfat_set_change *change, bool undo)
{
  const uint8_t *from = undo ? change->after : change->before;
  const uint8_t *to = undo ? change->before : change->after;
  bool creating = !(from[0] & EXFAT_TYPE_IN_USE);
  unsigned last_piece = change->count - 1;
  bool at_end = false;

  while (last_piece > 0
	 && change->at[last_piece - 1] + EXFAT_ENTRY_SIZE
	 == change->at[last_piece])
    last_piece--;
  for (unsigned i = 0; i < last_piece; i++)
    if (from[i * EXFAT_ENTRY_SIZE] == 0)
      at_end = true;

  if (creating && at_end)
    {
      /* Section 6.2.1: only entries of type 00h follow an entry of
	 that type, and the last piece would follow those in the
	 pieces before it.  So these first get the entries they are to
	 hold, marked as unused.  */
      uint8_t unused[EXFAT_SET_MAX * EXFAT_ENTRY_SIZE];
      error_t err;

      memcpy (unused, to, last_piece * EXFAT_ENTRY_SIZE);
      for (unsigned i = 0; i < last_piece; i++)
	unused[i * EXFAT_ENTRY_SIZE] &= ~EXFAT_TYPE_IN_USE;
      err = transfer_entries (change->at, unused, last_piece, true, false);
      if (err)
	return err;
    }
  return transfer_entries (change->at, (uint8_t *) to, change->count, true,
			   creating);
}

/* Make the COUNT changes in CHANGES, in that order.  They were all
   prepared, so only the store can make this fail.  If it does, what
   was written is taken back and the volume is noted as damaged.  If
   the store does not allow that either, the entries are left as
   neither the caller nor a reader of the volume expects them, with a
   file under two names or a name without its file, and nothing more
   is written to the volume: freeing the clusters of a file that has
   two entry sets would give those of the other away.  */
error_t
exfat_set_changes_apply (struct exfat_set_change *const *changes,
			 unsigned count)
{
  error_t err = 0;
  bool undone = true;
  unsigned done = 0;

  while (!err && done < count)
    err = change_write (changes[done++], false);
  if (!err)
    return 0;

  /* The one that failed may be partly made.  */
  while (done-- > 0)
    if (change_write (changes[done], true))
      undone = false;
  if (undone)
    exfat_volume_damaged ();
  else
    exfat_volume_failed ();
  return err;
}

/* Section 8.2 has the allocations of the unrecognized benign
   secondary entries of a set freed when the set is deleted, and those
   of the unrecognized benign primary entries in a directory when the
   directory is deleted.  The Linux driver frees them.  Here they are
   left allocated instead.  Only the entry says that the clusters are
   its own, and on a damaged or crafted volume that fsck.exfat finds
   nothing wrong with they are another file's, or the allocation
   bitmap's: freeing them hands that file's clusters to the next one
   written.  Telling the two apart takes a walk of every directory on
   the volume and every node that also keeps out what is allocated
   while it goes on; the walk this translator had did not, and a file
   that grew meanwhile lost its end.  Leaving them loses nothing.
   Marking the volume dirty would not get them back: fsck.exfat neither
   reports nor frees clusters that no entry names.  Such entries are
   rare, and the first time this happens it says so.  */
static void
left_allocated (uint64_t clusters)
{
  static bool told;

  if (clusters && !__atomic_exchange_n (&told, true, __ATOMIC_RELAXED))
    error (0, 0, "%s: did not free what a removed directory entry of a kind"
	   " not known here named (%llu clusters), since it may be another"
	   " file's", diskfs_disk_name, (unsigned long long) clusters);
}

/* Return how many clusters the entries of the COUNT entries in RAW, a
   set that is removed, name that section 8.2 has freed with it: those
   of benign entries other than the Stream Extension and File Name
   entries of a file, and other than Vendor Extension entries, whose
   bytes past the flags are the vendor's (section 7.8).  */
static uint64_t
unknown_clusters (const uint8_t *raw, unsigned count)
{
  bool file = raw[0] == EXFAT_TYPE_FILE;
  uint64_t clusters = 0;

  for (unsigned i = file ? 1 : 0; i < count; i++)
    {
      const uint8_t *entry = raw + i * EXFAT_ENTRY_SIZE;
      unsigned flags = i == 0 ? get_le16 (entry + 4) : entry[1];
      cluster_t first = get_le32 (entry + 20);
      uint64_t length = get_le64 (entry + 24);

      if ((file && (entry[0] == EXFAT_TYPE_STREAM
		    || entry[0] == EXFAT_TYPE_NAME))
	  || !(entry[0] & EXFAT_TYPE_BENIGN)
	  || entry[0] == EXFAT_TYPE_VENDOR_EXTENSION)
	continue;
      /* AllocationPossible, and NoFatChain.  */
      if ((flags & 1) && first != 0 && length != 0
	  && exfat_allocation_valid (first, flags & 2, length))
	clusters += round_cluster (length) >> vol.cluster_shift;
    }
  return clusters;
}

/* Leave allocated what the COUNT entries in RAW, a set that was
   deleted, name that section 8.2 has freed with it.  */
void
exfat_set_keep_unknown (const uint8_t *raw, unsigned count)
{
  left_allocated (unknown_clusters (raw, count));
}

/* Mark the entries of the set whose File entry is at OFFSET as no
   longer in use, and put the entries it had, as they were, in RAW, and
   how many there are in *COUNT.  The File entry goes first: without it
   the rest are again secondary entries that no set claims.  */
error_t
exfat_set_delete_at (off_t offset, bool contiguous, uint8_t *raw,
		     unsigned *count_out)
{
  error_t err;
  off_t at[EXFAT_SET_MAX];
  unsigned count = 1, n;

  err = entry_positions (offset, contiguous, 1, at);
  if (!err)
    err = transfer_entries (at, raw, 1, false, false);
  if (!err && raw[0] != EXFAT_TYPE_FILE)
    err = EIO;
  if (!err)
    {
      count = 1 + raw[1];
      err = entry_positions (offset, contiguous, count, at);
    }
  if (!err)
    err = transfer_entries (at, raw, count, false, false);
  if (!err)
    {
      for (n = 1; n < count; n++)
	if (!in_use_secondary (raw[n * EXFAT_ENTRY_SIZE]))
	  break;
      for (unsigned i = 0; i < n; i++)
	raw[i * EXFAT_ENTRY_SIZE] &= ~EXFAT_TYPE_IN_USE;
      err = transfer_entries (at, raw, n, true, false);
    }
  if (!err)
    {
      for (unsigned i = 0; i < n; i++)
	raw[i * EXFAT_ENTRY_SIZE] |= EXFAT_TYPE_IN_USE;
      *count_out = n;
    }
  return err;
}

/* Return whether META says something that SET, which was read and
   found good, does not, the time of last access aside.  The time of
   last modification is compared as the volume records it.  */
bool
exfat_set_differs (const struct exfat_set *set, const struct exfat_meta *meta)
{
  uint32_t stamp, meta_stamp;
  uint8_t increment, meta_increment;

  encode_time (set->modify_time, &stamp, &increment);
  encode_time (meta->modify_time, &meta_stamp, &meta_increment);
  return set->attributes != meta->attributes
    || set->first_cluster != meta->first_cluster
    || set->contiguous != meta->contiguous
    || set->data_length != meta->data_length
    || set->valid_length != meta->valid_length
    || stamp != meta_stamp || increment != meta_increment;
}

static void
seal_set (struct exfat_set *set)
{
  uint8_t *file = set->raw;

  file[1] = set->count - 1;
  put_le16 (file + 2, checksum16 (checksum16 (0, file, 2), file + 4,
				  set->count * EXFAT_ENTRY_SIZE - 4));
}

/* Make SET, which was read and found good, say what META says.  The
   time of creation is left as it is.  */
void
exfat_set_update (struct exfat_set *set, const struct exfat_meta *meta)
{
  uint8_t *file = set->raw;
  uint8_t *stream = set->raw + EXFAT_ENTRY_SIZE;

  put_le16 (file + 4, meta->attributes);
  put_time (meta->modify_time, file + 12, file + 21, file + 23);
  put_time (meta->access_time, file + 16, NULL, file + 24);

  /* AllocationPossible, and NoFatChain.  */
  stream[1] = (stream[1] & ~3U) | 1 | (meta->contiguous ? 2 : 0);
  put_le64 (stream + 8, meta->valid_length);
  put_le32 (stream + 20, meta->first_cluster);
  put_le64 (stream + 24, meta->data_length);
  seal_set (set);
}

/* Give SET, which was read and found good, the name in NAME.  The
   entries that follow those of its old name are kept.  */
error_t
exfat_set_rename (struct exfat_set *set, const struct exfat_name_key *name)
{
  uint8_t *stream = set->raw + EXFAT_ENTRY_SIZE;
  unsigned old_entries = (set->name_length + 14) / 15;
  unsigned new_entries = (name->length + 14) / 15;
  unsigned others = set->count - 2 - old_entries;

  if (2 + new_entries + others > EXFAT_SET_MAX)
    return ENAMETOOLONG;
  memmove (set->raw + (2 + new_entries) * EXFAT_ENTRY_SIZE,
	   set->raw + (2 + old_entries) * EXFAT_ENTRY_SIZE,
	   others * EXFAT_ENTRY_SIZE);
  memset (set->raw + 2 * EXFAT_ENTRY_SIZE, 0, new_entries * EXFAT_ENTRY_SIZE);
  for (unsigned i = 0; i < new_entries; i++)
    set->raw[(2 + i) * EXFAT_ENTRY_SIZE] = EXFAT_TYPE_NAME;
  for (unsigned i = 0; i < name->length; i++)
    put_le16 (set->raw + (2 + i / 15) * EXFAT_ENTRY_SIZE + 2 + (i % 15) * 2,
	      name->name[i]);

  stream[3] = name->length;
  put_le16 (stream + 4, name->hash);
  set->count = 2 + new_entries + others;
  set->name_length = name->length;
  memcpy (set->name, name->name, name->length * sizeof *name->name);
  set->name_hash = name->hash;
  seal_set (set);
  return 0;
}

/* Make SET the entry set of a new file called NAME, which META
   describes.  */
void
exfat_set_create (struct exfat_set *set, const struct exfat_name_key *name,
		  const struct exfat_meta *meta)
{
  uint8_t *file = set->raw;
  uint8_t *stream = set->raw + EXFAT_ENTRY_SIZE;
  uint32_t stamp;

  memset (set->raw, 0, 2 * EXFAT_ENTRY_SIZE);
  file[0] = EXFAT_TYPE_FILE;
  stream[0] = EXFAT_TYPE_STREAM;
  encode_time (meta->create_time, &stamp, &file[20]);
  put_le32 (file + 8, stamp);
  file[22] = 0x80;
  set->count = 2;
  set->name_length = 0;
  exfat_set_rename (set, name);
  exfat_set_update (set, meta);
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

/* Leave allocated what the sets in DIR that have a benign primary
   entry name, for a directory that is deleted.  */
void
exfat_dir_keep_benign (struct exfat_dir *dir)
{
  uint8_t *raw = malloc (EXFAT_SET_MAX * EXFAT_ENTRY_SIZE);
  uint64_t clusters = 0;
  const uint8_t *entry;

  if (!raw)
    return;
  while (!exfat_dir_next_primary (dir, &entry) && entry)
    {
      unsigned count = 1 + entry[1], n;

      if (!(entry[0] & EXFAT_TYPE_BENIGN))
	{
	  dir->pos += EXFAT_ENTRY_SIZE;
	  continue;
	}
      memcpy (raw, entry, EXFAT_ENTRY_SIZE);
      for (n = 1; n < count; n++)
	{
	  off_t pos = dir->pos + n * EXFAT_ENTRY_SIZE;

	  if (pos + EXFAT_ENTRY_SIZE > dir->stream->length
	      || dir_entry (dir, pos, &entry) || !in_use_secondary (entry[0]))
	    break;
	  memcpy (raw + n * EXFAT_ENTRY_SIZE, entry, EXFAT_ENTRY_SIZE);
	}
      if (n == count)
	clusters += unknown_clusters (raw, count);
      dir->pos += n * EXFAT_ENTRY_SIZE;
    }
  free (raw);
  left_allocated (clusters);
}

/* Find COUNT unused entries that follow one another in DIR, and set
   *POS to the offset in the directory of the first.  Return ENOSPC if
   the directory has to grow to hold them.  Return EIO, and set
   *HIDDEN, if they would be in the entries of type 00h at the end of
   DIR but an entry after those is in use.  */
error_t
exfat_dir_find_free (struct exfat_dir *dir, unsigned count, off_t *pos,
		     bool *hidden)
{
  unsigned run = 0;

  for (off_t at = 0; at + EXFAT_ENTRY_SIZE <= dir->stream->length;
       at += EXFAT_ENTRY_SIZE)
    {
      const uint8_t *e;
      error_t err = dir_entry (dir, at, &e);

      if (err)
	return err;
      if (e[0] == 0)
	{
	  /* Section 6.2.1: every entry from here on is unused as well.  A
	     set written here moves that end past its last entry, and
	     what follows would be in the directory again.  */
	  off_t start = at - run * EXFAT_ENTRY_SIZE;

	  for (off_t rest = at; rest + EXFAT_ENTRY_SIZE <= dir->stream->length;
	       rest += EXFAT_ENTRY_SIZE)
	    {
	      err = dir_entry (dir, rest, &e);
	      if (err)
		return err;
	      if (e[0] != 0)
		{
		  *hidden = true;
		  return EIO;
		}
	    }
	  if ((dir->stream->length - start) / EXFAT_ENTRY_SIZE < count)
	    return ENOSPC;
	  *pos = start;
	  return 0;
	}
      if (e[0] & EXFAT_TYPE_IN_USE)
	run = 0;
      else if (++run == count)
	{
	  *pos = at - (count - 1) * EXFAT_ENTRY_SIZE;
	  return 0;
	}
    }
  return ENOSPC;
}
