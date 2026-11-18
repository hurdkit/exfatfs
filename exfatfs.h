/* exfatfs.h - Interface for exfatfs.

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

#ifndef EXFATFS_H
#define EXFATFS_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <hurd/diskfs.h>
#include <hurd/diskfs-pager.h>
#include <hurd/store.h>

typedef uint32_t cluster_t;

#define EXFAT_FIRST_CLUSTER	2
#define EXFAT_EOC		0xffffffffU
#define EXFAT_MAX_CLUSTERS	0xfffffff5U

#define EXFAT_ENTRY_SIZE	32
#define EXFAT_SET_MAX		256	/* A primary and 255 secondaries.  */
#define EXFAT_NAME_MAX		255	/* In UTF-16 code units.  */
#define EXFAT_UTF8_NAME_MAX	(EXFAT_NAME_MAX * 3)
/* The longest name a directory listing can carry, in bytes: d_namlen
   in struct dirent is an unsigned char.  */
#define EXFAT_LISTED_NAME_MAX	255
#define EXFAT_DIR_SIZE_MAX	(256 << 20)

#define EXFAT_TYPE_IN_USE	0x80
#define EXFAT_TYPE_SECONDARY	0x40
#define EXFAT_TYPE_BENIGN	0x20
#define EXFAT_TYPE_BITMAP	0x81
#define EXFAT_TYPE_UPCASE	0x82
#define EXFAT_TYPE_LABEL	0x83
#define EXFAT_TYPE_FILE		0x85
#define EXFAT_TYPE_STREAM	0xc0
#define EXFAT_TYPE_NAME		0xc1
#define EXFAT_TYPE_VENDOR_EXTENSION	0xe0

#define EXFAT_ATTR_READONLY	0x01
#define EXFAT_ATTR_DIRECTORY	0x10
#define EXFAT_ATTR_ARCHIVE	0x20

/* The inode number of the root directory.  The other files are
   numbered from the next one up.  */
#define EXFAT_ROOT_INO		1

/* The parameters of the volume, parsed and validated from the main boot
   region when the volume is mounted.  */
struct exfat_volume
{
  unsigned sector_shift;
  unsigned cluster_shift;
  off_t fat_offset;		/* Byte offset of the active FAT.  */
  off_t fat_size;		/* Its size in bytes.  */
  off_t heap_offset;		/* Byte offset of cluster 2.  */
  cluster_t cluster_count;
  cluster_t root_cluster;
  unsigned active_fat;		/* Selects the FAT and the bitmap.  */
  unsigned fat_count;
  uint32_t serial_number;
};

extern struct exfat_volume vol;

#define SECTOR_SIZE	((size_t) 1 << vol.sector_shift)
#define CLUSTER_SIZE	((off_t) 1 << vol.cluster_shift)

static inline uint16_t
get_le16 (const uint8_t *p)
{
  return p[0] | p[1] << 8;
}

static inline uint32_t
get_le32 (const uint8_t *p)
{
  return get_le16 (p) | (uint32_t) get_le16 (p + 2) << 16;
}

static inline uint64_t
get_le64 (const uint8_t *p)
{
  return get_le32 (p) | (uint64_t) get_le32 (p + 4) << 32;
}

static inline void
put_le16 (uint8_t *p, uint16_t value)
{
  p[0] = value;
  p[1] = value >> 8;
}

static inline void
put_le32 (uint8_t *p, uint32_t value)
{
  put_le16 (p, value);
  put_le16 (p + 2, value >> 16);
}

static inline void
put_le64 (uint8_t *p, uint64_t value)
{
  put_le32 (p, value);
  put_le32 (p + 4, value >> 32);
}

static inline bool
cluster_valid (cluster_t cluster)
{
  return cluster >= EXFAT_FIRST_CLUSTER
    && cluster - EXFAT_FIRST_CLUSTER < vol.cluster_count;
}

static inline off_t
cluster_offset (cluster_t cluster)
{
  return vol.heap_offset
    + ((off_t) (cluster - EXFAT_FIRST_CLUSTER) << vol.cluster_shift);
}

static inline off_t
round_cluster (off_t length)
{
  return (length + CLUSTER_SIZE - 1) & ~(CLUSTER_SIZE - 1);
}

