/* vinode.c - Inode numbers and where their files are on the volume.

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
#include <assert-backtrace.h>
#include <hurd/ihash.h>
#include "exfatfs.h"

/* An exFAT volume has no inode numbers, and the one thing that names a
   file, the place of its entry set, changes when the file is renamed.
   So each file gets a number here the first time a directory shows it,
   and keeps it for as long as the translator runs, while the place
   recorded for that number can change.  A node is read from the volume
   by its number alone, whether a lookup just found it or libdiskfs asks
   for it again later.

   The first directory to show an entry set keeps it, and a file is
   recorded here before it can be searched itself, so following the
   parents from any file ends at the root.  */
struct vinode
{
  uint64_t ino;			/* The key in BY_INO.  */
  uint64_t set_offset;		/* The key in BY_SET_OFFSET.  */
  ino_t parent;
  bool parent_contiguous;
};

static hurd_ihash_key_t
hash_key (const void *key)
{
  uint64_t i = *(const uint64_t *) key;

  /* The mix function of fasthash, as in libdiskfs's node cache.  */
  i ^= i >> 23;
  i *= 0x2127599bf4325c37ULL;
  i ^= i >> 47;
  return (hurd_ihash_key_t) i;
}

static int
compare_key (const void *a, const void *b)
{
  return *(const uint64_t *) a == *(const uint64_t *) b;
}

static struct hurd_ihash by_ino
  = HURD_IHASH_INITIALIZER_GKI (HURD_IHASH_NO_LOCP, NULL, NULL,
				hash_key, compare_key);
static struct hurd_ihash by_set_offset
  = HURD_IHASH_INITIALIZER_GKI (HURD_IHASH_NO_LOCP, NULL, NULL,
				hash_key, compare_key);
static uint64_t next_ino = EXFAT_ROOT_INO + 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct vinode *
find_ino (ino_t ino)
{
  uint64_t key = ino;

  return hurd_ihash_find (&by_ino, (hurd_ihash_key_t) &key);
}

static struct vinode *
find_set_offset (off_t set_offset)
{
  uint64_t key = set_offset;

  return hurd_ihash_find (&by_set_offset, (hurd_ihash_key_t) &key);
}

/* Set *INO to the inode number of the file whose entry set is at
   WHERE, giving it one if it has none yet.  A file that already has a
   number keeps the directory recorded for it, whatever WHERE says.  */
error_t
exfat_inode_get (const struct exfat_location *where, ino_t *ino)
{
  error_t err = 0;
  struct vinode *vi;

  pthread_mutex_lock (&lock);
  vi = find_set_offset (where->set_offset);
  if (!vi)
    {
      vi = malloc (sizeof *vi);
      if (!vi)
	err = ENOMEM;
      else
	{
	  *vi = (struct vinode) { next_ino, where->set_offset, where->parent,
				  where->parent_contiguous };
	  err = hurd_ihash_add (&by_ino, (hurd_ihash_key_t) &vi->ino, vi);
	  if (!err)
	    {
	      err = hurd_ihash_add (&by_set_offset,
				    (hurd_ihash_key_t) &vi->set_offset, vi);
	      if (err)
		hurd_ihash_remove (&by_ino, (hurd_ihash_key_t) &vi->ino);
	    }
	  if (err)
	    free (vi);
	  else
	    next_ino++;
	}
    }
  if (!err)
    *ino = vi->ino;
  pthread_mutex_unlock (&lock);
  return err;
}

/* Set *WHERE to the place of the entry set of the file numbered INO.
   Return false if there is none: for the root, and for a file that
   exfat_inode_forget was told about.  */
bool
exfat_inode_location (ino_t ino, struct exfat_location *where)
{
  struct vinode *vi;

  pthread_mutex_lock (&lock);
  vi = find_ino (ino);
  if (vi)
    *where = (struct exfat_location) { vi->set_offset, vi->parent,
				       vi->parent_contiguous };
  pthread_mutex_unlock (&lock);
  return vi != NULL;
}

/* Record that the entry set of the file numbered INO is now at WHERE,
   where no other file's is.  The caller keeps a directory from moving
   into itself or below itself.  */
void
exfat_inode_move (ino_t ino, const struct exfat_location *where)
{
  error_t err;
  struct vinode *vi;

  pthread_mutex_lock (&lock);
  vi = find_ino (ino);
  assert_backtrace (vi);
  if ((off_t) vi->set_offset != where->set_offset)
    {
      assert_backtrace (!find_set_offset (where->set_offset));
      hurd_ihash_remove (&by_set_offset, (hurd_ihash_key_t) &vi->set_offset);
      vi->set_offset = where->set_offset;
      err = hurd_ihash_add (&by_set_offset,
			    (hurd_ihash_key_t) &vi->set_offset, vi);
      /* As for libdiskfs's node cache.  */
      assert_perror_backtrace (err);
    }
  vi->parent = where->parent;
  vi->parent_contiguous = where->parent_contiguous;
  pthread_mutex_unlock (&lock);
}

/* Forget where the entry set of the file numbered INO was, because it
   is no longer there.  The number is not given to another file.  */
void
exfat_inode_forget (ino_t ino)
{
  struct vinode *vi;

  pthread_mutex_lock (&lock);
  vi = find_ino (ino);
  if (vi)
    {
      hurd_ihash_remove (&by_ino, (hurd_ihash_key_t) &vi->ino);
      hurd_ihash_remove (&by_set_offset, (hurd_ihash_key_t) &vi->set_offset);
      free (vi);
    }
  pthread_mutex_unlock (&lock);
}
