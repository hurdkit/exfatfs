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
  /* The key in BY_SET_OFFSET, or zero for a file that has no entry
     set and is not in that table.  */
  uint64_t set_offset;
  ino_t parent;
  bool parent_contiguous;
  /* For a directory that has clusters, the first of them, which is the
     key in BY_DIR_CLUSTER; zero otherwise.  */
  cluster_t dir_cluster;
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
static struct hurd_ihash by_dir_cluster
  = HURD_IHASH_INITIALIZER (HURD_IHASH_NO_LOCP);
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

/* Enter VI in the tables.  */
static error_t
insert (struct vinode *vi)
{
  error_t err = hurd_ihash_add (&by_ino, (hurd_ihash_key_t) &vi->ino, vi);

  if (err)
    return err;
  if (vi->set_offset)
    err = hurd_ihash_add (&by_set_offset, (hurd_ihash_key_t) &vi->set_offset,
			  vi);
  if (!err && vi->dir_cluster)
    {
      err = hurd_ihash_add (&by_dir_cluster, vi->dir_cluster, vi);
      if (err && vi->set_offset)
	hurd_ihash_remove (&by_set_offset,
			   (hurd_ihash_key_t) &vi->set_offset);
    }
  if (err)
    hurd_ihash_remove (&by_ino, (hurd_ihash_key_t) &vi->ino);
  return err;
}

/* Set *INO to the inode number of the file whose entry set is at
   WHERE, giving it one if it has none yet.  A file that already has a
   number keeps the directory recorded for it, whatever WHERE says.
   DIR_CLUSTER is the first cluster of the file if it is a directory
   that has clusters, and zero otherwise.  Return EIO for a directory
   whose clusters another directory has.  */
