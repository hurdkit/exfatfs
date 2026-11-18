/* map.c - Mapping file offsets to the disk.

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
#include "exfatfs.h"

/* Return whether an allocation of LENGTH bytes starting at cluster
   FIRST can exist on the volume.  */
bool
exfat_allocation_valid (cluster_t first, bool contiguous, uint64_t length)
{
  uint64_t clusters;

  if (length == 0)
    return true;
  if (length > (uint64_t) vol.cluster_count << vol.cluster_shift)
    return false;
  clusters = (length + CLUSTER_SIZE - 1) >> vol.cluster_shift;

  if (!cluster_valid (first))
    return false;
  if (contiguous
      && clusters > vol.cluster_count - (first - EXFAT_FIRST_CLUSTER))
    return false;
  return true;
}

/* Set *COUNT to the number of clusters in the FAT chain that starts at
   FIRST.  */
error_t
exfat_chain_length (cluster_t first, cluster_t *count)
{
  cluster_t cluster = first;
  cluster_t n = 0;

  while (cluster != EXFAT_EOC)
    {
      error_t err;

      /* A chain cannot be longer than the heap unless it loops.  */
      if (n == vol.cluster_count)
	return EIO;
      err = exfat_fat_next (cluster, &cluster);
      if (err)
	return err;
      n++;
    }
  *count = n;
  return 0;
}

/* Describe in STREAM, which nothing uses yet, an allocation that
   exfat_allocation_valid accepted.  */
void
exfat_stream_init (struct exfat_stream *stream, cluster_t first,
		   bool contiguous, off_t length)
{
  memset (stream, 0, sizeof *stream);
  stream->first_cluster = first;
  stream->contiguous = contiguous;
  stream->length = length;
  pthread_mutex_init (&stream->lock, NULL);
}

/* Make STREAM describe another allocation that exfat_allocation_valid
   accepted.  Other threads may be mapping STREAM meanwhile.  */
void
exfat_stream_replace (struct exfat_stream *stream, cluster_t first,
		      bool contiguous, off_t length)
{
  pthread_mutex_lock (&stream->lock);
  free (stream->extents);
  stream->extents = NULL;
  stream->extent_count = stream->extents_allocated = 0;
  stream->mapped = 0;
  stream->first_cluster = first;
  stream->contiguous = contiguous;
  stream->length = length;
  pthread_mutex_unlock (&stream->lock);
}

/* Release STREAM, which nothing uses any more.  */
void
exfat_stream_fini (struct exfat_stream *stream)
{
  free (stream->extents);
  pthread_mutex_destroy (&stream->lock);
}

/* Walk STREAM's FAT chain until it covers cluster INDEX.  STREAM is
   locked.  */
static error_t
extend_chain (struct exfat_stream *stream, cluster_t index)
{
  while (stream->mapped <= index)
    {
      struct exfat_extent *last = NULL;
      cluster_t next;

      if (stream->extent_count == 0)
	next = stream->first_cluster;
      else
	{
	  error_t err;

	  last = &stream->extents[stream->extent_count - 1];
	  err = exfat_fat_next (last->start + last->count - 1, &next);
	  if (err)
	    return err;
	  if (next == EXFAT_EOC)
	    /* The chain is shorter than the length recorded for it.  */
	    return EIO;
	}

      if (last && next == last->start + last->count)
	last->count++;
      else
	{
	  if (stream->extent_count == stream->extents_allocated)
	    {
	      size_t n = stream->extents_allocated * 2 ?: 8;
	      struct exfat_extent *new
		= realloc (stream->extents, n * sizeof *new);

	      if (!new)
		return ENOMEM;
	      stream->extents = new;
	      stream->extents_allocated = n;
	    }
	  stream->extents[stream->extent_count++]
	    = (struct exfat_extent) { stream->mapped, next, 1 };
	}
      stream->mapped++;
    }
  return 0;
}

/* Find where byte OFFSET of STREAM is on the disk.  Set *DISK_OFFSET to
   its byte offset on the volume and *RUN to the number of bytes that
   follow it there contiguously.  OFFSET must lie inside the stream's
   allocation.  */
error_t
exfat_stream_map (struct exfat_stream *stream, off_t offset,
		  off_t *disk_offset, off_t *run)
{
  error_t err = 0;
  cluster_t index = offset >> vol.cluster_shift;
  off_t within = offset & (CLUSTER_SIZE - 1);
  cluster_t clusters;
  cluster_t start = 0, count = 0;

  pthread_mutex_lock (&stream->lock);
  clusters = round_cluster (stream->length) >> vol.cluster_shift;
  if (offset < 0 || index >= clusters)
    err = EIO;
  else if (stream->contiguous)
    {
      start = stream->first_cluster + index;
      count = clusters - index;
    }
  else
    {
      err = extend_chain (stream, index);
      if (!err)
	{
	  /* Find the last extent that starts at or before INDEX.  */
	  size_t low = 0, high = stream->extent_count;

	  while (high - low > 1)
	    {
	      size_t mid = low + (high - low) / 2;

	      if (stream->extents[mid].index <= index)
		low = mid;
	      else
		high = mid;
	    }
	  start = stream->extents[low].start
	    + (index - stream->extents[low].index);
	  count = stream->extents[low].count
	    - (index - stream->extents[low].index);
	}
    }
  pthread_mutex_unlock (&stream->lock);
  if (err)
    return err;

  *disk_offset = cluster_offset (start) + within;
  *run = ((off_t) count << vol.cluster_shift) - within;
  return 0;
}

