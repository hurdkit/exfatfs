/* inode.c - Nodes and their attributes.

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
#include <unistd.h>
#include <time.h>
#include <error.h>
#include "exfatfs.h"
#include "libdiskfs/fs_S.h"

pthread_rwlock_t exfat_entry_lock = PTHREAD_RWLOCK_INITIALIZER;

/* The user must define this function if she wants to use the node
   cache.  Create and initialize a node.  */
error_t
diskfs_user_make_node (struct node **npp, struct lookup_context *ctx)
{
  struct node *np = diskfs_make_node_alloc (sizeof (struct disknode));

  if (np == NULL)
    return ENOMEM;
  memset (np->dn, 0, sizeof *np->dn);
  exfat_stream_init (&np->dn->stream, 0, false, 0);
  pthread_rwlock_init (&np->dn->alloc_lock, NULL);
  pthread_mutex_init (&np->dn->valid_lock, NULL);
  *npp = np;
  return 0;
}

/* Return the number of directories in the directory NP.  The volume
   records no `..', but libdiskfs counts one link to a directory for
   each directory in it.  Every File entry that says it is a directory
   counts, whether a lookup would accept its set or not: too many
   links keep a directory, too few would have it freed while it is in
   use.  */
static nlink_t
count_subdirectories (struct node *np)
{
  struct exfat_dir dir;
  nlink_t count = 0;

  if (exfat_dir_open (&dir, &np->dn->stream, np->cache_id == EXFAT_ROOT_INO))
    return 0;
  pthread_rwlock_rdlock (&exfat_entry_lock);
  for (;;)
    {
      const uint8_t *entry;

      if (exfat_dir_next_primary (&dir, &entry) || !entry)
	break;
      if (entry[0] == EXFAT_TYPE_FILE
	  && (get_le16 (entry + 4) & EXFAT_ATTR_DIRECTORY))
	count++;
      dir.pos += EXFAT_ENTRY_SIZE;
    }
  pthread_rwlock_unlock (&exfat_entry_lock);
  exfat_dir_close (&dir);
  return count;
}

static mode_t
node_mode (const struct disknode *dn)
{
  /* The read-only attribute of a directory says nothing about adding
     to it or removing from it.  */
  if (dn->attributes & EXFAT_ATTR_DIRECTORY)
    return S_IFDIR | 0755;
  return S_IFREG | (dn->attributes & EXFAT_ATTR_READONLY ? 0444 : 0644);
}

/* The user must define this function if she wants to use the node
   cache.  Read stat information out of the on-disk node, or with CTX
   set up a file that is not on the disk yet.  */
error_t
diskfs_user_read_node (struct node *np, struct lookup_context *ctx)
{
  error_t err;
  struct disknode *dn = np->dn;
  struct stat *st = &np->dn_stat;
  bool directory;

  if (np->cache_id == EXFAT_ROOT_INO)
    {
      cluster_t clusters;

      err = exfat_chain_length (vol.root_cluster, &clusters);
      if (err)
	return err;
      exfat_stream_replace (&dn->stream, vol.root_cluster, false,
			    (off_t) clusters << vol.cluster_shift);
      dn->valid_length = dn->stream.length;
      dn->attributes = EXFAT_ATTR_DIRECTORY;
      st->st_size = dn->stream.length;
      st->st_mtim = st->st_atim = (struct timespec) { 0, 0 };
    }
  else if (ctx)
    {
      dn->attributes = S_ISDIR (ctx->mode)
	? EXFAT_ATTR_DIRECTORY
	: EXFAT_ATTR_ARCHIVE | (ctx->mode & 0222 ? 0 : EXFAT_ATTR_READONLY);
      clock_gettime (CLOCK_REALTIME, &dn->create_time);
      st->st_size = 0;
      st->st_mtim = st->st_atim = dn->create_time;
    }
  else
    {
      struct exfat_location where;
      struct exfat_set *set;

      set = malloc (sizeof *set);
      if (!set)
	return ENOMEM;
      pthread_rwlock_rdlock (&exfat_entry_lock);
      if (!exfat_inode_location (np->cache_id, &where) || !where.set_offset)
	err = ESTALE;
      else
	err = exfat_set_read_at (where.set_offset, where.parent_contiguous,
				 set);
      pthread_rwlock_unlock (&exfat_entry_lock);
      if (err)
	{
	  free (set);
	  return err;
	}

      exfat_stream_replace (&dn->stream, set->first_cluster,
			    set->contiguous, round_cluster (set->data_length));
      dn->valid_length = set->valid_length;
      dn->attributes = set->attributes;
      dn->create_time = set->create_time;
      st->st_size = set->data_length;
      st->st_mtim = set->modify_time;
      st->st_atim = set->access_time;
      free (set);
    }
  directory = dn->attributes & EXFAT_ATTR_DIRECTORY;

  dn->readdir_index = 0;
  dn->readdir_pos = 0;
  dn->readdir_hidden = 0;

  st->st_fstype = FSTYPE_MSLOSS;
  st->st_fsid = getpid ();
  st->st_ino = np->cache_id;
  st->st_gen = 0;
  st->st_rdev = 0;
  st->st_nlink = directory ? 2 + (ctx ? 0 : count_subdirectories (np)) : 1;
  st->st_uid = fs_uid;
  st->st_gid = fs_gid;
  st->st_author = fs_uid;
  st->st_flags = 0;
  st->st_mode = node_mode (dn);

  np->allocsize = dn->stream.length;
  st->st_blksize = vm_page_size;
  st->st_blocks = np->allocsize >> 9;

  /* The volume records when a file was created, not when its status
     last changed; the last modification is the closest to that.  */
  st->st_ctim = st->st_mtim;

  return 0;
}

