/* volume.c - The boot region, the FAT and the system tables.

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

#include <string.h>
#include <stdlib.h>
#include <error.h>
#include <assert-backtrace.h>
#include <device/device_types.h>
#include "exfatfs.h"

struct exfat_volume vol;
uint16_t *exfat_upcase;
cluster_t exfat_free_clusters;

#define refuse(fmt, ...) \
  error (1, 0, "%s: " fmt, diskfs_disk_name, ##__VA_ARGS__)

/* Read LEN bytes at byte OFFSET of the store into BUF.  Both must be
   multiples of the sector size.  */
error_t
exfat_disk_read (off_t offset, void *buf, size_t len)
{
  error_t err;
  void *data = buf;
  size_t got = len;

  assert_backtrace ((offset & (store->block_size - 1)) == 0);

  err = store_read (store, offset >> store->log2_block_size, len,
		    &data, &got);
  if (err)
    return err;

  if (got != len)
    err = EIO;
  else if (data != buf)
    memcpy (buf, data, len);
  if (data != buf)
    munmap (data, got);
  return err;
}


/* Whether the main boot sector says that the volume is being written
   from here: VolumeDirty is set and PercentInUse is "not available".
   And whether VolumeDirty was set when the volume was mounted.
   Whoever left it set then may have left the volume damaged, which
   nothing here repairs, so in that case it stays set (section
   3.1.13.2).  */
static bool volume_in_use;
static bool volume_dirty_at_mount;
static pthread_mutex_t volume_dirty_lock = PTHREAD_MUTEX_INITIALIZER;

static error_t
raw_write (off_t offset, const void *buf, size_t len)
{
  error_t err;
  size_t wrote;

  assert_backtrace ((offset & (store->block_size - 1)) == 0);

  err = store_write (store, offset >> store->log2_block_size, buf, len,
		     &wrote);
  if (!err && wrote != len)
    err = EIO;
  return err;
}

/* Have the store put what was written to it on its medium, if it can
   be told to.  */
error_t
exfat_store_flush (void)
{
  error_t err = store_sync (store);

  return err == EOPNOTSUPP || err == D_INVALID_OPERATION ? 0 : err;
}

/* Record in the main boot sector that the volume is being written, if
   IN_USE, or that everything written to it is in place.  Section
   3.1.13.2: VolumeDirty is set while the volume may be inconsistent.
   Section 3.1.18: PercentInUse either follows the bitmap or says that
   it does not.  The boot checksum leaves out both fields, so it stays
   right.  */
error_t
exfat_volume_mark (bool in_use)
{
  error_t err = 0;
  uint8_t sector[4096];

  pthread_mutex_lock (&volume_dirty_lock);
  if (in_use != volume_in_use)
    {
      if (!in_use)
	/* What the flag vouches for must be on the medium before it.  */
	err = exfat_store_flush ();
      if (!err)
	err = exfat_disk_read (0, sector, SECTOR_SIZE);
      if (!err)
	{
	  bool dirty = in_use || volume_dirty_at_mount;

	  put_le16 (sector + 106,
		    (get_le16 (sector + 106) & ~2U) | (dirty ? 2 : 0));
	  sector[112] = in_use ? 0xff
	    : (uint64_t) (vol.cluster_count - exfat_free_clusters) * 100
	    / vol.cluster_count;
	  err = raw_write (0, sector, SECTOR_SIZE);
	}
      if (!err)
	{
	  volume_in_use = in_use;
	  err = exfat_store_flush ();
	}
    }
  pthread_mutex_unlock (&volume_dirty_lock);
  return err;
}

/* Write LEN bytes from BUF at byte OFFSET of the store.  Both must be
   multiples of the sector size.  Everything written to the volume
   goes through here, so that VolumeDirty is set before the first
   write.  */
error_t
exfat_disk_write (off_t offset, const void *buf, size_t len)
{
  error_t err = exfat_volume_mark (true);

  if (err)
    return err;
  return raw_write (offset, buf, len);
}

error_t
exfat_disk_zero (off_t offset, off_t len)
{
  static const uint8_t zeros[1 << 16];

  while (len > 0)
    {
      size_t chunk = len < (off_t) sizeof zeros ? (size_t) len : sizeof zeros;
      error_t err = exfat_disk_write (offset, zeros, chunk);

      if (err)
	return err;
      offset += chunk;
      len -= chunk;
    }
  return 0;
}


#define FAT_BLOCK_SIZE		4096
#define FAT_BLOCK_ENTRIES	(FAT_BLOCK_SIZE / sizeof (uint32_t))
#define FAT_CACHE_BLOCKS	64