/* A run of consecutive clusters: clusters INDEX to INDEX + COUNT - 1 of
   a stream are clusters START to START + COUNT - 1 of the volume.  */
struct exfat_extent
{
  cluster_t index;
  cluster_t start;
  cluster_t count;
};

/* The allocation of a file, a directory or a system table.  */
struct exfat_stream
{
  /* LOCK protects everything else here.  Whoever holds the lock of
     the node the stream belongs to may read what describes the
     allocation without it.  */
  pthread_mutex_t lock;
  cluster_t first_cluster;
  bool contiguous;		/* NoFatChain: the FAT does not describe it.  */
  /* The clusters allocated are those that LENGTH bytes take.  For a
     node this is a whole number of clusters.  */
  off_t length;

  /* The part of the FAT chain walked so far, as extents covering
     clusters 0 to MAPPED - 1.  Unused when CONTIGUOUS.  */
  struct exfat_extent *extents;
  size_t extent_count;
  size_t extents_allocated;
  cluster_t mapped;
};

/* A File directory entry set, as read from a directory.  */
struct exfat_set
{
  off_t offset;			/* Byte offset on the volume of the File entry.  */
  unsigned count;		/* Entries in the set.  */
  uint8_t raw[EXFAT_SET_MAX * EXFAT_ENTRY_SIZE];

  /* Set once the structure of the set has been checked.  */
  bool named;
  uint16_t name[EXFAT_NAME_MAX];
  unsigned name_length;

  /* EIO when the set failed validation; the fields below are then
     undefined.  */
  error_t error;
  uint16_t name_hash;
  uint16_t attributes;
  cluster_t first_cluster;
  bool contiguous;
  off_t data_length;
  off_t valid_length;
  struct timespec create_time;
  struct timespec modify_time;
  struct timespec access_time;
};

/* A change to the entries of one set, or to entries that are to hold
   one: where they are on the volume, what they hold and what they are
   to hold.  All that can fail in finding that out does so before the
   first of the changes that go together is written.  */
struct exfat_set_change
{
  unsigned count;
  off_t at[EXFAT_SET_MAX];
  uint8_t before[EXFAT_SET_MAX * EXFAT_ENTRY_SIZE];
  uint8_t after[EXFAT_SET_MAX * EXFAT_ENTRY_SIZE];
};

/* An allocation that a directory entry names.  */
struct exfat_alloc
{
  cluster_t first;
  bool contiguous;
  uint64_t length;
};

/* What the entry set of a file says about it, apart from its name.  */
struct exfat_meta
{
  uint16_t attributes;
  cluster_t first_cluster;
  bool contiguous;
  off_t data_length;
  off_t valid_length;
  struct timespec create_time;
  struct timespec modify_time;
  struct timespec access_time;
};

/* A position in a directory.  */
struct exfat_dir
{
  struct exfat_stream *stream;
  bool root;
  off_t pos;			/* Offset of the next entry to examine.  */

  uint8_t *window;
  off_t window_start;
  size_t window_length;
};

/* A name in UTF-16, as it is and prepared for comparison with the
   names in a directory.  */
struct exfat_name_key
{
  uint16_t name[EXFAT_NAME_MAX];
  uint16_t upcased[EXFAT_NAME_MAX];
  unsigned length;
  uint16_t hash;
};

/* Where the entry set of a file is.  */
struct exfat_location
{
  /* Volume offset of its File entry, or zero for a file that has no
     entry set: one not entered in a directory yet, or removed from
     its directory and still open.  */
  off_t set_offset;
  ino_t parent;			/* The directory that holds it.  */
  /* Whether that directory is contiguous, which is what it takes to
     follow the set from SET_OFFSET across a cluster boundary.  */
  bool parent_contiguous;
};

/* What is here is read from the node's entry set.  Where that set is,
   exfat_inode_location says.  */
struct disknode
{
  struct exfat_stream stream;
  /* Held for writing while the allocation changes, and for reading by
     the pager, which uses the allocation without the node's lock.  */
  pthread_rwlock_t alloc_lock;