/* The last reference to a node has gone away; drop it from the hash
   table and clean all state in the dn structure.  */
void
diskfs_node_norefs (struct node *np)
{
  assert_backtrace (!np->dn->pager);
  exfat_rename_forget (np->cache_id);
  exfat_stream_fini (&np->dn->stream);
  pthread_rwlock_destroy (&np->dn->alloc_lock);
  pthread_mutex_destroy (&np->dn->valid_lock);
  free (np);
}

/* The user must define this function if she wants to use the node
   cache.  The last hard reference to a node has gone away; arrange to
   have all the weak references dropped that can be.  */
void
diskfs_user_try_dropping_softrefs (struct node *np)
{
  drop_pager_softrefs (np);
}

/* The last hard reference to a node has gone away.  */
void
diskfs_lost_hardrefs (struct node *np)
{
}

/* A new hard reference to a node has been created; it's now OK to
   have unused weak references.  */
void
diskfs_new_hardrefs (struct node *np)
{
  allow_pager_softrefs (np);
}

/* Reload all data specific to NODE from disk, without writing anything.
   Always called with DISKFS_READONLY true.  */
error_t
diskfs_node_reload (struct node *node)
{
  error_t err;

  pthread_rwlock_wrlock (&node->dn->alloc_lock);
  err = diskfs_user_read_node (node, NULL);
  pthread_rwlock_unlock (&node->dn->alloc_lock);

  /* Pages are being read in meanwhile, and one read before the node
     changed must not outlive it.  */
  flush_node_pager (node);
  return err;
}

void
refresh_node_stats (void)
{
  error_t refresh_one_node_stat (struct node *node)
    {
      node->dn_stat.st_uid = fs_uid;
      node->dn_stat.st_gid = fs_gid;
      node->dn_stat.st_author = fs_uid;
      return 0;
    }

  diskfs_node_iterate (refresh_one_node_stat);
}

/* Set *ST with appropriate values to reflect the current state of the
   filesystem.  */
error_t
diskfs_set_statfs (struct statfs *st)
{
  st->f_type = FSTYPE_MSLOSS;
  st->f_bsize = CLUSTER_SIZE;
  st->f_frsize = CLUSTER_SIZE;
  st->f_blocks = vol.cluster_count;
  st->f_bfree = exfat_free_clusters;
  st->f_bavail = exfat_free_clusters;
  /* The volume has no table of files to count.  */
  st->f_files = 0;
  st->f_ffree = 0;
  st->f_favail = 0;
  st->f_fsid = getpid ();
  return 0;
}



/* Writing.  */

/* Set *META to what the entry set of NP, which is locked, is to say
   with DATA_LENGTH as the size.

   The allocation it records is the clusters of NP that DATA_LENGTH
   takes.  NP has more than those between diskfs_grow and the write
   that asked for them, and on the volume they belong to no file until
   the size says that the file has them: sections 6.2.3 and 7.6.7 have
   DataLength be the size of the file, and what its allocation holds.  */