static struct fat_block
{
  off_t tag;			/* Block number plus one; zero when empty.  */
  uint8_t data[FAT_BLOCK_SIZE];
} fat_cache[FAT_CACHE_BLOCKS];
static pthread_mutex_t fat_cache_lock = PTHREAD_MUTEX_INITIALIZER;

/* Set *NEXT to the FAT entry of CLUSTER: the cluster after it in its
   chain, or EXFAT_EOC.  Any other value is EIO.  */
error_t
exfat_fat_next (cluster_t cluster, cluster_t *next)
{
  error_t err = 0;
  off_t block = cluster / FAT_BLOCK_ENTRIES;
  struct fat_block *slot = &fat_cache[block % FAT_CACHE_BLOCKS];
  cluster_t value = 0;

  if (!cluster_valid (cluster))
    return EIO;

  pthread_mutex_lock (&fat_cache_lock);
  if (slot->tag != block + 1)
    {
      off_t start = block * FAT_BLOCK_SIZE;
      size_t len = FAT_BLOCK_SIZE;

      if (start + (off_t) len > vol.fat_size)
	len = vol.fat_size - start;
      slot->tag = 0;
      err = exfat_disk_read (vol.fat_offset + start, slot->data, len);
      if (!err)
	slot->tag = block + 1;
    }
  if (!err)
    value = get_le32 (slot->data
		      + (cluster % FAT_BLOCK_ENTRIES) * sizeof (uint32_t));
  pthread_mutex_unlock (&fat_cache_lock);

  if (err)
    return err;
  if (value != EXFAT_EOC && !cluster_valid (value))
    return EIO;
  *next = value;
  return 0;
}


static uint32_t
checksum32 (uint32_t sum, const uint8_t *data, size_t len)
{
  for (size_t i = 0; i < len; i++)
    sum = (sum << 31 | sum >> 1) + data[i];
  return sum;
}

/* Parse and validate the main boot region into VOL.  */
static void
read_boot_region (void)
{
  error_t err;
  uint8_t *boot;
  size_t probe = store->block_size > 512 ? store->block_size : 512;
  size_t sector, region;
  uint32_t sum;
  uint64_t volume_length;
  uint32_t fat_offset, fat_length, heap_offset, cluster_count, root_cluster;
  unsigned sector_shift, cluster_shift, fats, active_fat;

  if (store->block_size != (size_t) 1 << store->log2_block_size
      || store->block_size > 4096)
    refuse ("unsupported store block size %zu", store->block_size);
  if (store->size < probe)
    refuse ("too small to hold a file system");

  boot = malloc (12 * 4096);
  if (!boot)
    error (1, ENOMEM, "boot region");

  err = exfat_disk_read (0, boot, probe);
  if (err)
    error (1, err, "%s: reading the boot sector", diskfs_disk_name);

  if (memcmp (boot + 3, "EXFAT   ", 8) != 0)
    refuse ("not an exFAT file system");
  for (int i = 11; i < 64; i++)
    if (boot[i] != 0)
      refuse ("boot sector: the MustBeZero field is not zero");
  if (get_le16 (boot + 510) != 0xaa55)
    refuse ("boot sector: bad boot signature");

  sector_shift = boot[108];
  cluster_shift = boot[109];
  if (sector_shift < 9 || sector_shift > 12)
    refuse ("boot sector: BytesPerSectorShift %u is out of range",
	    sector_shift);
  if (cluster_shift > 25 - sector_shift)
    refuse ("boot sector: SectorsPerClusterShift %u is out of range",
	    cluster_shift);
  sector = (size_t) 1 << sector_shift;
  if (sector < store->block_size)
    refuse ("%zu-byte sectors on a store with %zu-byte blocks",
	    sector, store->block_size);
  if (vm_page_size % sector != 0)
    refuse ("%zu-byte sectors do not divide the page size", sector);

  region = 12 * sector;
  if (store->size < region)
    refuse ("too small to hold the boot region");
  err = exfat_disk_read (0, boot, region);
  if (err)
    error (1, err, "%s: reading the boot region", diskfs_disk_name);

  /* VolumeFlags and PercentInUse change while the volume is in use, so
     the checksum leaves them out.  */
  sum = checksum32 (0, boot, 106);
  sum = checksum32 (sum, boot + 108, 4);
  sum = checksum32 (sum, boot + 113, 11 * sector - 113);
  for (size_t i = 0; i < sector; i += 4)
    if (get_le32 (boot + 11 * sector + i) != sum)
      refuse ("the boot region's checksum is wrong");

  if (boot[105] != 1)
    refuse ("unsupported exFAT revision %u.%02u", boot[105], boot[104]);

  fats = boot[110];
  active_fat = get_le16 (boot + 106) & 1;
  if (fats != 1 && fats != 2)
    refuse ("boot sector: NumberOfFats is %u", fats);
  if (active_fat >= fats)
    refuse ("boot sector: the second FAT is active, but there is only one");

  volume_length = get_le64 (boot + 72);
  fat_offset = get_le32 (boot + 80);
  fat_length = get_le32 (boot + 84);
  heap_offset = get_le32 (boot + 88);
  cluster_count = get_le32 (boot + 92);
  root_cluster = get_le32 (boot + 96);

  if (volume_length < ((uint64_t) 1 << 20) >> sector_shift)
    refuse ("boot sector: VolumeLength %llu is under 1 MiB",
	    (unsigned long long) volume_length);
  if (volume_length > (uint64_t) store->size >> sector_shift)
    refuse ("the volume has %llu sectors, but the store holds only %llu",
	    (unsigned long long) volume_length,
	    (unsigned long long) (store->size >> sector_shift));
  if (cluster_count == 0 || cluster_count > EXFAT_MAX_CLUSTERS)
    refuse ("boot sector: ClusterCount %u is out of range", cluster_count);
  if (fat_offset < 24)
    refuse ("boot sector: the FAT overlaps the boot regions");
  if (fat_length < (((uint64_t) cluster_count + 2) * 4 + sector - 1)
      >> sector_shift)
    refuse ("boot sector: the FAT is too short for %u clusters",
	    cluster_count);
  if ((uint64_t) fat_offset + (uint64_t) fat_length * fats > heap_offset)
    refuse ("boot sector: the FAT overlaps the cluster heap");
  if (heap_offset + ((uint64_t) cluster_count << cluster_shift)
      > volume_length)
    refuse ("boot sector: the cluster heap extends past the volume");
  if (root_cluster < EXFAT_FIRST_CLUSTER
      || root_cluster - EXFAT_FIRST_CLUSTER >= cluster_count)
    refuse ("boot sector: the root directory's cluster %u is out of range",
	    root_cluster);

  vol.sector_shift = sector_shift;
  vol.cluster_shift = sector_shift + cluster_shift;
  vol.fat_offset = ((off_t) fat_offset + (off_t) fat_length * active_fat)
    << sector_shift;
  vol.fat_size = (off_t) fat_length << sector_shift;
  vol.heap_offset = (off_t) heap_offset << sector_shift;
  vol.cluster_count = cluster_count;
  vol.root_cluster = root_cluster;
  vol.active_fat = active_fat;
  vol.fat_count = fats;
  vol.serial_number = get_le32 (boot + 100);
  volume_dirty_at_mount = get_le16 (boot + 106) & 2;

  free (boot);
}


