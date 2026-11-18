/* unknown.c - Freeing what directory entries of unknown kinds hold.

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
#include "exfatfs.h"

/* Section 8.2 has the allocations of entries that are not recognized
   freed along with their sets.  Only the entry says that the clusters
   it names are its own, and on a damaged or crafted volume they may be
   a file's, or the allocation bitmap's, while fsck.exfat finds nothing
   wrong.  So they are freed only if no other entry on the volume names
   any of them and no node here has any of them, and are left allocated
   otherwise: that loses nothing.  */

struct candidate
{
  struct exfat_stream stream;
  /* Its clusters, one run if the allocation is contiguous.  */
  const struct exfat_extent *extents;
  size_t extent_count;
  struct exfat_extent run;
  bool kept;
};

/* What the walk of the volume looks for, and the directories it is to
   read.  */
struct scan
{
  struct candidate *candidates;
  unsigned count;
  struct exfat_alloc *dirs;
  size_t dir_count, dirs_allocated;
  cluster_t dirs_read;
};

/* Note that what the COUNT clusters from START hold is not to be
   freed, if a candidate has one of them.  */
static error_t
keep_meeting (struct candidate *candidates, unsigned count,
	      error_t (*meets) (void *, cluster_t, cluster_t, bool *),
	      void *arg)
{
  for (unsigned i = 0; i < count; i++)
    for (size_t j = 0; !candidates[i].kept && j < candidates[i].extent_count;
	 j++)
      {
	const struct exfat_extent *e = &candidates[i].extents[j];
	bool hit;
	error_t err = meets (arg, e->start, e->count, &hit);

	if (err)
	  return err;
	candidates[i].kept = hit;
      }
  return 0;
}

static error_t
stream_meets (void *stream, cluster_t start, cluster_t count, bool *meets)
{
  return exfat_stream_meets (stream, start, count, meets);
}

static error_t
nodes_meet (void *unused, cluster_t start, cluster_t count, bool *meets)
{
  return exfat_nodes_meet (start, count, meets);
}

/* Keep the candidates of SCAN that have a cluster of the allocation
   that an entry on the volume names.  */
static error_t
claim (struct scan *scan, cluster_t first, bool contiguous, uint64_t length)
{
  error_t err;
  struct exfat_stream stream;

  if (first == 0 || length == 0)
    return 0;
  if (!exfat_allocation_valid (first, contiguous, length))
    return EIO;
  exfat_stream_init (&stream, first, contiguous, length);
  err = keep_meeting (scan->candidates, scan->count, stream_meets, &stream);
  exfat_stream_fini (&stream);
  return err;
}

static error_t
add_dir (struct scan *scan, cluster_t first, bool contiguous,
	 uint64_t length)
{
  if (length == 0)
    return 0;
  if (length % CLUSTER_SIZE != 0 || length > EXFAT_DIR_SIZE_MAX
      /* More directories than clusters: they are linked in a loop.  */
      || scan->dirs_read + scan->dir_count >= vol.cluster_count)
    return EIO;
  if (scan->dir_count == scan->dirs_allocated)
    {
      size_t n = scan->dirs_allocated * 2 ?: 16;
      struct exfat_alloc *dirs = realloc (scan->dirs, n * sizeof *dirs);

      if (!dirs)
	return ENOMEM;
      scan->dirs = dirs;
      scan->dirs_allocated = n;
    }
  scan->dirs[scan->dir_count++]
    = (struct exfat_alloc) { first, contiguous, length };
  return 0;
}

/* Look at ENTRY, which is in use.  *FILE is the attributes of the File
   entry right before it plus one, or zero if there is none.  Every
   entry that may name an allocation counts, whether a set holds it or
   not, and so do those past the end of the directory: nothing here
   frees what one of them may still say is its own.  */
static error_t
scan_entry (struct scan *scan, const uint8_t *entry, unsigned *file)
{
  uint8_t type = entry[0];
  unsigned after_file = *file;
  unsigned flags;
  cluster_t first = get_le32 (entry + 20);
  uint64_t length = get_le64 (entry + 24);
  error_t err;

  *file = 0;
  switch (type)
    {
    case EXFAT_TYPE_FILE:
      *file = 1 + get_le16 (entry + 4);
      return 0;

    case EXFAT_TYPE_STREAM:
      err = claim (scan, first, entry[1] & 2, length);
      if (!err && after_file && ((after_file - 1) & EXFAT_ATTR_DIRECTORY))
	err = add_dir (scan, first, entry[1] & 2, length);
      return err;

    case EXFAT_TYPE_BITMAP:
    case EXFAT_TYPE_UPCASE:
      return claim (scan, first, false, length);

    case EXFAT_TYPE_LABEL:
    case EXFAT_TYPE_NAME:
      return 0;
    }

  flags = type & EXFAT_TYPE_SECONDARY ? entry[1] : get_le16 (entry + 4);
  /* AllocationPossible, and NoFatChain.  */
  if (!(flags & 1))
    return 0;
  return claim (scan, first, flags & 2, length);
}

