/* alloc.c - The allocation bitmap.

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

cluster_t exfat_free_clusters;

/* The bitmap as it is on the disk, in whole sectors.  Bit N is set
   when cluster N + EXFAT_FIRST_CLUSTER is allocated.  Every change is
   written to the disk before the lock is released, so the two only
   differ when a write fails.  */
static uint8_t *bitmap;
static size_t bitmap_size;
static struct exfat_stream bitmap_stream;
/* Where the search for free clusters goes on.  Starting after the last
   allocation keeps files written one after the other contiguous.  */
static cluster_t rotor;
/* How many times clusters were freed.  An allocated cluster stays
   with whatever has it until it is freed.  */
static uint64_t free_generation;
static pthread_mutex_t bitmap_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
allocated (cluster_t n)
{
  return bitmap[n / 8] >> (n % 8) & 1;
}

static error_t
read_bitmap (void)
{
  error_t err = exfat_stream_read (&bitmap_stream, 0, bitmap, bitmap_size);
  cluster_t used = 0;

  if (err)
    return err;
  for (cluster_t n = 0; n < vol.cluster_count; n++)
    used += allocated (n);
  exfat_free_clusters = vol.cluster_count - used;
  rotor = 0;
  return 0;
}

/* Read the allocation bitmap, LENGTH bytes that start at cluster FIRST
   and hold a bit for every cluster.  */
error_t
exfat_bitmap_load (cluster_t first, uint64_t length)
{
  size_t bytes = ((size_t) vol.cluster_count + 7) / 8;

  bitmap_size = (bytes + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
  bitmap = malloc (bitmap_size);
  if (!bitmap)
    return ENOMEM;
  exfat_stream_init (&bitmap_stream, first, false, length);
  return read_bitmap ();
}

/* Read the bitmap again, after something else changed the volume.  */
error_t
exfat_bitmap_reload (void)
{
  error_t err;

  pthread_mutex_lock (&bitmap_lock);
  exfat_stream_replace (&bitmap_stream, bitmap_stream.first_cluster, false,
			bitmap_stream.length);
  err = read_bitmap ();
  pthread_mutex_unlock (&bitmap_lock);
  return err;
}

/* Write the sectors that hold bits START to START + COUNT - 1.  */
static error_t
write_bits (cluster_t start, cluster_t count)
{
  size_t low = (start / 8) & ~(SECTOR_SIZE - 1);
  size_t high = ((start + count - 1) / 8 + SECTOR_SIZE) & ~(SECTOR_SIZE - 1);

  return exfat_stream_write (&bitmap_stream, low, bitmap + low, high - low);
}

static void
set_bits (cluster_t start, cluster_t count, bool value)
{
  for (cluster_t n = start; n < start + count; n++)
    if (value)
      bitmap[n / 8] |= 1U << (n % 8);
    else
      bitmap[n / 8] &= ~(1U << (n % 8));
}

/* Allocate up to WANT clusters that follow one another: from HINT on if
   that cluster is free, and otherwise the first free ones found.  Set
   *START to the first and *COUNT to how many there are.  Return ENOSPC
   if no cluster is free.  */
error_t
exfat_clusters_alloc (cluster_t hint, cluster_t want, cluster_t *start,
		      cluster_t *count)
{
  error_t err;
  cluster_t at, n = 0;

  pthread_mutex_lock (&bitmap_lock);
  if (exfat_free_clusters == 0)
    {
      pthread_mutex_unlock (&bitmap_lock);
      return ENOSPC;
    }

  if (cluster_valid (hint) && !allocated (hint - EXFAT_FIRST_CLUSTER))
    at = hint - EXFAT_FIRST_CLUSTER;
  else
    {
      at = rotor;
      while (allocated (at))
	{
	  /* A byte of allocated clusters is the common case.  */
	  if (at % 8 == 0 && bitmap[at / 8] == 0xff)
	    at += 8;
	  else
	    at++;
	  if (at >= vol.cluster_count)
	    at = 0;
	}
    }
  while (n < want && at + n < vol.cluster_count && !allocated (at + n))
    n++;

  set_bits (at, n, true);
  err = write_bits (at, n);
  if (err)
    set_bits (at, n, false);
  else
    {
      exfat_free_clusters -= n;
      rotor = at + n < vol.cluster_count ? at + n : 0;
      *start = at + EXFAT_FIRST_CLUSTER;
      *count = n;
    }
  pthread_mutex_unlock (&bitmap_lock);
  return err;
}

/* Free the COUNT clusters from START, which nothing refers to any
   more.  */
error_t
exfat_clusters_free (cluster_t start, cluster_t count)
{
  error_t err;
  cluster_t at = start - EXFAT_FIRST_CLUSTER;
  cluster_t freed = 0;

  if (count == 0)
    return 0;
  if (!cluster_valid (start) || count > vol.cluster_count - at)
    return EIO;

  pthread_mutex_lock (&bitmap_lock);
  for (cluster_t n = at; n < at + count; n++)
    freed += allocated (n);
  set_bits (at, count, false);
  exfat_free_clusters += freed;
  free_generation++;
  err = write_bits (at, count);
  pthread_mutex_unlock (&bitmap_lock);
  return err;
}

/* Return whether the COUNT clusters from START are all allocated.  */
bool
exfat_clusters_allocated (cluster_t start, cluster_t count)
{
  bool all = cluster_valid (start)
    && count <= vol.cluster_count - (start - EXFAT_FIRST_CLUSTER);

  pthread_mutex_lock (&bitmap_lock);
  for (cluster_t n = 0; all && n < count; n++)
    all = allocated (start - EXFAT_FIRST_CLUSTER + n);
  pthread_mutex_unlock (&bitmap_lock);
  return all;
}

uint64_t
exfat_free_generation (void)
{
  uint64_t generation;

  pthread_mutex_lock (&bitmap_lock);
  generation = free_generation;
  pthread_mutex_unlock (&bitmap_lock);
  return generation;
}

/* Free the clusters of the COUNT extents in EXTENTS, which do not
   overlap, if every one of them is allocated and no cluster was freed
   since exfat_free_generation returned *GENERATION.  Return EAGAIN if
   one was, and EIO if a cluster is not allocated.  *GENERATION then
   counts this one.  */
error_t
exfat_clusters_free_unchanged (const struct exfat_extent *extents,
			       size_t count, uint64_t *generation)
{
  error_t err = 0;

  pthread_mutex_lock (&bitmap_lock);
  if (*generation != free_generation)
    err = EAGAIN;
  for (size_t i = 0; !err && i < count; i++)
    {
      cluster_t at = extents[i].start - EXFAT_FIRST_CLUSTER;

      if (!cluster_valid (extents[i].start)
	  || extents[i].count > vol.cluster_count - at)
	err = EIO;
      for (cluster_t n = at; !err && n < at + extents[i].count; n++)
	if (!allocated (n))
	  err = EIO;
    }
  if (!err)
    {
      for (size_t i = 0; i < count; i++)
	{
	  set_bits (extents[i].start - EXFAT_FIRST_CLUSTER, extents[i].count,
		    false);
	  exfat_free_clusters += extents[i].count;
	}
      *generation = ++free_generation;
      for (size_t i = 0; i < count; i++)
	{
	  error_t e = write_bits (extents[i].start - EXFAT_FIRST_CLUSTER,
				  extents[i].count);

	  if (!err)
	    err = e;
	}
    }
  pthread_mutex_unlock (&bitmap_lock);
  return err;
}