  /* ValidDataLength: the disk holds the file's data up to here, and
     what follows reads as zeros.  The pager moves it as it writes
     pages out, under VALID_LOCK.  */
  pthread_mutex_t valid_lock;
  off_t valid_length;

  uint16_t attributes;
  struct timespec create_time;

  struct pager *pager;

  /* The directory offset at which entry set number READDIR_INDEX
     starts, so that a listing read in pieces is not rescanned from
     the start for each piece, and the number of sets before it that
     the listing leaves out for their long names.  */
  int readdir_index;
  off_t readdir_pos;
  int readdir_hidden;

  /* Set once the directory is found to hold entries that are past its
     end, as section 6.2.1 puts it, and not of type 00h.  Its entries
     are not changed from then on.  */
  bool entries_hidden;

  /* Every disknode is on one list, so that what the nodes hold can be
     looked through without their locks.  */
  struct disknode *next_node, **prev_node;
};

/* Given to diskfs_cached_lookup_context for a node that is not on the
   volume yet.  */
struct lookup_context
{
  mode_t mode;
};

struct user_pager_info
{
  struct node *node;
  vm_prot_t max_prot;
};

extern struct store *store;

extern uid_t fs_uid;
extern gid_t fs_gid;

/* Set by --allow-dirty: a volume that was not unmounted cleanly may
   be written all the same.  */
extern bool exfat_allow_dirty;

/* The up-case table of the volume, one entry per UTF-16 code unit.  */
extern uint16_t *exfat_upcase;

extern cluster_t exfat_free_clusters;

/* Held for reading while directory entries are read and for writing
   while they change.  A node's entry set is written under the node's
   lock, not under that of the directory that holds it, so the lock of
   a directory does not keep its entries still.  */
extern pthread_rwlock_t exfat_entry_lock;

/* volume.c */
void exfat_mount (void);
void exfat_volume_allow_dirty (void);
void exfat_volume_damaged (void);
void exfat_volume_failed (void);
error_t exfat_disk_read (off_t offset, void *buf, size_t len);
error_t exfat_disk_write (off_t offset, const void *buf, size_t len);
error_t exfat_disk_zero (off_t offset, off_t len);
error_t exfat_store_flush (void);
error_t exfat_volume_mark (bool in_use, bool leaking);
error_t exfat_fat_next (cluster_t cluster, cluster_t *next);
error_t exfat_fat_set_run (cluster_t first, cluster_t count, cluster_t next);
void exfat_fat_forget (void);

/* alloc.c */
error_t exfat_bitmap_load (cluster_t first, uint64_t length);
error_t exfat_bitmap_reload (void);
error_t exfat_clusters_alloc (cluster_t hint, cluster_t want,
			      cluster_t *start, cluster_t *count);
error_t exfat_clusters_free (cluster_t start, cluster_t count);
bool exfat_clusters_allocated (cluster_t start, cluster_t count);
uint64_t exfat_free_generation (void);
error_t exfat_clusters_free_unchanged (const struct exfat_extent *extents,
				       size_t count, uint64_t *generation);

/* map.c */
bool exfat_allocation_valid (cluster_t first, bool contiguous, uint64_t length);
error_t exfat_chain_length (cluster_t first, cluster_t *count);
void exfat_stream_init (struct exfat_stream *stream, cluster_t first,
			bool contiguous, off_t length);
void exfat_stream_replace (struct exfat_stream *stream, cluster_t first,
			   bool contiguous, off_t length);
void exfat_stream_fini (struct exfat_stream *stream);
error_t exfat_stream_map (struct exfat_stream *stream, off_t offset,
			  off_t *disk_offset, off_t *run);
error_t exfat_stream_check (struct exfat_stream *stream);
error_t exfat_stream_read (struct exfat_stream *stream, off_t offset,
			   void *buf, size_t len);
error_t exfat_stream_write (struct exfat_stream *stream, off_t offset,
			    const void *buf, size_t len);
error_t exfat_stream_zero (struct exfat_stream *stream, off_t offset,
			   off_t len);
error_t exfat_stream_grow (struct exfat_stream *stream, cluster_t clusters,
			   bool zero);
error_t exfat_stream_shrink (struct exfat_stream *stream,
			     cluster_t clusters);
