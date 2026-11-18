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
#include <error.h>
#include "exfatfs.h"

/* What a lookup found, for the directory change that follows it.  */
struct dirstat
{
  enum
    {
      DS_NONE,
      DS_DOT,
      DS_DOTDOT,
      DS_SET			/* An entry set.  */
    } kind;
  off_t set_offset;		/* DS_SET: where the set is.  */
  /* DS_SET: the file.  */
  ino_t ino;
};

const size_t diskfs_dirstat_size = sizeof (struct dirstat);

void
diskfs_null_dirstat (struct dirstat *ds)
{
  memset (ds, 0, sizeof *ds);
}

error_t
diskfs_drop_dirstat (struct node *dp, struct dirstat *ds)
{
  ds->kind = DS_NONE;
  return 0;
}


/* Set *INUM to the inode number of the file that SET, read from the
   directory DP, describes.  Return EIO if the volume's directories
   are linked so that looking the file up could deadlock.  */
static error_t
set_inode (struct node *dp, const struct exfat_set *set, ino_t *inum)
{
  error_t err;
  struct exfat_location where
    = { set->offset, dp->cache_id, dp->dn->stream.contiguous };
  bool dir = (set->attributes & EXFAT_ATTR_DIRECTORY) && set->data_length;

  err = exfat_inode_get (&where, dir ? set->first_cluster : 0, inum);
  if (!err && exfat_inode_is_ancestor (*inum, dp->cache_id))
    /* Only a directory whose clusters hold its own entry set, or that
       of a directory above it, can show one of those.  DP's lock is
       held, and a lookup that holds the lock of one above it may be
       on its way here.  */
    err = EIO;
  return err;
}

/* Write the name of SET to UTF8 and return its length there, or zero
   if it is too long for a directory listing to carry.  */
static size_t
listable_name (const struct exfat_set *set, char *utf8)
{
  size_t len = exfat_name_to_utf8 (set->name, set->name_length, utf8);

  return len > EXFAT_LISTED_NAME_MAX ? 0 : len;
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
  bool entering = type == CREATE || type == RENAME;
  struct disknode *dn = dp->dn;
  struct node *np = NULL;
  struct exfat_location where;
  struct exfat_name_key key;
  struct exfat_dir dir;
  struct exfat_set *set;
  char utf8[EXFAT_UTF8_NAME_MAX + 1];
  bool found = false;
  ino_t inum = 0;

  if (npp)
    *npp = NULL;
  if (ds)
    diskfs_null_dirstat (ds);

  if (strcmp (name, ".") == 0)
    {
      if (npp)
	{
	  diskfs_nref (dp);
	  *npp = dp;
	}
      if (ds)
	ds->kind = DS_DOT;
      return 0;
    }

  if (strcmp (name, "..") == 0)
    {
      if (dp == diskfs_root_node)
	return EAGAIN;
      if (!exfat_inode_location (dp->cache_id, &where))
	return ENOENT;
      if (ds)
	ds->kind = DS_DOTDOT;
      if (!npp)
	return 0;

      if (!spec_dotdot)
	{
	  /* A directory is locked before the nodes in it.  */
	  pthread_mutex_unlock (&dp->lock);
	  err = diskfs_cached_lookup (where.parent, &np);
	  pthread_mutex_lock (&dp->lock);
	}
      else if (type == LOOKUP)
	{
	  diskfs_nput (dp);
	  err = diskfs_cached_lookup (where.parent, &np);
	}
      else
	{
	  np = diskfs_cached_ifind (where.parent);
	  err = 0;
	}
      if (!err)
	*npp = np;
      return err;
    }

  if (strlen (name) > EXFAT_LISTED_NAME_MAX)
    return ENAMETOOLONG;
  if (!exfat_name_key (name, &key))
    return entering ? EINVAL : ENOENT;

  set = malloc (sizeof *set);
  if (!set)
    return ENOMEM;
  err = exfat_dir_open (&dir, &dn->stream, dp == diskfs_root_node);
  if (err)
    {
      free (set);
      return err;
    }
  pthread_rwlock_rdlock (&exfat_entry_lock);
  while (!(err = exfat_dir_next_set (&dir, set, &found)) && found)
    if (exfat_name_match (set, &key))
      break;
  if (!err && !found)
    err = ENOENT;
  if (!err && !listable_name (set, utf8))
    /* A name that fits can still match one that does not, where the
       two cases of a letter differ in length.  */
    err = ENAMETOOLONG;
  if (!err)
    err = set->error;
  if (!err)
    err = set_inode (dp, set, &inum);
  pthread_rwlock_unlock (&exfat_entry_lock);
  exfat_dir_close (&dir);

  if (err == ENOENT && entering && !exfat_name_creatable (&key))
    err = EINVAL;
  else if (!err)
    {
      if (ds)
	{
	  ds->kind = DS_SET;
	  ds->set_offset = set->offset;
	  ds->ino = inum;
	}
      if (npp)
	{
	  err = diskfs_cached_lookup (inum, &np);
	  if (!err)
	    *npp = np;
	}
    }

  free (set);
  return err;
}

