/* dir.c - Directory lookup and listing.

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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "exfatfs.h"

/* Nothing is carried from a lookup to a later directory change while
   the file system cannot be written.  */
struct dirstat
{
  int unused;
};

const size_t diskfs_dirstat_size = sizeof (struct dirstat);

void
diskfs_null_dirstat (struct dirstat *ds)
{
}

error_t
diskfs_drop_dirstat (struct node *dp, struct dirstat *ds)
{
  return 0;
}

/* Implement the diskfs_lookup_hard callback as described in
   <hurd/diskfs.h>.  */
error_t
diskfs_lookup_hard (struct node *dp, const char *name, lookup_flags_t l_flags,
		    struct node **npp, struct dirstat *ds,
		    struct protid *cred)
{
  if (npp)
    *npp = NULL;

  if (strcmp (name, ".") == 0)
    {
      if (npp)
	{
	  diskfs_nref (dp);
	  *npp = dp;
	}
      return 0;
    }

  /* The root is the only directory so far, and it has no entries.  */
  if (strcmp (name, "..") == 0)
    return EAGAIN;
  return ENOENT;
}

error_t
diskfs_direnter_hard (struct node *dp, const char *name, struct node *np,
		      struct dirstat *ds, struct protid *cred)
{
  return EROFS;
}

error_t
diskfs_dirremove_hard (struct node *dp, struct dirstat *ds)
{
  return EROFS;
}

error_t
diskfs_dirrewrite_hard (struct node *dp, struct node *np, struct dirstat *ds)
{
  return EROFS;
}

int
diskfs_dirempty (struct node *dp, struct protid *cred)
{
  return 1;
}

#define DIRENT_ALIGN	__alignof__ (struct dirent)
#define DIRENT_LEN(namlen) \
  ((offsetof (struct dirent, d_name) + (namlen) + 1 + DIRENT_ALIGN - 1) \
   & ~(DIRENT_ALIGN - 1))

/* Implement the diskfs_get_directs callback as described in
   <hurd/diskfs.h>.  Entries 0 and 1 are `.' and `..', which the volume
   does not record.  */
error_t
diskfs_get_directs (struct node *dp, int entry, int nentries, char **data,
		    mach_msg_type_number_t *datacnt, vm_size_t bufsiz,
		    int *amt)
{
  static const char *const names[] = { ".", ".." };
  size_t reclen = DIRENT_LEN (2);
  char *datap;
  int count = 0;

  if (entry < 2 && 2 * reclen > *datacnt)
    {
      void *new = mmap (0, vm_page_size, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);

      if (new == MAP_FAILED)
	return errno;
      *data = new;
    }
  datap = *data;

  for (int index = entry;
       index < 2 && (nentries == -1 || count < nentries);
       index++, count++)
    {
      struct dirent *userp = (struct dirent *) datap;

      userp->d_fileno = dp->cache_id;
      userp->d_type = DT_DIR;
      userp->d_reclen = reclen;
      userp->d_namlen = strlen (names[index]);
      strcpy (userp->d_name, names[index]);
      datap += reclen;
    }

  *amt = count;
  *datacnt = datap - *data;
  return 0;
}