/* Read the first LENGTH bytes of the table that starts at cluster FIRST
   into a new buffer.  */
static error_t
read_table (cluster_t first, off_t length, uint8_t **data)
{
  error_t err;
  struct exfat_stream stream;
  size_t rounded = (length + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);

  *data = malloc (rounded);
  if (!*data)
    return ENOMEM;
  exfat_stream_init (&stream, first, false, length);
  err = exfat_stream_read (&stream, 0, *data, rounded);
  exfat_stream_fini (&stream);
  if (err)
    free (*data);
  return err;
}

static void
load_upcase_table (cluster_t first, uint64_t length, uint32_t checksum)
{
  error_t err;
  uint8_t *data;
  size_t units = length / 2;
  size_t unit = 0;
  uint32_t code = 0;

  if (length == 0 || length % 2 != 0 || length > 4 * 0x10000
      || !exfat_allocation_valid (first, false, length))
    refuse ("the up-case table's allocation is invalid");
  err = read_table (first, length, &data);
  if (err)
    error (1, err, "%s: reading the up-case table", diskfs_disk_name);
  if (checksum32 (0, data, length) != checksum)
    refuse ("the up-case table's checksum is wrong");

  exfat_upcase = malloc (0x10000 * sizeof *exfat_upcase);
  if (!exfat_upcase)
    error (1, ENOMEM, "up-case table");
  for (uint32_t i = 0; i < 0x10000; i++)
    exfat_upcase[i] = i;

  /* In the compressed form, FFFFh followed by a count stands for that
     many characters which map to themselves.  */
  while (unit < units)
    {
      uint16_t value = get_le16 (data + 2 * unit++);

      if (value == 0xffff && unit < units)
	code += get_le16 (data + 2 * unit++);
      else if (code < 0x10000)
	exfat_upcase[code++] = value;
      else
	code++;
      if (code > 0x10000)
	refuse ("the up-case table describes more than 65536 characters");
    }
  free (data);
}