/* Note that the entries of DP changed.  */
static void
directory_changed (struct node *dp)
{
  /* A name that was looked up and not found may be there now, under
     another case as well.  The cache keeps such names as entries for
     node zero.  */
  static struct node none;

  diskfs_purge_lookup_cache (dp, &none);
  dp->dn->readdir_index = 0;
  dp->dn->readdir_pos = 0;
  dp->dn->readdir_hidden = 0;
  dp->dn_set_mtime = 1;
  dp->dn_set_ctime = 1;
}

/* Find unused entries of DP for SET to go in, growing DP if it has
   none, and set *WHERE to where they are.  */
static error_t
place_set (struct node *dp, const struct exfat_set *set,
	   struct exfat_location *where)
{
  error_t err;
  struct exfat_dir dir;
  off_t pos, run;

  for (;;)
    {
      err = exfat_dir_open (&dir, &dp->dn->stream, dp == diskfs_root_node);
      if (err)
	return err;
      pthread_rwlock_rdlock (&exfat_entry_lock);
      err = exfat_dir_find_free (&dir, set->count, &pos);
      pthread_rwlock_unlock (&exfat_entry_lock);
      exfat_dir_close (&dir);
      if (err != ENOSPC)
	break;
      if (dp->allocsize + CLUSTER_SIZE > EXFAT_DIR_SIZE_MAX)
	return ENOSPC;
      err = exfat_node_grow (dp, dp->allocsize + CLUSTER_SIZE);
      if (err)
	return err;
    }
  if (!err)
    err = exfat_stream_map (&dp->dn->stream, pos, &where->set_offset, &run);
  if (err)
    return err;
  where->parent = dp->cache_id;
  where->parent_contiguous = dp->dn->stream.contiguous;
  return 0;
}

/* Implement the diskfs_direnter_hard callback as described in
   <hurd/diskfs.h>.  NP gets its entry set here.  A file that has one
   already has its one name.  */
error_t
diskfs_direnter_hard (struct node *dp, const char *name, struct node *np,
		      struct dirstat *ds, struct protid *cred)
{
  error_t err;
  struct exfat_name_key key;
  struct exfat_location old, new;
  struct exfat_meta meta;
  struct exfat_set *set;

  ds->kind = DS_NONE;
  if (!exfat_name_key (name, &key) || !exfat_name_creatable (&key))
    return EINVAL;
  if (!exfat_inode_location (np->cache_id, &old))
    return ESTALE;
  if (old.set_offset)
    return EOPNOTSUPP;

  set = malloc (sizeof *set);
  if (!set)
    return ENOMEM;
  exfat_node_meta (np, &meta);
  exfat_set_create (set, &key, &meta);
  err = place_set (dp, set, &new);
  if (!err)
    {
      pthread_rwlock_wrlock (&exfat_entry_lock);
      err = exfat_set_write_at (new.set_offset, new.parent_contiguous, set,
				true);
      if (!err)
	exfat_inode_move (np->cache_id, &new);
      pthread_rwlock_unlock (&exfat_entry_lock);
    }
  free (set);

  if (!err)
    directory_changed (dp);
  return err;
}

/* Implement the diskfs_dirremove_hard callback as described in
   <hurd/diskfs.h>.  The clusters of the file are freed when its node
   goes; until then, or if the translator dies first, they stay
   allocated with no file that has them.  */