static void
node_meta (struct node *np, off_t data_length, struct exfat_meta *meta)
{
  struct disknode *dn = np->dn;
  bool clusters = data_length > 0;

  *meta = (struct exfat_meta)
    {
      .attributes = dn->attributes,
      .first_cluster = clusters ? dn->stream.first_cluster : 0,
      .contiguous = clusters && dn->stream.contiguous,
      .data_length = data_length,
      .valid_length = data_length,
      .create_time = dn->create_time,
      .modify_time = np->dn_stat.st_mtim,
      .access_time = np->dn_stat.st_atim,
    };
  if (!(dn->attributes & EXFAT_ATTR_DIRECTORY))
    {
      if (np->dn_stat.st_size < meta->valid_length)
	meta->valid_length = np->dn_stat.st_size;
      pthread_mutex_lock (&dn->valid_lock);
      if (dn->valid_length < meta->valid_length)
	meta->valid_length = dn->valid_length;
      pthread_mutex_unlock (&dn->valid_lock);
    }
}

/* Note that what NP, which is locked, holds is changing: its data or
   its size.  As MS-DOS does, that sets the Archive attribute of a
   file (section 7.4.4).  It is an access as well, so LastAccessed
   moves with LastModified (section 7.4.7).  Setting the times with
   utimes afterwards still has the last word.  */
void
exfat_node_modified (struct node *np)
{
  if (!(np->dn->attributes & EXFAT_ATTR_DIRECTORY))
    np->dn->attributes |= EXFAT_ATTR_ARCHIVE;
  np->dn_set_atime = 1;
}

/* Write the entry set of NP, which is locked, with DATA_LENGTH as the
   size.  A file that has no entry set is left alone.  So is a set
   that would change in its time of last access alone: reading a file
   does not write to the volume, and the time is recorded along with
   the next change that is.  */
error_t
exfat_node_write_set (struct node *np, off_t data_length)
{
  error_t err = 0;
  struct exfat_location where;
  struct exfat_set *set;
  struct exfat_meta meta;

  if (np->cache_id == EXFAT_ROOT_INO)
    return 0;
  node_meta (np, data_length, &meta);

  set = malloc (sizeof *set);
  if (!set)
    return ENOMEM;
  pthread_rwlock_wrlock (&exfat_entry_lock);
  if (exfat_inode_location (np->cache_id, &where) && where.set_offset)
    {
      err = exfat_set_read_at (where.set_offset, where.parent_contiguous,
			       set);
      if (!err && exfat_set_differs (set, &meta))
	{
	  exfat_set_update (set, &meta);
	  err = exfat_set_write_at (where.set_offset, where.parent_contiguous,
				    set);
	}
    }
  pthread_rwlock_unlock (&exfat_entry_lock);
  free (set);
  return err;
}

/* The size to record for NP on the volume: that of a file, and the
   allocation of a directory.  */
static off_t
node_data_length (struct node *np)
{
  return np->dn->attributes & EXFAT_ATTR_DIRECTORY
    ? np->allocsize : np->dn_stat.st_size;
}

/* Set *META to what the entry set of NP, which is locked, is to say,
   for a set that is yet to be made.  */
void
exfat_node_meta (struct node *np, struct exfat_meta *meta)
{
  node_meta (np, node_data_length (np), meta);
}

/* Sync the info in NP->dn_stat and any associated format-specific
   information to disk.  */
void
diskfs_write_disknode (struct node *np, int wait)
{
  error_t err;
  struct disknode *dn = np->dn;
  struct stat *st = &np->dn_stat;

  /* The pager sets this again when it moves ValidDataLength, which
     may be while the set is written here; clearing it afterwards
     could lose that.  */
  np->dn_stat_dirty = 0;
  if (diskfs_readonly)
    return;

  if (S_ISREG (st->st_mode))
    {
      /* The volume has one bit for the mode.  */
      if (st->st_mode & 0222)
	dn->attributes &= ~EXFAT_ATTR_READONLY;
      else
	dn->attributes |= EXFAT_ATTR_READONLY;
      st->st_mode = node_mode (dn);
    }

  err = exfat_node_write_set (np, node_data_length (np));
  if (err)
    error (0, err, "%s: writing the directory entry of inode %llu",
	   diskfs_disk_name, (unsigned long long) np->cache_id);
}

