/* main.c - exFAT filesystem.

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
#include <stdio.h>
#include <error.h>
#include <argp.h>
#include <argz.h>

#include <version.h>
#include "exfatfs.h"

struct node *diskfs_root_node;

struct store *store = 0;
struct store_parsed *store_parsed = 0;
char *diskfs_disk_name = 0;

char *diskfs_server_name = "exfatfs";
char *diskfs_server_version = HURD_VERSION;
char *diskfs_extra_version = "GNU Hurd";
int diskfs_synchronous = 0;

int diskfs_link_max = 1;
int diskfs_name_max = EXFAT_UTF8_NAME_MAX;
int diskfs_maxsymlinks = 8;

/* The UID and GID for all files in the filesystem.  */
static uid_t default_fs_uid;
static gid_t default_fs_gid;
uid_t fs_uid;
gid_t fs_gid;

static const struct argp_option options[] =
  {
    { "uid", 'U', "uid", 0, "Default uid for files" },
    { "gid", 'G', "gid", 0, "Default gid for files" },
    { 0 }
  };

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'U':
      fs_uid = atoi (arg);
      refresh_node_stats ();
      break;
    case 'G':
      fs_gid = atoi (arg);
      refresh_node_stats ();
      break;
    case ARGP_KEY_INIT:
      state->child_inputs[0] = state->input;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }

  return 0;
}

/* Add our startup arguments to the standard diskfs set.  */
static const struct argp_child startup_children[] =
  { { &diskfs_store_startup_argp }, { 0 } };
static struct argp startup_argp =
  { options, parse_opt, 0, 0, startup_children };

/* Similarly at runtime.  */
static const struct argp_child runtime_children[] =
  { { &diskfs_std_runtime_argp }, { 0 } };
static struct argp runtime_argp =
  { options, parse_opt, 0, 0, runtime_children };

struct argp *diskfs_runtime_argp = (struct argp *) &runtime_argp;

/* Override the standard diskfs routine so we can add our own
   output.  */
error_t
diskfs_append_args (char **argz, size_t *argz_len)
{
  error_t err;
  char buf[100];

  err = diskfs_append_std_options (argz, argz_len);

  if (!err && fs_uid != default_fs_uid)
    {
      snprintf (buf, sizeof buf, "--uid=%d", fs_uid);
      err = argz_add (argz, argz_len, buf);
    }

  if (!err && fs_gid != default_fs_gid)
    {
      snprintf (buf, sizeof buf, "--gid=%d", fs_gid);
      err = argz_add (argz, argz_len, buf);
    }

  if (!err)
    err = store_parsed_append_args (store_parsed, argz, argz_len);

  return err;
}

int
main (int argc, char **argv)
{
  error_t err;
  mach_port_t bootstrap;

  default_fs_uid = getuid ();
  default_fs_gid = getgid ();
  fs_uid = default_fs_uid;
  fs_gid = default_fs_gid;

  /* This filesystem is not capable of writing yet.  */
  diskfs_readonly = 1;
  diskfs_hard_readonly = 1;

  /* Initialize the diskfs library, parse arguments, and open the
     store.  This starts the first diskfs thread for us.  */
  store = diskfs_init_main (&startup_argp, argc, argv, &store_parsed,
			    &bootstrap);

  exfat_mount ();

  create_file_pagers ();

  err = diskfs_cached_lookup (EXFAT_ROOT_INO, &diskfs_root_node);
  if (err)
    error (1, err, "%s: reading the root directory", diskfs_disk_name);
  pthread_mutex_unlock (&diskfs_root_node->lock);

  diskfs_startup_diskfs (bootstrap, 0);

  pthread_exit (NULL);

  return 0;
}

/* Nothing to do for read-only medium.  */
error_t
diskfs_reload_global_state (void)
{
  return 0;
}

error_t
diskfs_set_hypermetadata (int wait, int clean)
{
  return 0;
}

void
diskfs_readonly_changed (int readonly)
{
  /* We should never get here because we set diskfs_hard_readonly above.  */
  abort ();
}