error_t
diskfs_dirremove_hard (struct node *dp, struct dirstat *ds)
{
  error_t err = 0;

  switch (ds->kind)
    {
    case DS_DOT:
    case DS_DOTDOT:
      /* The volume does not record these.  */
      break;

    case DS_SET:
      pthread_rwlock_wrlock (&exfat_entry_lock);
      err = exfat_set_delete_at (ds->set_offset, dp->dn->stream.contiguous);
      if (!err)
	exfat_inode_detach (ds->ino);
      pthread_rwlock_unlock (&exfat_entry_lock);
      if (!err)
	directory_changed (dp);
      break;

    default:
      err = EINVAL;
    }
  ds->kind = DS_NONE;
  return err;
}

/* A name cannot be given to another file yet.  */
error_t
diskfs_dirrewrite_hard (struct node *dp, struct node *np, struct dirstat *ds)
{
  ds->kind = DS_NONE;
  return EOPNOTSUPP;
}

/* Read the next entry set of DIR that can be opened.  */
static error_t
next_good_set (struct exfat_dir *dir, struct exfat_set *set, bool *found)
{
  error_t err;

  do
    err = exfat_dir_next_set (dir, set, found);
  while (!err && *found && set->error);
  return err;
}

/* Read the next entry set of DIR, which is open on the directory DP,
   that a listing shows: one that a lookup would not refuse with EIO.
   Write its name to UTF8, the length of that to *NAMLEN and its inode
   number to *INUM.  Add to *HIDDEN the number of sets passed over
   because their names are too long.  */
static error_t
next_listed_set (struct node *dp, struct exfat_dir *dir,
		 struct exfat_set *set, bool *found, char *utf8,
		 size_t *namlen, ino_t *inum, int *hidden)
{
  for (;;)
    {
      error_t err = next_good_set (dir, set, found);

      if (err || !*found)
	return err;
      *namlen = listable_name (set, utf8);
      if (!*namlen)
	{
	  ++*hidden;
	  continue;
	}
      err = set_inode (dp, set, inum);
      if (err != EIO)
	return err;
    }
}

/* Say that a listing of a directory left out HIDDEN names.  Only the
   first such directory is reported, so that a volume full of them
   does not fill the log.  */
static void
report_hidden_names (int hidden)
{
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static bool reported;
  bool first;

  pthread_mutex_lock (&lock);
  first = !reported;
  reported = true;
  pthread_mutex_unlock (&lock);
  if (first)
    error (0, 0, "%s: hiding %d name%s longer than %d bytes in a directory;"
	   " this is reported once, for the first such directory listed",
	   diskfs_disk_name, hidden, hidden == 1 ? "" : "s",
	   EXFAT_LISTED_NAME_MAX);
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
      pthread_rwlock_rdlock (&exfat_entry_lock);
      err = next_good_set (&dir, set, &found);
      pthread_rwlock_unlock (&exfat_entry_lock);
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
   does not record; entry N + 2 is the Nth entry set that a listing
   shows.  */
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
  char name[EXFAT_UTF8_NAME_MAX + 1];
  size_t namlen;
  ino_t inum;
  int index = entry;
  int count = 0;
  int hidden = 0;

  set = malloc (sizeof *set);
  if (!set)
    return ENOMEM;
  err = exfat_dir_open (&dir, &dn->stream, dp == diskfs_root_node);
  if (err)
    {
      free (set);
      return err;
    }
  pthread_rwlock_rdlock (&exfat_entry_lock);

  if (entry > 2)
    {
      int at = 0;

      if (dn->readdir_index <= entry - 2)
	{
	  at = dn->readdir_index;
	  dir.pos = dn->readdir_pos;
	  hidden = dn->readdir_hidden;
	}
      for (; !err && found && at < entry - 2; at++)
	err = next_listed_set (dp, &dir, set, &found, name, &namlen, &inum,
			       &hidden);
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
      size_t reclen;
      struct dirent *userp;
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
	  if (dp == diskfs_root_node)
	    inum = dp->cache_id;
	  else
	    {
	      struct exfat_location where;

	      if (!exfat_inode_location (dp->cache_id, &where))
		{
		  err = ENOENT;
		  break;
		}
	      inum = where.parent;
	    }
	}
      else
	{
	  err = next_listed_set (dp, &dir, set, &found, name, &namlen, &inum,
				 &hidden);
	  if (err || !found)
	    break;
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
      dn->readdir_hidden = hidden;
    }
  pthread_rwlock_unlock (&exfat_entry_lock);
  if (!err && !found && hidden)
    report_hidden_names (hidden);
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