/* Free the clusters of NP, which is locked, that its size does not
   take.  A file is left with those when the write that diskfs_grow
   was for does not follow, or when freeing them failed before.  No
   entry set records them, so they would stay allocated for
   nothing.  */
static void
trim_node (struct node *np)
{
  off_t used = round_cluster (np->dn_stat.st_size);

  if (diskfs_readonly || (np->dn->attributes & EXFAT_ATTR_DIRECTORY)
      || used >= np->allocsize
      /* Whoever has the conch was told that it may write up to the
	 end of the allocation without asking.  */
      || np->conch.holder)
    return;
  diskfs_truncate (np, np->dn_stat.st_size);
}

/* Write the entry set of every node that needs it.  No write to a
   file is under way while its node is locked here, so this is when
   clusters that a write asked for and did not use can go.  */
void
exfat_write_all_nodes (void)
{
  error_t write_one (struct node *node)
    {
      trim_node (node);
      diskfs_node_update (node, 1);
      return 0;
    }

  diskfs_node_iterate (write_one);
}

/* Return whether a node has clusters that no entry set on the volume
   records, and that nothing will free once the translator is gone: a
   file that was removed while it was open, which has its clusters
   until it is closed, or a file with more clusters than its size
   takes, because trim_node left it alone or failed.  */
bool
exfat_nodes_leak_clusters (void)
{
  bool found = false;

  error_t check_one (struct node *node)
    {
      struct exfat_location where;

      if (!node->allocsize)
	return 0;
      if (exfat_inode_location (node->cache_id, &where) && !where.set_offset)
	found = true;
      if (!(node->dn->attributes & EXFAT_ATTR_DIRECTORY)
	  && node->allocsize > round_cluster (node->dn_stat.st_size))
	found = true;
      return 0;
    }

  diskfs_node_iterate (check_one);
  return found;
}

/* Make the allocation of NP, which is locked, hold SIZE bytes.

   The entry set of a directory says so at once: its size is its
   allocation.  That of a file does not change here.  It records the
   clusters that the size of the file takes, and the new ones become
   the file's when the write that asked for them has moved the size
   and the set is written with it.  A write that does not get that far
   leaves the file the size it was.

   Dying before the set is written leaves the file as it was, and
   clusters allocated that no file has.  A file that has a FAT chain
   already is the exception: exfat_stream_grow joins the new clusters
   to that chain at once, and until the set is written the chain is
   longer than DataLength says (section 7.6.7), which fsck.exfat
   reports and cuts back, with nothing that the file held lost.  No
   order of the two writes avoids that: the set first would leave a
   chain shorter than its file.  Dying after the set is written
   leaves a file that reads as zeros where nothing was written yet.  */
error_t
exfat_node_grow (struct node *np, off_t size)
{
  error_t err;
  struct disknode *dn = np->dn;
  bool directory = dn->attributes & EXFAT_ATTR_DIRECTORY;
  cluster_t had = np->allocsize >> vol.cluster_shift;
  cluster_t clusters;

  if (size <= np->allocsize)
    return 0;
  if (size > (off_t) vol.cluster_count << vol.cluster_shift)
    return ENOSPC;
  clusters = round_cluster (size) >> vol.cluster_shift;

  pthread_rwlock_wrlock (&dn->alloc_lock);
  /* A directory is searched up to its size, so its new clusters hold
     zeros before they are part of it.  */
  err = exfat_stream_grow (&dn->stream, clusters, directory);
  if (!err && directory)
    {
      if (!had)
	err = exfat_inode_set_dir_cluster (np->cache_id,
					   dn->stream.first_cluster);
      if (!err)
	err = exfat_node_write_set (np, dn->stream.length);
      if (err)
	{
	  exfat_stream_shrink (&dn->stream, had);
	  if (!had)
	    exfat_inode_set_dir_cluster (np->cache_id, 0);
	}
    }
  if (!err)
    {
      np->allocsize = dn->stream.length;
      np->dn_stat.st_blocks = np->allocsize >> 9;
      if (directory)
	{
	  np->dn_stat.st_size = np->allocsize;
	  dn->valid_length = np->allocsize;
	}
    }
  pthread_rwlock_unlock (&dn->alloc_lock);
  return err;
}

