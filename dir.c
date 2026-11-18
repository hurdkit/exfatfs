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
      DS_SET,			/* An entry set.  */
      DS_RENAMED		/* A name that a rename is about to remove.  */
    } kind;
  off_t set_offset;		/* DS_SET: where the set is.  */
  /* DS_SET and DS_RENAMED: the file.  DS_NONE, after a lookup for
     RENAME found no such name: the file whose name differs from it in
     case only, if there is one.  */
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


/* libdiskfs renames a file by entering it under its new name and then
   removing its old name, as for a file with two links.  A file here
   has one entry set, so entering a file that has one already renames
   that set, and what is left of the old name is a record here, for
   the removal that follows to find.  */
struct renamed
{
  struct renamed *next;
  ino_t dir;			/* The directory the old name was in.  */
  struct exfat_name_key name;
  ino_t ino;
  /* The links libdiskfs counts for the file while it is between its
     two names.  A record whose removal never came, because the rename
     failed after the first half, is known by the count having gone
     back.  */
  nlink_t links;
};

static struct renamed *renamed;
static pthread_mutex_t renamed_lock = PTHREAD_MUTEX_INITIALIZER;

/* Forget the old name of the file numbered INO.  */
void
exfat_rename_forget (ino_t ino)
{
  pthread_mutex_lock (&renamed_lock);
  for (struct renamed **r = &renamed; *r; r = &(*r)->next)
    if ((*r)->ino == ino)
      {
	struct renamed *gone = *r;

	*r = gone->next;
	free (gone);
	break;
      }
  pthread_mutex_unlock (&renamed_lock);
}

static error_t
renamed_add (ino_t dir, const struct exfat_name_key *name, struct node *np)
{
  struct renamed *r = malloc (sizeof *r);

  if (!r)
    return ENOMEM;
  exfat_rename_forget (np->cache_id);
  r->dir = dir;
  r->name = *name;
  r->ino = np->cache_id;
  r->links = np->dn_stat.st_nlink;
  pthread_mutex_lock (&renamed_lock);
  r->next = renamed;
  renamed = r;
  pthread_mutex_unlock (&renamed_lock);
  return 0;
}

/* Return the file that had the name NAME in the directory numbered
   DIR before it was renamed, or zero, and set *LINKS to the links it
   had then.  */