/* Walk the whole of STREAM's cluster chain, so that damage to it shows
   up as an error here and not in a page fault later.  */
error_t
exfat_stream_check (struct exfat_stream *stream)
{
  off_t disk_offset, run;

  if (stream->length == 0)
    return 0;
  return exfat_stream_map (stream, stream->length - 1, &disk_offset, &run);
}

/* Read LEN bytes at OFFSET of STREAM into BUF.  OFFSET and LEN must be
   multiples of the sector size, and the range must lie inside the
   stream's allocation.  */
error_t
exfat_stream_read (struct exfat_stream *stream, off_t offset,
		   void *buf, size_t len)
{
  while (len > 0)
    {
      error_t err;
      off_t disk_offset, run;
      size_t chunk;

      err = exfat_stream_map (stream, offset, &disk_offset, &run);
      if (err)
	return err;
      chunk = run < (off_t) len ? (size_t) run : len;
      err = exfat_disk_read (disk_offset, buf, chunk);
      if (err)
	return err;
      offset += chunk;
      buf = (char *) buf + chunk;
      len -= chunk;
    }
  return 0;
}

/* Write LEN bytes from BUF at OFFSET of STREAM, under the conditions of
   exfat_stream_read.  */
error_t
exfat_stream_write (struct exfat_stream *stream, off_t offset,
		    const void *buf, size_t len)
{
  while (len > 0)
    {
      error_t err;
      off_t disk_offset, run;
      size_t chunk;

      err = exfat_stream_map (stream, offset, &disk_offset, &run);
      if (err)
	return err;
      chunk = run < (off_t) len ? (size_t) run : len;
      err = exfat_disk_write (disk_offset, buf, chunk);
      if (err)
	return err;
      offset += chunk;
      buf = (const char *) buf + chunk;
      len -= chunk;
    }
  return 0;
}

/* Write zeros over the LEN bytes at OFFSET of STREAM, which must lie
   inside its allocation and end at a multiple of the sector size.
   OFFSET can be anywhere; what its sector holds before it is kept.  */
error_t
exfat_stream_zero (struct exfat_stream *stream, off_t offset, off_t len)
{
  error_t err;
  size_t head = offset & (SECTOR_SIZE - 1);

  if (head && len > 0)
    {
      uint8_t sector[4096];
      size_t n = SECTOR_SIZE - head;

      err = exfat_stream_read (stream, offset - head, sector, SECTOR_SIZE);
      if (err)
	return err;
      memset (sector + head, 0, SECTOR_SIZE - head);
      err = exfat_stream_write (stream, offset - head, sector, SECTOR_SIZE);
      if (err)
	return err;
      offset += n;
      len -= n;
    }

  while (len > 0)
    {
      off_t disk_offset, run;

      err = exfat_stream_map (stream, offset, &disk_offset, &run);
      if (err)
	return err;
      if (run > len)
	run = len;
      err = exfat_disk_zero (disk_offset, run);
      if (err)
	return err;
      offset += run;
      len -= run;
    }
  return 0;
}

/* The last cluster of STREAM, which has COUNT clusters and is
   locked.  */
static error_t
last_cluster (struct exfat_stream *stream, cluster_t count, cluster_t *last)
{
  if (stream->contiguous)
    *last = stream->first_cluster + count - 1;
  else
    {
      error_t err = extend_chain (stream, count - 1);
      const struct exfat_extent *e;

      if (err)
	return err;
      for (e = &stream->extents[stream->extent_count - 1];
	   e->index > count - 1; e--)
	;
      *last = e->start + (count - 1 - e->index);
    }
  return 0;
}

/* Make the allocation of STREAM CLUSTERS clusters long, if it is
   shorter, writing zeros to the new ones if ZERO.  The caller then
   records the new allocation in whatever names the stream.

   The bitmap is written first, then the FAT entries of the new
   clusters, and last what joins them to the old ones.  If the
   translator dies on the way, clusters are marked allocated that
   nothing uses, and the stream is as it was.  */
