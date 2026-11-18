/* dir.c - Directory lookup and listing.

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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "exfatfs.h"

/* Nothing is carried from a lookup to a later directory change while
   the file system cannot be written.  */
struct dirstat
{
  int unused;
};

const size_t diskfs_dirstat_size = sizeof (struct dirstat);

void
diskfs_null_dirstat (struct dirstat *ds)
{
}

error_t
diskfs_drop_dirstat (struct node *dp, struct dirstat *ds)
{
  return 0;
}

/* Implement the diskfs_lookup_hard callback as described in
   <hurd/diskfs.h>.  */
error_t
diskfs_lookup_hard (struct node *dp, const char *name, lookup_flags_t l_flags,
		    struct node **npp, struct dirstat *ds,
		    struct protid *cred)
{
  error_t err;
  bool spec_dotdot = l_flags & SPEC_DOTDOT;
  enum lookup_type type = l_flags & ~SPEC_DOTDOT;
  struct disknode *dn = dp->dn;
  struct node *np = NULL;
  struct exfat_name_key key;
  struct exfat_dir dir;
  struct exfat_set *set;
  bool found = false;

  if (npp)
    *npp = NULL;

  if (strcmp (name, ".") == 0)
    {
      if (npp)
	{
	  diskfs_nref (dp);
	  *npp = dp;
	}
      return 0;
    }

  if (strcmp (name, "..") == 0)
    {
      if (dp == diskfs_root_node)
	return EAGAIN;
      if (!npp)
	return 0;

      if (!spec_dotdot)
	{
	  /* A directory is locked before the nodes in it.  */
	  pthread_mutex_unlock (&dp->lock);
	  err = diskfs_cached_lookup (dn->parent, &np);
	  pthread_mutex_lock (&dp->lock);
	}
      else if (type == LOOKUP)
	{
	  ino_t parent = dn->parent;

	  diskfs_nput (dp);
	  err = diskfs_cached_lookup (parent, &np);
	}
      else
	{
	  np = diskfs_cached_ifind (dn->parent);
	  err = 0;
	}
      if (!err)
	*npp = np;
      return err;
    }

  if (!exfat_name_key (name, &key))
    return ENOENT;

  set = malloc (sizeof *set);
  if (!set)
    return ENOMEM;
  err = exfat_dir_open (&dir, &dn->stream, dp == diskfs_root_node);
  if (err)
    {
      free (set);
      return err;
    }
  while (!(err = exfat_dir_next_set (&dir, set, &found)) && found)
    if (exfat_name_match (set, &key))
      break;
  exfat_dir_close (&dir);

  if (!err && !found)
    err = ENOENT;
  if (!err)
    err = set->error;
  if (!err && npp)
    {
      ino_t inum = set->offset / EXFAT_ENTRY_SIZE;

      if (inum == dp->cache_id)
	/* Only a directory whose clusters hold its own entry can list
	   itself.  */
	err = EIO;
      else
	err = exfat_record_parent (inum, dp->cache_id,
				   dn->stream.contiguous);
      if (!err)
	err = diskfs_cached_lookup (inum, &np);
      if (!err)
	*npp = np;
    }

  free (set);
  return err;
}

error_t
diskfs_direnter_hard (struct node *dp, const char *name, struct node *np,
		      struct dirstat *ds, struct protid *cred)
{
  return EROFS;
}

error_t
diskfs_dirremove_hard (struct node *dp, struct dirstat *ds)
{
  return EROFS;
}

error_t
diskfs_dirrewrite_hard (struct node *dp, struct node *np, struct dirstat *ds)
{
  return EROFS;
}

/* Read the next entry set of DIR that a listing shows.  */
static error_t
next_listed_set (struct exfat_dir *dir, struct exfat_set *set, bool *found)
{
  error_t err;

  do
    err = exfat_dir_next_set (dir, set, found);
  while (!err && *found && set->error);
  return err;
}

int
diskfs_dirempty (struct node *dp, struct protid *cred)
{
  error_t err;
  struct exfat_dir dir;
  struct exfat_set *set = malloc (sizeof *set);
  bool found = true;

  if (!set)
    return 0;
  err = exfat_dir_open (&dir, &dp->dn->stream, dp == diskfs_root_node);
  if (!err)
    {
      err = next_listed_set (&dir, set, &found);
      exfat_dir_close (&dir);
    }
  free (set);
  return !err && !found;
}