static ino_t
renamed_find (ino_t dir, const struct exfat_name_key *name, nlink_t *links)
{
  ino_t ino = 0;

  pthread_mutex_lock (&renamed_lock);
  for (struct renamed *r = renamed; r && !ino; r = r->next)
    if (r->dir == dir && r->name.length == name->length
	&& memcmp (r->name.upcased, name->upcased,
		   name->length * sizeof *name->upcased) == 0)
      {
	ino = r->ino;
	*links = r->links;
      }
  pthread_mutex_unlock (&renamed_lock);
  return ino;
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
  nlink_t links;

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

  if (type == REMOVE && (inum = renamed_find (dp->cache_id, &key, &links)))
    {
      err = diskfs_cached_lookup (inum, &np);
      if (err)
	return err;
      if (np->dn_stat.st_nlink == links)
	{
	  ds->kind = DS_RENAMED;
	  ds->ino = inum;
	  *npp = np;
	  return 0;
	}
      /* The rename that left the record never came back for it.  */
      exfat_rename_forget (inum);
      diskfs_nput (np);
    }

  set = malloc (sizeof *set);
  if (!set)
    return ENOMEM;
  err = exfat_dir_open (&dir, &dn->stream, dp == diskfs_root_node);
  if (err)
    {
      free (set);
      return err;
    }
  /* The number is found under the same lock as the set: a rename
     moves both together.  */
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
  else if (!err && type == RENAME && !exfat_name_exact (set, &key))
    {
      /* libdiskfs takes a rename onto the file itself as done.  One
	 that changes the case of the name is not, so the name is
	 reported free, and diskfs_direnter_hard gets to see whose it
	 is.  */
      ds->ino = inum;
      err = ENOENT;
    }
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
	  /* A rename may have moved the set while the node was waited
	     for.  */
	  if (!err && (!exfat_inode_location (inum, &where)
		       || where.set_offset != set->offset))
	    {
	      diskfs_nput (np);
	      err = ENOENT;
	    }
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
  exfat_node_modified (dp);
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
  bool hidden = false;

  for (;;)
    {
      err = exfat_dir_open (&dir, &dp->dn->stream, dp == diskfs_root_node);
      if (err)
	return err;
      pthread_rwlock_rdlock (&exfat_entry_lock);
      err = exfat_dir_find_free (&dir, set->count, &pos, &hidden);
      pthread_rwlock_unlock (&exfat_entry_lock);
      exfat_dir_close (&dir);
      if (hidden)
	{
	  dp->dn->entries_hidden = true;
	  error (0, 0, "%s: directory inode %llu holds entries in use after"
		 " its end, which a new entry would bring back; nothing more"
		 " is written to it, and fsck.exfat should check the volume",
		 diskfs_disk_name, (unsigned long long) dp->cache_id);
	}
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

/* What it takes to enter a file in a directory: its entry set, and
   the changes to the entries of the set that had the name, of those
   that get the set, and of those that had it.  */
struct entering
{
  struct exfat_set set;
  struct exfat_set_change target, new, old;
};

/* Give NP, which is locked, an entry set in DP, or give the entry set
   it has a new name and place there: the first half of a rename, which
   here is all of it.  The name is KEY.  If TARGET_OFFSET is not zero,
   it is instead the name of the set there, which the file numbered
   REPLACED loses to NP.

   Everything that can fail for want of memory, of unused entries, or
   of clusters for DP to grow by, fails before any of the sets is
   written, and leaves the volume and the records here as they were.
   After that only the store can fail; exfat_set_changes_apply then
   takes back what it can and has the volume stay marked as dirty, and
   the records here are left as they were.

   What is written, in this order, and so what a death of the
   translator can leave:

   - If the new name takes as many entries as the old one, in the same
     directory, and one request writes them: the set, where it is.
   - If NP's set fits in the entries of the one that has the name, and
     one request writes them: NP's set over that one, so that the name
     is never without a file, and then NP's old set as unused.  In
     between, NP is under both names, sharing its clusters.
   - Otherwise: the set that has the name, if there is one, as unused;
     NP's set, whole, into unused entries; NP's old set as unused.

   A set that a cluster boundary splits is not written over where it
   is, because a death between the two requests would leave a set with
   a wrong checksum.  */
static error_t
enter_set (struct node *dp, struct node *np, const struct exfat_name_key *key,
	   off_t target_offset, ino_t replaced)
{
  error_t err = 0;
  bool contiguous = dp->dn->stream.contiguous;
  struct exfat_location old;
  struct exfat_location new = { target_offset, dp->cache_id, contiguous };
  struct exfat_name_key target_key, old_key;
  struct entering *w;
  struct exfat_set_change *steps[3];
  unsigned nsteps = 0;
  bool renaming, moving = true;

  if (dp->dn->entries_hidden)
    return EIO;
  if (!exfat_inode_location (np->cache_id, &old))
    return ESTALE;
  renaming = old.set_offset != 0;
  if (target_offset && !renaming)
    return ESTALE;

  w = malloc (sizeof *w);
  if (!w)
    return ENOMEM;

  /* NP is locked, and so is the file that has the name, so their sets
     stay as they are read here.  */
  pthread_rwlock_rdlock (&exfat_entry_lock);
  if (target_offset)
    {
      err = exfat_set_read_at (target_offset, contiguous, &w->set);
      if (!err)
	{
	  exfat_name_key_of_set (&w->set, &target_key);
	  key = &target_key;
	  err = exfat_set_change_at (&w->target, target_offset, contiguous,
				     w->set.count);
	}
    }
  if (!err && renaming)
    {
      err = exfat_set_read_at (old.set_offset, old.parent_contiguous,
			       &w->set);
      if (!err)
	{
	  exfat_name_key_of_set (&w->set, &old_key);
	  err = exfat_set_change_at (&w->old, old.set_offset,
				     old.parent_contiguous, w->set.count);
	}
      if (!err)
	err = exfat_set_rename (&w->set, key);
    }
  pthread_rwlock_unlock (&exfat_entry_lock);
  if (!err && !renaming)
    {
      struct exfat_meta meta;

      exfat_node_meta (np, &meta);
      exfat_set_create (&w->set, key, &meta);
    }

  if (!err && target_offset && w->set.count <= w->target.count)
    {
      steps[nsteps++] = &w->target;
      if (exfat_set_change_in_one_piece (&w->target))
	exfat_set_change_put (&w->target, &w->set);
      else
	{
	  exfat_set_change_put (&w->target, NULL);
	  exfat_set_change_follow (&w->new, &w->target, w->set.count);
	  exfat_set_change_put (&w->new, &w->set);
	  steps[nsteps++] = &w->new;
	}
    }
  else if (!err && !target_offset && renaming && old.parent == dp->cache_id
	   && w->set.count == w->old.count
	   && exfat_set_change_in_one_piece (&w->old))
    {
      exfat_set_change_put (&w->old, &w->set);
      steps[nsteps++] = &w->old;
      moving = false;
    }
  else if (!err)
    {
      /* DP is locked, so what place_set finds unused stays so.  */
      err = place_set (dp, &w->set, &new);
      if (!err)
	{
	  pthread_rwlock_rdlock (&exfat_entry_lock);
	  err = exfat_set_change_at (&w->new, new.set_offset,
				     new.parent_contiguous, w->set.count);
	  pthread_rwlock_unlock (&exfat_entry_lock);
	}
      if (!err && !exfat_set_change_into_unused (&w->new))
	err = EIO;
      if (!err)
	{
	  if (target_offset)
	    {
	      exfat_set_change_put (&w->target, NULL);
	      steps[nsteps++] = &w->target;
	    }
	  exfat_set_change_put (&w->new, &w->set);
	  steps[nsteps++] = &w->new;
	}
    }
  if (!err && renaming && moving)
    {
      exfat_set_change_put (&w->old, NULL);
      steps[nsteps++] = &w->old;
    }

  if (!err && renaming)
    err = renamed_add (old.parent, &old_key, np);
  if (!err)
    {
      pthread_rwlock_wrlock (&exfat_entry_lock);
      err = exfat_set_changes_apply (steps, nsteps);
      if (!err)
	{
	  if (target_offset)
	    exfat_inode_detach (replaced);
	  if (moving)
	    exfat_inode_move (np->cache_id, &new);
	}
      pthread_rwlock_unlock (&exfat_entry_lock);
      if (err && renaming)
	exfat_rename_forget (np->cache_id);
      if (!err && target_offset)
	/* The set that had the name is gone, and what it held of its
	   own with it.  */
	exfat_set_free_unknown (w->target.before, w->target.count);
    }
  free (w);

  if (!err)
    directory_changed (dp);
  return err;
}

/* Implement the diskfs_direnter_hard callback as described in
   <hurd/diskfs.h>.  NP gets its entry set here if it has none.  If it
   has one, this is the first half of a rename.  */
error_t
diskfs_direnter_hard (struct node *dp, const char *name, struct node *np,
		      struct dirstat *ds, struct protid *cred)
{
  ino_t other = ds->ino;
  struct exfat_name_key key;

  ds->kind = DS_NONE;
  if (!exfat_name_key (name, &key) || !exfat_name_creatable (&key))
    return EINVAL;
  if (other && other != np->cache_id)
    /* Another file has this name in another case.  libdiskfs would
       have had to be told so to replace it, and was not, for the sake
       of the file that changes the case of its own name.  */
    return EEXIST;
  return enter_set (dp, np, &key, 0, 0);
}

/* Implement the diskfs_dirremove_hard callback as described in
   <hurd/diskfs.h>.  The clusters of the file are freed when its node
   goes; until then, or if the translator dies first, they stay
   allocated with no file that has them.  */
error_t
diskfs_dirremove_hard (struct node *dp, struct dirstat *ds)
{
  error_t err = 0;
  uint8_t *raw;
  unsigned count;

  switch (ds->kind)
    {
    case DS_DOT:
    case DS_DOTDOT:
      /* The volume does not record these.  */
      break;

    case DS_RENAMED:
      /* diskfs_direnter_hard marked the set unused, without DP's
	 lock.  */
      exfat_rename_forget (ds->ino);
      directory_changed (dp);
      break;

    case DS_SET:
      if (dp->dn->entries_hidden)
	{
	  err = EIO;
	  break;
	}
      raw = malloc (EXFAT_SET_MAX * EXFAT_ENTRY_SIZE);
      if (!raw)
	{
	  err = ENOMEM;
	  break;
	}
      pthread_rwlock_wrlock (&exfat_entry_lock);
      err = exfat_set_delete_at (ds->set_offset, dp->dn->stream.contiguous,
				 raw, &count);
      if (!err)
	exfat_inode_detach (ds->ino);
      pthread_rwlock_unlock (&exfat_entry_lock);
      if (!err)
	{
	  exfat_set_free_unknown (raw, count);
	  directory_changed (dp);
	}
      free (raw);
      break;

    default:
      err = EINVAL;
    }
  ds->kind = DS_NONE;
  return err;
}

/* Implement the diskfs_dirrewrite_hard callback as described in
   <hurd/diskfs.h>: the name DS found is to be NP's from now on, and
   this is the first half of a rename, as in diskfs_direnter_hard.  */
error_t
diskfs_dirrewrite_hard (struct node *dp, struct node *np, struct dirstat *ds)
{
  off_t target_offset = ds->set_offset;
  ino_t replaced = ds->ino;

  if (ds->kind == DS_DOTDOT)
    {
      /* diskfs_direnter_hard records the new parent.  */
      ds->kind = DS_NONE;
      return 0;
    }
  if (ds->kind != DS_SET)
    return EINVAL;
  ds->kind = DS_NONE;
  return enter_set (dp, np, NULL, target_offset, replaced);
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

/* A directory is empty when none of its entries is the File entry of
   a set, whether a lookup would accept that set or not and whether a
   listing shows its name or not.  Removing the directory frees its
   clusters, and the clusters of a file left in it would then belong
   to nothing, damaged as its set may be.  An entry that cannot be
   told from a File entry counts as one, and a directory that cannot
   be read is not empty.  */
int
diskfs_dirempty (struct node *dp, struct protid *cred)
{
  error_t err;
  struct exfat_dir dir;
  const uint8_t *entry = NULL;

  if (exfat_dir_open (&dir, &dp->dn->stream, dp == diskfs_root_node))
    return 0;
  pthread_rwlock_rdlock (&exfat_entry_lock);
  while (!(err = exfat_dir_next_primary (&dir, &entry))
	 && entry && (entry[0] & EXFAT_TYPE_BENIGN))
    dir.pos += EXFAT_ENTRY_SIZE;
  pthread_rwlock_unlock (&exfat_entry_lock);
  exfat_dir_close (&dir);
  return !err && !entry;
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