error_t
diskfs_grow (struct node *node, loff_t size, struct protid *cred)
{
  if (diskfs_check_readonly ())
    return EROFS;
  exfat_node_modified (node);
  return exfat_node_grow (node, size);
}

/* Truncate locked node NODE to be LENGTH bytes long.

   The entry set gets the new size before the chain is cut and before
   the clusters are freed.  Dying in between leaves a chain longer
   than its file, or clusters allocated that no file has, and never a
   file whose clusters are free.  The store is told to put the set on
   its medium in between, so that a cache that writes in another order
   does not let the clusters go to another file while the set still
   names them; this is also where a file that was removed, or replaced
   by a rename, frees its clusters, after its set was marked unused.
   A store that cannot be told so, such as a device without
   DEV_FLUSH_CACHE, leaves that to the order the medium writes in.  */
error_t
diskfs_truncate (struct node *node, loff_t length)
{
  error_t err = 0;
  struct disknode *dn = node->dn;
  struct stat *st = &node->dn_stat;
  bool directory = dn->attributes & EXFAT_ATTR_DIRECTORY;
  cluster_t clusters = round_cluster (length) >> vol.cluster_shift;
  loff_t page_end = round_page (length);
  off_t had = node->allocsize;

  if (diskfs_check_readonly ())
    return EROFS;
  if (length >= st->st_size && round_cluster (length) >= node->allocsize)
    return 0;

  if (length < st->st_size)
    {
      if (!directory && length < page_end)
	{
	  /* A page that the kernel holds keeps what the file had past
	     its new end, and would show it again if the file grew.  */
	  size_t amount = (page_end < st->st_size ? page_end : st->st_size)
	    - length;
	  char *zeros = alloca (amount);

	  memset (zeros, 0, amount);
	  err = diskfs_node_rdwr (node, zeros, length, amount, 1, NULL, NULL);
	  if (err)
	    return err;
	}
      st->st_size = length;
      exfat_node_modified (node);
      node->dn_set_mtime = 1;
      node->dn_set_ctime = 1;
      node->dn_stat_dirty = 1;
    }

  pthread_rwlock_wrlock (&dn->alloc_lock);
  pthread_mutex_lock (&dn->valid_lock);
  if (dn->valid_length > length)
    dn->valid_length = length;
  pthread_mutex_unlock (&dn->valid_lock);

  if (clusters < node->allocsize >> vol.cluster_shift)
    {
      off_t allocated = (off_t) clusters << vol.cluster_shift;
      struct exfat_dir dir;

      if (directory && !clusters && !st->st_nlink
	  && !exfat_dir_open (&dir, &dn->stream, false))
	{
	  /* The directory is being deleted, and nothing can reach it
	     any more to change its entries.  */
	  exfat_dir_keep_benign (&dir);
	  exfat_dir_close (&dir);
	}

      diskfs_set_node_times (node);
      err = exfat_node_write_set (node, directory ? allocated : st->st_size);
      if (!err)
	err = exfat_store_flush ();
      if (!err)
	{
	  /* What a failed shrink freed is freed all the same.  */
	  err = exfat_stream_shrink (&dn->stream, clusters);
	  node->allocsize = dn->stream.length;
	  st->st_blocks = node->allocsize >> 9;
	  if (directory && !node->allocsize)
	    exfat_inode_set_dir_cluster (node->cache_id, 0);
	}
    }
  pthread_rwlock_unlock (&dn->alloc_lock);

  /* A page past the end that the kernel holds is not part of the file
     any more, and would show what it holds if the file grew again.
     Pages are read in without the node's lock, so one may have been
     read with the old ValidDataLength until it was cut above.  The
     discard waits for pages that are being read in, and so for the
     allocation lock, which is why it comes after.  */
  if (page_end < had)
    discard_node_pages (node, page_end, had - page_end);
  return err;
}

/* Allocate a new node to be of mode MODE in locked directory DIR; set
   *NODE to it, locked.  It gets its entry set when it is entered in a
   directory.  A directory gets its first cluster here.  */