#define DIRENT_ALIGN	__alignof__ (struct dirent)
#define DIRENT_LEN(namlen) \
  ((offsetof (struct dirent, d_name) + (namlen) + 1 + DIRENT_ALIGN - 1) \
   & ~(DIRENT_ALIGN - 1))

/* Implement the diskfs_get_directs callback as described in
   <hurd/diskfs.h>.  Entries 0 and 1 are `.' and `..', which the volume
   does not record; entry N + 2 is the Nth entry set of the directory.  */
error_t
diskfs_get_directs (struct node *dp, int entry, int nentries, char **data,
		    mach_msg_type_number_t *datacnt, vm_size_t bufsiz,
		    int *amt)
{
  error_t err = 0;
  struct disknode *dn = dp->dn;
  struct exfat_dir dir;
  struct exfat_set *set;
  size_t allocsize = bufsiz ? round_page (bufsiz) : 4 * vm_page_size;
  bool ouralloc = false;
  bool found = true;
  char *datap;
  int index = entry;
  int count = 0;

  set = malloc (sizeof *set);
  if (!set)
    return ENOMEM;
  err = exfat_dir_open (&dir, &dn->stream, dp == diskfs_root_node);
  if (err)
    {
      free (set);
      return err;
    }

  if (entry > 2)
    {
      int at = 0;

      if (dn->readdir_index <= entry - 2)
	{
	  at = dn->readdir_index;
	  dir.pos = dn->readdir_pos;
	}
      for (; !err && found && at < entry - 2; at++)
	err = next_listed_set (&dir, set, &found);
    }

  if (!err && found && allocsize > *datacnt)
    {
      void *new = mmap (0, allocsize, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);

      if (new == MAP_FAILED)
	err = errno;
      else
	{
	  *data = new;
	  ouralloc = true;
	}
    }
  datap = *data;

  while (!err && found
	 && (nentries == -1 || count < nentries)
	 && (!bufsiz || (vm_size_t) (datap - *data) < bufsiz))
    {
      char name[EXFAT_UTF8_NAME_MAX + 1];
      size_t namlen, reclen;
      struct dirent *userp;
      ino_t inum;
      unsigned char d_type = DT_DIR;

      if (index == 0)
	{
	  strcpy (name, ".");
	  namlen = 1;
	  inum = dp->cache_id;
	}
      else if (index == 1)
	{
	  strcpy (name, "..");
	  namlen = 2;
	  inum = dp == diskfs_root_node ? dp->cache_id : dn->parent;
	}
      else
	{
	  err = next_listed_set (&dir, set, &found);
	  if (err || !found)
	    break;
	  namlen = exfat_name_to_utf8 (set->name, set->name_length, name);
	  inum = set->offset / EXFAT_ENTRY_SIZE;
	  if (!(set->attributes & EXFAT_ATTR_DIRECTORY))
	    d_type = DT_REG;
	}

      reclen = DIRENT_LEN (namlen);
      if (datap - *data + reclen > allocsize)
	{
	  size_t newsize = allocsize * 2;
	  char *new = mmap (0, newsize, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);

	  if (new == MAP_FAILED)
	    {
	      err = errno;
	      break;
	    }
	  memcpy (new, *data, datap - *data);
	  if (ouralloc)
	    munmap (*data, allocsize);
	  datap = new + (datap - *data);
	  *data = new;
	  allocsize = newsize;
	  ouralloc = true;
	}

      userp = (struct dirent *) datap;
      userp->d_fileno = inum;
      userp->d_type = d_type;
      userp->d_reclen = reclen;
      userp->d_namlen = namlen;
      memcpy (userp->d_name, name, namlen + 1);

      datap += reclen;
      index++;
      count++;
    }

  if (!err && index >= 2)
    {
      dn->readdir_index = index - 2;
      dn->readdir_pos = dir.pos;
    }
  exfat_dir_close (&dir);
  free (set);

  if (err)
    {
      if (ouralloc)
	munmap (*data, allocsize);
      return err;
    }

  if (ouralloc && round_page (datap - *data) < allocsize)
    munmap (*data + round_page (datap - *data),
	    allocsize - round_page (datap - *data));
  *amt = count;
  *datacnt = datap - *data;
  return 0;
}