static error_t
scan_dir (struct scan *scan, const struct exfat_alloc *where)
{
  error_t err = 0;
  struct exfat_stream stream;
  size_t chunk = CLUSTER_SIZE < 65536 ? CLUSTER_SIZE : 65536;
  uint8_t *buf = malloc (chunk);
  unsigned file = 0;

  if (!buf)
    return ENOMEM;
  exfat_stream_init (&stream, where->first, where->contiguous,
		     where->length);
  for (off_t pos = 0; !err && pos < (off_t) where->length; pos += chunk)
    {
      err = exfat_stream_read (&stream, pos, buf, chunk);
      for (size_t i = 0; !err && i < chunk; i += EXFAT_ENTRY_SIZE)
	if (buf[i] & EXFAT_TYPE_IN_USE)
	  err = scan_entry (scan, buf + i, &file);
	else
	  file = 0;
    }
  exfat_stream_fini (&stream);
  free (buf);
  return err;
}

/* Keep the candidates of SCAN that have a cluster that an entry in a
   directory on the volume names, or the root directory has.  */
static error_t
scan_volume (struct scan *scan)
{
  error_t err;
  cluster_t root_clusters;

  err = exfat_chain_length (vol.root_cluster, &root_clusters);
  if (!err)
    err = claim (scan, vol.root_cluster, false,
		 (uint64_t) root_clusters << vol.cluster_shift);
  if (!err)
    err = add_dir (scan, vol.root_cluster, false,
		   (uint64_t) root_clusters << vol.cluster_shift);
  while (!err && scan->dir_count > 0)
    {
      struct exfat_alloc dir = scan->dirs[--scan->dir_count];

      scan->dirs_read++;
      err = scan_dir (scan, &dir);
    }
  free (scan->dirs);
  return err;
}

/* Set up C for ALLOC, or return false if it cannot be freed whatever
   else names it: it is not a valid allocation, or not a FAT chain that
   ends where its length says.  */
static bool
candidate_init (struct candidate *c, const struct exfat_alloc *alloc)
{
  cluster_t clusters = (alloc->length + CLUSTER_SIZE - 1) >> vol.cluster_shift;
  cluster_t chained;

  if (!exfat_allocation_valid (alloc->first, alloc->contiguous,
			       alloc->length)
      || alloc->first == 0 || alloc->length == 0)
    return false;
  if (!alloc->contiguous
      && (exfat_chain_length (alloc->first, &chained) || chained != clusters))
    return false;

  exfat_stream_init (&c->stream, alloc->first, alloc->contiguous,
		     alloc->length);
  if (alloc->contiguous)
    {
      c->run = (struct exfat_extent) { 0, alloc->first, clusters };
      c->extents = &c->run;
      c->extent_count = 1;
    }
  else if (exfat_stream_check (&c->stream))
    {
      exfat_stream_fini (&c->stream);
      return false;
    }
  else
    {
      c->extents = c->stream.extents;
      c->extent_count = c->stream.extent_count;
    }
  return true;
}

/* Free the COUNT allocations in ALLOCS, which entries of removed sets
   named, where nothing else has any of their clusters.  Those that
   KNOWN says are not of a kind that holds an allocation are kept.
   The caller holds no lock on directory entries.  */
void
exfat_free_unknown (const struct exfat_alloc *allocs, const bool *known,
		    unsigned count)
{
  static bool told;
  struct candidate *candidates = calloc (count, sizeof *candidates);
  struct scan scan = { candidates, count };
  bool kept = !candidates;
  unsigned live = 0;

  for (unsigned i = 0; candidates && i < count; i++)
    {
      candidates[i].kept = !known[i]
	|| !candidate_init (&candidates[i], &allocs[i]);
      if (candidates[i].kept)
	kept = true;
      else
	live++;
    }

  if (live)
    {
      error_t err;
      uint64_t generation;

      /* While this lock is held, no entry set changes.  What a node
	 has that no set records is looked at after the sets, and a
	 cluster freed meanwhile, which the node may now have, shows
	 in the generation.  */
      pthread_rwlock_rdlock (&exfat_entry_lock);
      generation = exfat_free_generation ();
      err = scan_volume (&scan);
      if (!err)
	err = keep_meeting (candidates, count, nodes_meet, NULL);
      /* The set that named them is to be unused on the medium before
	 they are free, as in diskfs_truncate.  */
      if (!err)
	err = exfat_store_flush ();
      for (unsigned i = 0; i < count; i++)
	{
	  if (!candidates[i].kept
	      && (err
		  || exfat_clusters_free_unchanged (candidates[i].extents,
						    candidates[i].extent_count,
						    &generation)))
	    candidates[i].kept = true;
	  if (candidates[i].kept)
	    kept = true;
	}
      pthread_rwlock_unlock (&exfat_entry_lock);
    }

  for (unsigned i = 0; candidates && i < count; i++)
    if (candidates[i].extents)
      exfat_stream_fini (&candidates[i].stream);
  free (candidates);

  if (kept)
    {
      exfat_volume_damaged ();
      if (!told)
	error (0, 0, "%s: clusters that a removed directory entry of a kind"
	       " not known here names are left allocated, since they may be"
	       " in use elsewhere; fsck.exfat should check the volume",
	       diskfs_disk_name);
      told = true;
    }
}