error_t
diskfs_alloc_node (struct node *dir, mode_t mode, struct node **node)
{
  error_t err;
  struct lookup_context ctx = { mode };
  struct node *np;
  ino_t ino;

  err = exfat_inode_new (dir->cache_id, &ino);
  if (err)
    return err;
  err = diskfs_cached_lookup_context (ino, &np, &ctx);
  if (err)
    {
      exfat_inode_forget (ino);
      return err;
    }
  if (S_ISDIR (mode))
    {
      err = exfat_node_grow (np, CLUSTER_SIZE);
      if (err)
	{
	  np->dn_stat.st_nlink = 0;
	  diskfs_nput (np);
	  return err;
	}
    }
  *node = np;
  return 0;
}

/* Free node NP, which has no entry set and no clusters any more.  */
void
diskfs_free_node (struct node *np, mode_t old_mode)
{
  exfat_inode_forget (np->cache_id);
}

/* libdiskfs's definition of diskfs_create_node comes with this.  */
int _diskfs_no_inherit_dir_group;

/* Create a new node in DIR under NAME, as described in
   <hurd/diskfs.h>.  libdiskfs's own version links `.' and `..' into a
   new directory, which the volume does not record, and gives a file
   the owner and the mode it was asked for, which the volume cannot
   record either.  */
error_t
diskfs_create_node (struct node *dir, const char *name, mode_t mode,
		    struct node **newnode, struct protid *cred,
		    struct dirstat *ds)
{
  error_t err;
  struct node *np;

  *newnode = NULL;
  if (diskfs_check_readonly ())
    err = EROFS;
  else if (!name || (!S_ISDIR (mode) && !S_ISREG (mode)))
    /* A file exists by its entry set, which has to have a name, and
       is a directory or a regular file.  */
    err = EOPNOTSUPP;
  else
    err = diskfs_alloc_node (dir, mode, &np);
  if (err)
    {
      if (name)
	diskfs_drop_dirstat (dir, ds);
      return err;
    }

  err = diskfs_direnter (dir, name, np, ds, cred);
  if (err)
    {
      np->dn_stat.st_nlink = 0;
      diskfs_nput (np);
      return err;
    }
  if (S_ISDIR (mode))
    {
      /* For the `..' of the new directory.  */
      dir->dn_stat.st_nlink++;
      dir->dn_set_ctime = 1;
    }
  *newnode = np;
  return 0;
}

/* A file is its entry set, and has one name.  */
kern_return_t
diskfs_S_dir_link (struct protid *dircred, struct protid *filecred,
		   const_string_t name, int excl)
{
  return dircred ? EPERM : EOPNOTSUPP;
}

/* The owner and the group of every file are those the translator was
   started with or told with --uid and --gid.  */

error_t
diskfs_validate_owner_change (struct node *np, uid_t uid)
{
  return uid == np->dn_stat.st_uid ? 0 : EPERM;
}

error_t
diskfs_validate_group_change (struct node *np, gid_t gid)
{
  return gid == np->dn_stat.st_gid ? 0 : EPERM;
}

/* Of a mode, the volume records whether a file can be written.  */
error_t
diskfs_validate_mode_change (struct node *np, mode_t mode)
{
  return 0;
}

error_t
diskfs_validate_author_change (struct node *np, uid_t author)
{
  return author == np->dn_stat.st_author ? 0 : EPERM;
}

error_t
diskfs_validate_flags_change (struct node *np, int flags)
{
  return flags ? EOPNOTSUPP : 0;
}

error_t
diskfs_set_translator (struct node *node, const char *name,
		       mach_msg_type_number_t namelen, struct protid *cred)
{
  return EOPNOTSUPP;
}

error_t
diskfs_get_translator (struct node *node, char **namep,
		       mach_msg_type_number_t *namelen)
{
  return EOPNOTSUPP;
}

void
diskfs_shutdown_soft_ports (void)
{
}

kern_return_t
diskfs_S_file_get_storage_info (struct protid *cred,
				mach_port_t **ports,
				mach_msg_type_name_t *ports_type,
				mach_msg_type_number_t *num_ports,
				int **ints, mach_msg_type_number_t *num_ints,
				loff_t **offsets,
				mach_msg_type_number_t *num_offsets,
				data_t *data, mach_msg_type_number_t *data_len)
{
  return EOPNOTSUPP;
}
