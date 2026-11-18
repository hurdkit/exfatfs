/* mig-mutate.h - type translations for the fs.defs stub header.

   Copyright (C) 2026 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

/* The file translations of libdiskfs's fsmutations.h, so the generated
   prototypes of the diskfs_S_ routines exfatfs overrides are the ones
   libdiskfs dispatches to.  */

#define FILE_INTRAN protid_t diskfs_begin_using_protid_port (file_t)
#define FILE_INTRAN_PAYLOAD protid_t diskfs_begin_using_protid_payload
#define FILE_DESTRUCTOR diskfs_end_using_protid_port (protid_t)

#define FILE_IMPORTS import "exfatfs.h";