static void
count_free_clusters (cluster_t first, uint64_t length)
{
  enum { WINDOW = 1 << 16 };
  error_t err = 0;
  struct exfat_stream stream;
  uint8_t *window;
  off_t bytes = ((off_t) vol.cluster_count + 7) / 8;
  cluster_t used = 0;

  if (length < (uint64_t) bytes
      || !exfat_allocation_valid (first, false, length))
    refuse ("the allocation bitmap's allocation is invalid");
  window = malloc (WINDOW);
  if (!window)
    error (1, ENOMEM, "allocation bitmap");
  exfat_stream_init (&stream, first, false, length);

  for (off_t start = 0; !err && start < bytes; start += WINDOW)
    {
      size_t len = bytes - start < WINDOW ? bytes - start : WINDOW;
      size_t rounded = (len + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);

      err = exfat_stream_read (&stream, start, window, rounded);
      for (size_t i = 0; !err && i < len; i++)
	{
	  unsigned bits = window[i];

	  if ((start + i) * 8 + 8 > vol.cluster_count)
	    bits &= (1U << (vol.cluster_count - (start + i) * 8)) - 1;
	  used += __builtin_popcount (bits);
	}
    }
  if (err)
    error (1, err, "%s: reading the allocation bitmap", diskfs_disk_name);

  exfat_free_clusters = vol.cluster_count - used;
  exfat_stream_fini (&stream);
  free (window);
}

/* Find the allocation bitmap and the up-case table in the root
   directory and load what the read path needs from them.  */
static void
load_system_tables (void)
{
  error_t err;
  struct exfat_stream root;
  struct exfat_dir dir;
  cluster_t root_clusters;
  bool have_bitmap = false, have_upcase = false;
  cluster_t bitmap_cluster = 0, upcase_cluster = 0;
  uint64_t bitmap_length = 0, upcase_length = 0;
  uint32_t upcase_checksum = 0;

  err = exfat_chain_length (vol.root_cluster, &root_clusters);
  if (err)
    refuse ("the root directory's cluster chain is damaged");
  exfat_stream_init (&root, vol.root_cluster, false,
		     (off_t) root_clusters << vol.cluster_shift);
  err = exfat_dir_open (&dir, &root, true);
  if (err)
    error (1, err, "root directory");

  for (;;)
    {
      const uint8_t *entry;

      err = exfat_dir_next_primary (&dir, &entry);
      if (err)
	error (1, err, "%s: reading the root directory", diskfs_disk_name);
      if (!entry)
	break;
      dir.pos += EXFAT_ENTRY_SIZE;

      switch (entry[0])
	{
	case EXFAT_TYPE_BITMAP:
	  if ((entry[1] & 1U) == vol.active_fat && !have_bitmap)
	    {
	      have_bitmap = true;
	      bitmap_cluster = get_le32 (entry + 20);
	      bitmap_length = get_le64 (entry + 24);
	    }
	  break;

	case EXFAT_TYPE_UPCASE:
	  if (!have_upcase)
	    {
	      have_upcase = true;
	      upcase_checksum = get_le32 (entry + 4);
	      upcase_cluster = get_le32 (entry + 20);
	      upcase_length = get_le64 (entry + 24);
	    }
	  break;

	case EXFAT_TYPE_LABEL:
	case EXFAT_TYPE_FILE:
	  break;

	default:
	  if (!(entry[0] & EXFAT_TYPE_BENIGN))
	    refuse ("unrecognized critical directory entry of type %02Xh"
		    " in the root directory", entry[0]);
	}
    }

  exfat_dir_close (&dir);
  exfat_stream_fini (&root);

  if (!have_bitmap)
    refuse ("the root directory has no allocation bitmap");
  if (!have_upcase)
    refuse ("the root directory has no up-case table");

  load_upcase_table (upcase_cluster, upcase_length, upcase_checksum);
  count_free_clusters (bitmap_cluster, bitmap_length);
}

/* Validate the volume on STORE and load its parameters, or exit with a
   message saying what is wrong with it.  */
void
exfat_mount (void)
{
  read_boot_region ();
  load_system_tables ();

  if (vol.fat_count != 1 && !diskfs_hard_readonly)
    {
      /* Section 3.1.9: a second FAT belongs to TexFAT, whose rules for
	 keeping the two in step are not implemented here.  */
      error (0, 0, "%s: the volume has two FATs; it can only be read",
	     diskfs_disk_name);
      diskfs_readonly = diskfs_hard_readonly = 1;
    }
  if (volume_dirty_at_mount && !diskfs_readonly)
    error (0, 0, "%s: the volume was not unmounted cleanly and may be"
	   " damaged; fsck.exfat can check it", diskfs_disk_name);
}