error_t
exfat_inode_get (const struct exfat_location *where, cluster_t dir_cluster,
		 ino_t *ino)
{
  error_t err = 0;
  struct vinode *vi, *owner = NULL;

  pthread_mutex_lock (&lock);
  vi = find_set_offset (where->set_offset);
  if (dir_cluster)
    owner = hurd_ihash_find (&by_dir_cluster, dir_cluster);

  /* Two directory entries with the same first cluster are one
     directory under two names, each with a node and a lock of its own.
     Its entries then have two parents, so one lookup can hold the
     first while it waits for a node that another lookup, holding the
     second, found below it: lookups of `b' in `a' and of `a' in `b'
     deadlock when `b' has the first cluster of the directory that
     holds `a'.  So the first entry seen with a first cluster keeps
     it, and any other is refused.  The root always has its own, and
     every ancestor of a directory was seen before it.

     Chains that start apart and join further on are not noticed:
     that would take walking the whole chain of every directory
     against all the others.  The lookup refuses, with
     exfat_inode_is_ancestor, the one of those that needs no more than
     two lookups to deadlock.  */
  if (dir_cluster == vol.root_cluster || (owner && owner != vi))
    err = EIO;
  else if (vi && vi->dir_cluster != dir_cluster)
    /* The entry set there is no longer the one the number was given
       to.  */
    err = EIO;
  else if (!vi)
    {
      vi = malloc (sizeof *vi);
      if (!vi)
	err = ENOMEM;
      else
	{
	  *vi = (struct vinode) { next_ino, where->set_offset, where->parent,
				  where->parent_contiguous, dir_cluster };
	  err = insert (vi);
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

/* Set *INO to a number for a new file in the directory numbered
   PARENT.  It has no entry set until exfat_inode_move says where one
   is.  */
error_t
exfat_inode_new (ino_t parent, ino_t *ino)
{
  error_t err;
  struct vinode *vi = malloc (sizeof *vi);

  if (!vi)
    return ENOMEM;
  pthread_mutex_lock (&lock);
  *vi = (struct vinode) { next_ino, 0, parent, false, 0 };
  err = insert (vi);
  if (err)
    free (vi);
  else
    *ino = next_ino++;
  pthread_mutex_unlock (&lock);
  return err;
}

/* Set *WHERE to the place of the entry set of the file numbered INO,
   which has none if WHERE->set_offset comes back zero, and to the
   directory it is or was in.  Return false if nothing is known of
   INO: for the root, and for a file that exfat_inode_forget was told
   about.  */
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

/* Return whether the file numbered INO is the directory numbered OF or
   one of those above it.  */
bool
exfat_inode_is_ancestor (ino_t ino, ino_t of)
{
  struct vinode *vi;

  pthread_mutex_lock (&lock);
  while (of != ino && (vi = find_ino (of)))
    of = vi->parent;
  pthread_mutex_unlock (&lock);
  return of == ino;
}

/* Record that the entry set of the file numbered INO is now at WHERE.
   The caller wrote it there, where no entry was in use, so a file
   still recorded at that place lost its entry set without this
   translator doing it, and is recorded as having none.  The caller
   keeps a directory from moving into itself or below itself.  */
void
exfat_inode_move (ino_t ino, const struct exfat_location *where)
{
  error_t err;
  struct vinode *vi, *stale;

  pthread_mutex_lock (&lock);
  vi = find_ino (ino);
  assert_backtrace (vi);
  if ((off_t) vi->set_offset != where->set_offset)
    {
      stale = find_set_offset (where->set_offset);
      if (stale)
	{
	  hurd_ihash_remove (&by_set_offset,
			     (hurd_ihash_key_t) &stale->set_offset);
	  stale->set_offset = 0;
	}
      if (vi->set_offset)
	hurd_ihash_remove (&by_set_offset,
			   (hurd_ihash_key_t) &vi->set_offset);
      vi->set_offset = where->set_offset;
      err = hurd_ihash_add (&by_set_offset,
			    (hurd_ihash_key_t) &vi->set_offset, vi);
      /* libdiskfs's node cache does not survive this either.  */
      assert_perror_backtrace (err);
    }
  vi->parent = where->parent;
  vi->parent_contiguous = where->parent_contiguous;
  pthread_mutex_unlock (&lock);
}

/* Record that the file numbered INO no longer has an entry set.  It
   keeps its number, and the directory it was in, for as long as it
   stays open.  */
void
exfat_inode_detach (ino_t ino)
{
  struct vinode *vi;

  pthread_mutex_lock (&lock);
  vi = find_ino (ino);
  if (vi && vi->set_offset)
    {
      hurd_ihash_remove (&by_set_offset, (hurd_ihash_key_t) &vi->set_offset);
      vi->set_offset = 0;
    }
  pthread_mutex_unlock (&lock);
}

/* Record that DIR_CLUSTER is now the first cluster of the directory
   numbered INO, or that it has no clusters if DIR_CLUSTER is zero.
   Return EIO if another directory has that cluster.  */
error_t
exfat_inode_set_dir_cluster (ino_t ino, cluster_t dir_cluster)
{
  error_t err = 0;
  struct vinode *vi;

  pthread_mutex_lock (&lock);
  vi = find_ino (ino);
  assert_backtrace (vi);
  if (vi->dir_cluster != dir_cluster)
    {
      if (dir_cluster == vol.root_cluster
	  || (dir_cluster && hurd_ihash_find (&by_dir_cluster, dir_cluster)))
	err = EIO;
      else if (dir_cluster)
	err = hurd_ihash_add (&by_dir_cluster, dir_cluster, vi);
      if (!err)
	{
	  if (vi->dir_cluster)
	    hurd_ihash_remove (&by_dir_cluster, vi->dir_cluster);
	  vi->dir_cluster = dir_cluster;
	}
    }
  pthread_mutex_unlock (&lock);
  return err;
}

/* Forget the file numbered INO, which is gone.  The number is not
   given to another file.  */
void
exfat_inode_forget (ino_t ino)
{
  struct vinode *vi;

  pthread_mutex_lock (&lock);
  vi = find_ino (ino);
  if (vi)
    {
      hurd_ihash_remove (&by_ino, (hurd_ihash_key_t) &vi->ino);
      if (vi->set_offset)
	hurd_ihash_remove (&by_set_offset,
			   (hurd_ihash_key_t) &vi->set_offset);
      if (vi->dir_cluster)
	hurd_ihash_remove (&by_dir_cluster, vi->dir_cluster);
      free (vi);
    }
  pthread_mutex_unlock (&lock);
}
