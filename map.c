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

/* Describe in STREAM an allocation that exfat_allocation_valid
   accepted.  */
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

void
exfat_stream_fini (struct exfat_stream *stream)
{
  free (stream->extents);
  stream->extents = NULL;
  stream->extent_count = stream->extents_allocated = 0;
  stream->mapped = 0;
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
  cluster_t clusters = round_cluster (stream->length) >> vol.cluster_shift;
  cluster_t start = 0, count = 0;

  if (offset < 0 || index >= clusters)
    return EIO;

  if (stream->contiguous)
    {
      start = stream->first_cluster + index;
      count = clusters - index;
    }
  else
    {
      size_t low = 0, high;

      pthread_mutex_lock (&stream->lock);
      err = extend_chain (stream, index);
      if (!err)
	{
	  /* Find the last extent that starts at or before INDEX.  */
	  high = stream->extent_count;
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
      pthread_mutex_unlock (&stream->lock);
      if (err)
	return err;
    }

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
