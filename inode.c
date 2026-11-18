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
#include <hurd/ihash.h>
#include "exfatfs.h"
#include "libdiskfs/fs_S.h"

/* The directory each node was found in, by inode number.  With this a
   node is read from the volume by its inode number alone, which is the
   place of its entry set, whether a lookup just found it or libdiskfs
   asks for it again later.  The first directory to list an entry keeps
   it, and a node is recorded here before it can be searched itself, so
   the links cannot form a loop.  */
struct parent_link
{
  ino_t child;
  ino_t parent;
  bool parent_contiguous;
};

static hurd_ihash_key_t
hash_inum (const void *key)
{
  uint64_t i = *(const ino_t *) key;

  /* The mix function of fasthash, as in libdiskfs's node cache.  */
  i ^= i >> 23;
  i *= 0x2127599bf4325c37ULL;
  i ^= i >> 47;
  return (hurd_ihash_key_t) i;
}

static int
compare_inum (const void *a, const void *b)
{
  return *(const ino_t *) a == *(const ino_t *) b;
}

static struct hurd_ihash parents
  = HURD_IHASH_INITIALIZER_GKI (HURD_IHASH_NO_LOCP, NULL, NULL,
				hash_inum, compare_inum);
static pthread_mutex_t parents_lock = PTHREAD_MUTEX_INITIALIZER;

/* Remember that the entry set of CHILD is in directory PARENT, unless a
   directory is already known for it.  */
error_t
exfat_record_parent (ino_t child, ino_t parent, bool parent_contiguous)
{
  error_t err = 0;

  pthread_mutex_lock (&parents_lock);
  if (!hurd_ihash_find (&parents, (hurd_ihash_key_t) &child))
    {
      struct parent_link *link = malloc (sizeof *link);

      if (!link)
	err = ENOMEM;
      else
	{
	  *link = (struct parent_link) { child, parent, parent_contiguous };
	  err = hurd_ihash_add (&parents, (hurd_ihash_key_t) &link->child,
				link);
	  if (err)
	    free (link);
	}
    }
  pthread_mutex_unlock (&parents_lock);
  return err;
}

static bool
find_parent (ino_t child, struct parent_link *link)
{
  struct parent_link *found;

  pthread_mutex_lock (&parents_lock);
  found = hurd_ihash_find (&parents, (hurd_ihash_key_t) &child);
  if (found)
    *link = *found;
  pthread_mutex_unlock (&parents_lock);
  return found != NULL;
}


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
  *npp = np;
  return 0;
}

/* The user must define this function if she wants to use the node
   cache.  Read stat information out of the on-disk node.  */
error_t
diskfs_user_read_node (struct node *np, struct lookup_context *ctx)
{
  error_t err;
  struct disknode *dn = np->dn;
  struct stat *st = &np->dn_stat;

  if (np->cache_id == EXFAT_ROOT_INO)
    {
      cluster_t clusters;

      err = exfat_chain_length (vol.root_cluster, &clusters);
      if (err)
	return err;
      exfat_stream_init (&dn->stream, vol.root_cluster, false,
			 (off_t) clusters << vol.cluster_shift);
      dn->valid_length = dn->stream.length;
      dn->attributes = EXFAT_ATTR_DIRECTORY;
      dn->parent = EXFAT_ROOT_INO;
    }
  else
    {
      struct parent_link link;
      struct exfat_set *set;

      if (!find_parent (np->cache_id, &link))
	return ESTALE;
      set = malloc (sizeof *set);
      if (!set)
	return ENOMEM;
      err = exfat_set_read_at ((off_t) np->cache_id * EXFAT_ENTRY_SIZE,
			       link.parent_contiguous, set);
      if (err)
	{
	  free (set);
	  return err;
	}

      dn->parent = link.parent;
      dn->parent_contiguous = link.parent_contiguous;
      dn->set_offset = set->offset;
      exfat_stream_init (&dn->stream, set->first_cluster, set->contiguous,
			 set->data_length);
      dn->valid_length = set->valid_length;
      dn->attributes = set->attributes;
      dn->create_time = set->create_time;
      dn->modify_time = set->modify_time;
      dn->access_time = set->access_time;
      free (set);
    }

  dn->readdir_index = 0;
  dn->readdir_pos = 0;

  st->st_fstype = FSTYPE_MSLOSS;
  st->st_fsid = getpid ();
  st->st_ino = np->cache_id;
  st->st_gen = 0;
  st->st_rdev = 0;
  st->st_nlink = 1;
  st->st_uid = fs_uid;
  st->st_gid = fs_gid;
  st->st_author = fs_uid;
  st->st_flags = 0;

  if (dn->attributes & EXFAT_ATTR_DIRECTORY)
    st->st_mode = S_IFDIR | 0755;
  else
    st->st_mode = S_IFREG | 0644;
  if (dn->attributes & EXFAT_ATTR_READONLY)
    st->st_mode &= ~0222;

  st->st_size = dn->stream.length;
  np->allocsize = round_cluster (dn->stream.length);
  st->st_blksize = vm_page_size;
  st->st_blocks = np->allocsize >> 9;

  /* The volume records when a file was created, not when its status
     last changed; the last modification is the closest to that.  */
  st->st_mtim = dn->modify_time;
  st->st_ctim = dn->modify_time;
  st->st_atim = dn->access_time;

  return 0;
}

/* The last reference to a node has gone away; drop it from the hash
   table and clean all state in the dn structure.  */
void
diskfs_node_norefs (struct node *np)
{
  assert_backtrace (!np->dn->pager);
  exfat_stream_fini (&np->dn->stream);
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
  flush_node_pager (node);
  exfat_stream_fini (&node->dn->stream);
  return diskfs_user_read_node (node, NULL);
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


/* Nothing below can succeed while the file system is read-only, and
   libdiskfs refuses most of it before it gets here.  */

error_t
diskfs_validate_owner_change (struct node *np, uid_t uid)
{
  return EROFS;
}

error_t
diskfs_validate_group_change (struct node *np, gid_t gid)
{
  return EROFS;
}

error_t
diskfs_validate_mode_change (struct node *np, mode_t mode)
{
  return EROFS;
}

error_t
diskfs_validate_author_change (struct node *np, uid_t author)
{
  return EROFS;
}

error_t
diskfs_validate_flags_change (struct node *np, int flags)
{
  return EROFS;
}

void
diskfs_write_disknode (struct node *np, int wait)
{
}

error_t
diskfs_set_translator (struct node *node, const char *name,
		       mach_msg_type_number_t namelen, struct protid *cred)
{
  return EROFS;
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

error_t
diskfs_truncate (struct node *node, loff_t length)
{
  return EROFS;
}

error_t
diskfs_alloc_node (struct node *dir, mode_t mode, struct node **node)
{
  return EROFS;
}

void
diskfs_free_node (struct node *np, mode_t old_mode)
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
