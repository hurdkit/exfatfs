/* pager.c - File data through libpager.

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
#include <unistd.h>
#include <error.h>
#include "exfatfs.h"

/* A ports bucket to hold file pager ports.  */
static struct port_bucket *file_pager_bucket;

static struct pager_requests *file_pager_requests;

static pthread_spinlock_t node_to_page_lock = PTHREAD_SPINLOCK_INITIALIZER;

/* Satisfy a pager read request for PAGER, of the page at offset PAGE,
   into a new buffer in *BUF.  */
error_t
pager_read_page (struct user_pager_info *pager, vm_offset_t page,
		 vm_address_t *buf, int *writelock)
{
  error_t err = 0;
  struct disknode *dn = pager->node->dn;
  off_t valid;
  void *data;

  /* Every page of a file has its clusters, which diskfs_grow sees
     to.  */
  *writelock = 0;

  data = mmap (0, vm_page_size, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);
  if (data == MAP_FAILED)
    return errno;

  pthread_rwlock_rdlock (&dn->alloc_lock);
  pthread_mutex_lock (&dn->valid_lock);
  valid = dn->valid_length;
  pthread_mutex_unlock (&dn->valid_lock);
  if (valid > dn->stream.length)
    valid = dn->stream.length;

  /* What lies past ValidDataLength reads as zeros, whatever the disk
     holds there.  */
  if ((off_t) page < valid)
    {
      size_t len = vm_page_size;
      size_t sectors;

      if (valid - (off_t) page < (off_t) len)
	len = valid - page;
      sectors = (len + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
      err = exfat_stream_read (&dn->stream, page, data, sectors);
      if (!err)
	memset ((char *) data + len, 0, sectors - len);
    }
  pthread_rwlock_unlock (&dn->alloc_lock);

  if (err)
    munmap (data, vm_page_size);
  else
    *buf = (vm_address_t) data;
  return err;
}

/* Satisfy a pager write request for PAGER, of the page at offset PAGE,
   from BUF.

   The page is written before ValidDataLength is moved past it, and
   the entry set gets the new ValidDataLength later still.  If the
   translator dies in between, the file reads as zeros where it could
   have read as what was written, and never as what the clusters held
   before they were the file's.  */
error_t
pager_write_page (struct user_pager_info *pager, vm_offset_t page,
		  vm_address_t buf)
{
  error_t err = 0;
  struct node *node = pager->node;
  struct disknode *dn = node->dn;
  off_t end;

  if (dn->attributes & EXFAT_ATTR_DIRECTORY)
    /* Directories are changed an entry set at a time, never through
       a mapping.  */
    return EROFS;

  pthread_rwlock_rdlock (&dn->alloc_lock);
  end = (off_t) page + vm_page_size;
  if (end > dn->stream.length)
    end = dn->stream.length;
  if ((off_t) page < end)
    {
      pthread_mutex_lock (&dn->valid_lock);
      /* Section 7.6.5.1: what lies between ValidDataLength and the
	 data written past it is to hold zeros.  */
      if (dn->valid_length < (off_t) page)
	err = exfat_stream_zero (&dn->stream, dn->valid_length,
				 page - dn->valid_length);
      if (!err)
	err = exfat_stream_write (&dn->stream, page, (void *) buf,
				  end - page);
      if (!err && dn->valid_length < end)
	{
	  dn->valid_length = end;
	  node->dn_stat_dirty = 1;
	}
      pthread_mutex_unlock (&dn->valid_lock);
    }
  pthread_rwlock_unlock (&dn->alloc_lock);
  return err;
}

error_t
pager_unlock_page (struct user_pager_info *pager, vm_offset_t page)
{
  return 0;
}

void
pager_notify_evict (struct user_pager_info *pager, vm_offset_t page)
{
  assert_backtrace (!"unrequested notification on eviction");
}

/* The pager of NODE with a reference, or null if it has none.  */
static struct pager *
node_pager (struct node *node)
{
  struct pager *pager;

  pthread_spin_lock (&node_to_page_lock);
  pager = node->dn->pager;
  if (pager)
    ports_port_ref (pager);
  pthread_spin_unlock (&node_to_page_lock);
  return pager;
}

/* This syncs a single file (NODE) to disk.  The pages go first: writing
   them moves ValidDataLength, which the entry set records.  */
void
diskfs_file_update (struct node *node, int wait)
{
  struct pager *pager = node_pager (node);

  if (pager)
    {
      pager_sync (pager, wait);
      ports_port_deref (pager);
    }
  diskfs_node_update (node, wait);
}

/* Have the kernel drop the pages it holds of the LEN bytes at OFFSET of
   NODE, without writing them.  */
void
discard_node_pages (struct node *node, off_t offset, off_t len)
{
  struct pager *pager = node_pager (node);

  if (pager)
    {
      pager_flush_some (pager, offset, len, 1);
      ports_port_deref (pager);
    }
}

/* Invalidate any pager data associated with NODE.  */
void
flush_node_pager (struct node *node)
{
  struct pager *pager = node_pager (node);

  if (pager)
    {
      pager_flush (pager, 1);
      ports_port_deref (pager);
    }
}

/* Return in *OFFSET and *SIZE the minimum valid address the pager
   will accept and the size of the object.  */
error_t
pager_report_extent (struct user_pager_info *pager,
		     vm_address_t *offset, vm_size_t *size)
{
  *offset = 0;
  *size = pager->node->allocsize;
  return 0;
}

/* This is called when a pager is being deallocated after all extant
   send rights have been destroyed.  */
void
pager_clear_user_data (struct user_pager_info *upi)
{
  diskfs_nrele_light (upi->node);
}

/* This will be called when the ports library wants to drop weak
   references.  The pager library creates no weak references itself.  */
void
pager_dropweak (struct user_pager_info *upi)
{
  struct pager *pager;

  pthread_spin_lock (&node_to_page_lock);
  pager = upi->node->dn->pager;
  if (pager)
    {
      assert_backtrace (pager_get_upi (pager) == upi);
      upi->node->dn->pager = NULL;
      ports_port_deref_weak (pager);
    }
  pthread_spin_unlock (&node_to_page_lock);
}

void
create_file_pagers (void)
{
  error_t err;

  file_pager_bucket = ports_create_bucket ();
  if (!file_pager_bucket)
    error (2, errno, "can't create file pager bucket");

  err = pager_start_workers (file_pager_bucket, &file_pager_requests);
  if (err)
    error (2, err, "can't create libpager worker threads");
}

/* Call this to create a file pager and return a send right.  NODE must
   be locked.  */
mach_port_t
diskfs_get_filemap (struct node *node, vm_prot_t prot)
{
  struct pager *pager;
  mach_port_t right;
  error_t err;

  assert_backtrace (S_ISDIR (node->dn_stat.st_mode)
		    || S_ISREG (node->dn_stat.st_mode));

  if (S_ISDIR (node->dn_stat.st_mode) && (prot & VM_PROT_WRITE))
    {
      errno = EISDIR;
      return MACH_PORT_NULL;
    }

  /* A pager can only report a bad chain as a memory error, which the
     reader takes as a signal.  Find out now, while the caller can still
     be told EIO.  */
  err = exfat_stream_check (&node->dn->stream);
  if (err)
    {
      errno = err;
      return MACH_PORT_NULL;
    }

  pthread_spin_lock (&node_to_page_lock);
  pager = node->dn->pager;
  if (pager)
    {
      ports_port_ref (pager);
      pager_get_upi (pager)->max_prot |= prot;
    }
  else
    {
      struct user_pager_info *upi;

      pager = pager_create_alloc (sizeof *upi, file_pager_bucket,
				  1, MEMORY_OBJECT_COPY_DELAY, 0);
      if (pager == NULL)
	{
	  pthread_spin_unlock (&node_to_page_lock);
	  return MACH_PORT_NULL;
	}
      upi = pager_get_upi (pager);
      upi->node = node;
      upi->max_prot = prot;
      diskfs_nref_light (node);
      node->dn->pager = pager;

      /* A weak reference for being part of the node.  */
      ports_port_ref_weak (pager);
    }
  pthread_spin_unlock (&node_to_page_lock);

  if (prot & VM_PROT_WRITE)
    right = ports_get_send_right (pager);
  else
    right = pager_create_ro_port (pager);
  ports_port_deref (pager);

  assert_backtrace (MACH_PORT_VALID (right));
  return right;
}

static void
set_node_pager_caching (struct node *node, int may_cache)
{
  struct pager *pager = node_pager (node);

  if (pager)
    {
      pager_change_attributes (pager, may_cache, MEMORY_OBJECT_COPY_DELAY, 0);
      ports_port_deref (pager);
    }
}

/* Call this when we should turn off caching so that unused memory
   object ports get freed.  */
void
drop_pager_softrefs (struct node *node)
{
  set_node_pager_caching (node, 0);
}

/* Call this when we should turn on caching because it's no longer
   important for unused memory object ports to get freed.  */
void
allow_pager_softrefs (struct node *node)
{
  set_node_pager_caching (node, 1);
}

/* Call this to find out the struct pager * corresponding to the file
   pager of NODE.  This should be used *only* as a subsequent argument
   to register_memory_fault_area.  NODE must be locked.  */
struct pager *
diskfs_get_filemap_pager_struct (struct node *node)
{
  /* This is safe because pager can't be cleared; there must be an
     active mapping for this to be called.  */
  return node->dn->pager;
}

/* Shutdown all the pagers, which writes their pages, and then write
   the entry sets, which record how far the pages got.  */
void
diskfs_shutdown_pager (void)
{
  error_t shutdown_one (void *v_p)
    {
      pager_shutdown (v_p);
      return 0;
    }

  ports_bucket_iterate (file_pager_bucket, shutdown_one);
  exfat_write_all_nodes ();
}

/* Sync all the pagers and then the entry sets.  */
void
diskfs_sync_everything (int wait)
{
  error_t sync_one (void *v_p)
    {
      pager_sync (v_p, wait);
      return 0;
    }

  ports_bucket_iterate (file_pager_bucket, sync_one);
  exfat_write_all_nodes ();
  if (wait)
    exfat_store_flush ();
}

static void
set_caching (int may_cache, int wait)
{
  error_t set_one (void *v_p)
    {
      pager_change_attributes (v_p, may_cache, MEMORY_OBJECT_COPY_DELAY,
			       wait);
      return 0;
    }

  ports_bucket_iterate (file_pager_bucket, set_one);
}

/* Tell diskfs if there are pagers exported, and if none, then
   prevent any new ones from showing up.  */
int
diskfs_pager_users (void)
{
  if (ports_count_bucket (file_pager_bucket) == 0)
    return 0;

  /* Turning caching off synchronously terminates every pager nobody
     maps.  Give it a second; the kernel doesn't actually shutdown
     immediately.  */
  set_caching (0, 1);
  sleep (1);
  if (ports_count_bucket (file_pager_bucket) == 0)
    return 0;

  set_caching (1, 0);
  ports_enable_bucket (file_pager_bucket);
  return 1;
}

/* Return the bitwise or of the maximum prot parameter (the second arg
   to diskfs_get_filemap) for all active user pagers.  */
vm_prot_t
diskfs_max_user_pager_prot (void)
{
  vm_prot_t max_prot = 0;

  error_t add_pager_max_prot (void *v_p)
    {
      max_prot |= pager_get_upi (v_p)->max_prot;
      return 0;
    }

  if (ports_count_bucket (file_pager_bucket) > 0)
    {
      set_caching (0, 1);
      sleep (1);
      ports_bucket_iterate (file_pager_bucket, add_pager_max_prot);
      set_caching (1, 0);
    }
  ports_enable_bucket (file_pager_bucket);
  return max_prot;
}