error_t
exfat_stream_grow (struct exfat_stream *stream, cluster_t clusters, bool zero)
{
  error_t err = 0;
  struct exfat_extent *runs = NULL;
  size_t run_count = 0, runs_allocated = 0;
  cluster_t have, need, last = 0, hint = 0;

  pthread_mutex_lock (&stream->lock);
  have = round_cluster (stream->length) >> vol.cluster_shift;
  if (clusters <= have)
    {
      pthread_mutex_unlock (&stream->lock);
      return 0;
    }
  need = clusters - have;

  if (need > exfat_free_clusters)
    err = ENOSPC;
  else if (have)
    {
      err = last_cluster (stream, have, &last);
      hint = last + 1;
    }
  while (!err && need)
    {
      cluster_t start, count;

      err = exfat_clusters_alloc (hint, need, &start, &count);
      if (err)
	break;
      if (run_count && runs[run_count - 1].start
	  + runs[run_count - 1].count == start)
	runs[run_count - 1].count += count;
      else
	{
	  if (run_count == runs_allocated)
	    {
	      size_t n = runs_allocated * 2 ?: 8;
	      struct exfat_extent *new = realloc (runs, n * sizeof *new);

	      if (!new)
		{
		  exfat_clusters_free (start, count);
		  err = ENOMEM;
		  break;
		}
	      runs = new;
	      runs_allocated = n;
	    }
	  runs[run_count++] = (struct exfat_extent) { 0, start, count };
	}
      hint = start + count;
      need -= count;
    }

  for (size_t i = 0; !err && zero && i < run_count; i++)
    err = exfat_disk_zero (cluster_offset (runs[i].start),
			   (off_t) runs[i].count << vol.cluster_shift);

  if (!err && run_count == 1
      && (have ? stream->contiguous && runs[0].start == last + 1
	  : true))
    {
      /* One run of clusters: the FAT need not describe it.  */
      if (!have)
	{
	  stream->first_cluster = runs[0].start;
	  stream->contiguous = true;
	}
    }
  else if (!err)
    {
      for (size_t i = 0; !err && i < run_count; i++)
	err = exfat_fat_set_run (runs[i].start, runs[i].count,
				 i + 1 < run_count
				 ? runs[i + 1].start : EXFAT_EOC);
      /* Section 7.6.2.4: once NoFatChain is clear, the FAT has to
	 describe the clusters the stream already had as well.  */
      if (!err && have && stream->contiguous)
	err = exfat_fat_set_run (stream->first_cluster, have, runs[0].start);
      else if (!err && have)
	err = exfat_fat_set_run (last, 1, runs[0].start);
      if (!err)
	{
	  if (!have)
	    stream->first_cluster = runs[0].start;
	  stream->contiguous = false;
	}
    }

  if (err)
    for (size_t i = 0; i < run_count; i++)
      exfat_clusters_free (runs[i].start, runs[i].count);
  else
    stream->length = (off_t) clusters << vol.cluster_shift;
  pthread_mutex_unlock (&stream->lock);
  free (runs);
  return err;
}

/* Make the allocation of STREAM CLUSTERS clusters long, if it is
   longer.  The caller has already recorded the new allocation in
   whatever names the stream, so that if the translator dies here the
   chain is longer than its file needs, or clusters are marked
   allocated that nothing uses, and no file has clusters it lost.  */
error_t
exfat_stream_shrink (struct exfat_stream *stream, cluster_t clusters)
{
  error_t err = 0;
  cluster_t have;

  pthread_mutex_lock (&stream->lock);
  have = round_cluster (stream->length) >> vol.cluster_shift;
  if (clusters >= have)
    {
      pthread_mutex_unlock (&stream->lock);
      return 0;
    }

  if (stream->contiguous)
    err = exfat_clusters_free (stream->first_cluster + clusters,
			       have - clusters);
  else
    {
      cluster_t last;

      /* This walks the chain to its end, so the extents cover it.  */
      err = last_cluster (stream, have, &last);
      if (!err && clusters)
	{
	  err = last_cluster (stream, clusters, &last);
	  if (!err)
	    err = exfat_fat_set_run (last, 1, EXFAT_EOC);
	}
      if (!err)
	{
	  size_t kept = 0;

	  for (size_t i = 0; i < stream->extent_count; i++)
	    {
	      struct exfat_extent *e = &stream->extents[i];
	      cluster_t keep = e->index >= clusters ? 0
		: e->index + e->count <= clusters ? e->count
		: clusters - e->index;

	      if (keep < e->count)
		exfat_clusters_free (e->start + keep, e->count - keep);
	      if (keep)
		{
		  e->count = keep;
		  kept = i + 1;
		}
	    }
	  stream->extent_count = kept;
	  stream->mapped = clusters;
	}
    }

  if (!err)
    {
      stream->length = (off_t) clusters << vol.cluster_shift;
      if (!clusters)
	{
	  stream->first_cluster = 0;
	  stream->contiguous = false;
	}
    }
  pthread_mutex_unlock (&stream->lock);
  return err;
}