error_t exfat_stream_meets (struct exfat_stream *stream, cluster_t start,
			    cluster_t count, bool *meets);

/* entry.c */
error_t exfat_dir_open (struct exfat_dir *dir, struct exfat_stream *stream,
			bool root);
void exfat_dir_close (struct exfat_dir *dir);
error_t exfat_dir_next_primary (struct exfat_dir *dir, const uint8_t **entry);
error_t exfat_dir_next_set (struct exfat_dir *dir, struct exfat_set *set,
			    bool *found);
error_t exfat_set_read_at (off_t offset, bool contiguous,
			   struct exfat_set *set);
error_t exfat_dir_find_free (struct exfat_dir *dir, unsigned count,
			     off_t *pos, bool *hidden);
error_t exfat_set_write_at (off_t offset, bool contiguous,
			    const struct exfat_set *set);
error_t exfat_set_change_at (struct exfat_set_change *change, off_t offset,
			     bool contiguous, unsigned count);
void exfat_set_change_follow (struct exfat_set_change *change,
			      const struct exfat_set_change *previous,
			      unsigned count);
void exfat_set_change_put (struct exfat_set_change *change,
			   const struct exfat_set *set);
bool exfat_set_change_in_one_piece (const struct exfat_set_change *change);
bool exfat_set_change_into_unused (const struct exfat_set_change *change);
error_t exfat_set_changes_apply (struct exfat_set_change *const *changes,
				 unsigned count);
error_t exfat_set_delete_at (off_t offset, bool contiguous, uint8_t *raw,
			     unsigned *count);
void exfat_set_free_unknown (const uint8_t *raw, unsigned count);
void exfat_dir_free_benign (struct exfat_dir *dir);
void exfat_set_create (struct exfat_set *set,
		       const struct exfat_name_key *name,
		       const struct exfat_meta *meta);
bool exfat_set_differs (const struct exfat_set *set,
			const struct exfat_meta *meta);
void exfat_set_update (struct exfat_set *set, const struct exfat_meta *meta);
error_t exfat_set_rename (struct exfat_set *set,
			  const struct exfat_name_key *name);
bool exfat_name_key (const char *name, struct exfat_name_key *key);
void exfat_name_key_of_set (const struct exfat_set *set,
			    struct exfat_name_key *key);
bool exfat_name_creatable (const struct exfat_name_key *key);
bool exfat_name_exact (const struct exfat_set *set,
		       const struct exfat_name_key *key);
bool exfat_name_match (const struct exfat_set *set,
		       const struct exfat_name_key *key);
size_t exfat_name_to_utf8 (const uint16_t *name, unsigned length, char *out);

/* vinode.c */
error_t exfat_inode_get (const struct exfat_location *where,
			 cluster_t dir_cluster, ino_t *ino);
error_t exfat_inode_new (ino_t parent, ino_t *ino);
bool exfat_inode_location (ino_t ino, struct exfat_location *where);
bool exfat_inode_is_ancestor (ino_t ino, ino_t of);
void exfat_inode_move (ino_t ino, const struct exfat_location *where);
void exfat_inode_detach (ino_t ino);
error_t exfat_inode_set_dir_cluster (ino_t ino, cluster_t dir_cluster);
void exfat_inode_forget (ino_t ino);

/* dir.c */
void exfat_rename_forget (ino_t ino);

/* inode.c */
void refresh_node_stats (void);
error_t exfat_node_grow (struct node *np, off_t size);
void exfat_node_modified (struct node *np);
error_t exfat_node_write_set (struct node *np, off_t data_length);
void exfat_node_meta (struct node *np, struct exfat_meta *meta);
void exfat_write_all_nodes (void);
bool exfat_orphans_hold_clusters (void);
error_t exfat_nodes_meet (cluster_t start, cluster_t count, bool *meets);

/* unknown.c */
void exfat_free_unknown (const struct exfat_alloc *allocs,
			 const bool *known, unsigned count);

/* pager.c */
void create_file_pagers (void);
void drop_pager_softrefs (struct node *node);
void allow_pager_softrefs (struct node *node);
void flush_node_pager (struct node *node);
void discard_node_pages (struct node *node, off_t offset, off_t len);

#endif /* EXFATFS_H */
