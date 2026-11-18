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

#define EXFAT_ATTR_READONLY	0x01
#define EXFAT_ATTR_DIRECTORY	0x10

/* No directory entry starts at byte 32 of the volume, so no file can
   have this inode number.  */
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
  cluster_t first_cluster;
  bool contiguous;		/* NoFatChain: the FAT does not describe it.  */
  off_t length;			/* DataLength.  */

  /* The part of the FAT chain walked so far, as extents covering
     clusters 0 to MAPPED - 1.  Unused when CONTIGUOUS.  */
  pthread_mutex_t lock;
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

/* A name prepared for comparison with the names in a directory.  */
struct exfat_name_key
{
  uint16_t upcased[EXFAT_NAME_MAX];
  unsigned length;
  uint16_t hash;
};

struct disknode
{
  struct exfat_stream stream;
  off_t valid_length;		/* ValidDataLength.  */
  uint16_t attributes;
  struct timespec create_time;
  struct timespec modify_time;
  struct timespec access_time;

  /* The directory holding this node's entry set, the volume offset of
     the set's File entry, and whether that directory is contiguous,
     which is what it takes to find the rest of the set from there.
     The inode number is SET_OFFSET / EXFAT_ENTRY_SIZE.  */
  ino_t parent;
  off_t set_offset;
  bool parent_contiguous;

  struct pager *pager;

  /* The directory offset at which entry set number READDIR_INDEX
     starts, so that a listing read in pieces is not rescanned from
     the start for each piece, and the number of sets before it that
     the listing leaves out for their long names.  */
  int readdir_index;
  off_t readdir_pos;
  int readdir_hidden;
};

struct user_pager_info
{
  struct node *node;
  vm_prot_t max_prot;
};

extern struct store *store;

extern uid_t fs_uid;
extern gid_t fs_gid;

/* The up-case table of the volume, one entry per UTF-16 code unit.  */
extern uint16_t *exfat_upcase;

extern cluster_t exfat_free_clusters;

/* volume.c */
void exfat_mount (void);
error_t exfat_disk_read (off_t offset, void *buf, size_t len);
error_t exfat_fat_next (cluster_t cluster, cluster_t *next);

/* map.c */
bool exfat_allocation_valid (cluster_t first, bool contiguous, uint64_t length);
error_t exfat_chain_length (cluster_t first, cluster_t *count);
void exfat_stream_init (struct exfat_stream *stream, cluster_t first,
			bool contiguous, off_t length);
void exfat_stream_fini (struct exfat_stream *stream);
error_t exfat_stream_map (struct exfat_stream *stream, off_t offset,
			  off_t *disk_offset, off_t *run);
error_t exfat_stream_check (struct exfat_stream *stream);
error_t exfat_stream_read (struct exfat_stream *stream, off_t offset,
			   void *buf, size_t len);

/* entry.c */
error_t exfat_dir_open (struct exfat_dir *dir, struct exfat_stream *stream,
			bool root);
void exfat_dir_close (struct exfat_dir *dir);
error_t exfat_dir_next_primary (struct exfat_dir *dir, const uint8_t **entry);
error_t exfat_dir_next_set (struct exfat_dir *dir, struct exfat_set *set,
			    bool *found);
error_t exfat_set_read_at (off_t offset, bool contiguous,
			   struct exfat_set *set);
bool exfat_name_key (const char *name, struct exfat_name_key *key);
bool exfat_name_match (const struct exfat_set *set,
		       const struct exfat_name_key *key);
size_t exfat_name_to_utf8 (const uint16_t *name, unsigned length, char *out);

/* inode.c */
error_t exfat_record_parent (ino_t child, ino_t parent, bool parent_contiguous);
void refresh_node_stats (void);

/* pager.c */
void create_file_pagers (void);
void drop_pager_softrefs (struct node *node);
void allow_pager_softrefs (struct node *node);
void flush_node_pager (struct node *node);

#endif /* EXFATFS_H */
