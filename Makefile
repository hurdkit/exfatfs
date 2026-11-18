# exfatfs: read-write exFAT translator for GNU/Hurd.
#
# Cross-builds against an x86_64-gnu prefix such as the one hurdkit's
# toolchain installs.  The prefix must hold the Hurd libraries in LIBS,
# libdiskfs and libstore included, with their headers.

HURD_PREFIX ?= $(HOME)/opt/x86_64-gnu
CC = $(HURD_PREFIX)/bin/x86_64-gnu-gcc
MIG = $(HURD_PREFIX)/bin/mig
CFLAGS ?= -g -O2

# The store types linked into exfatfs.static, which cannot load the
# others at run time.  The Hurd's default for static filesystems:
# device and remap, plus bunzip2 and gunzip when the prefix's libstore
# has them (it does when libbz2 and zlib were there to build them).
STORE_TYPES = device remap \
    $(patsubst $(HURD_PREFIX)/lib/libstore_%.a,%,$(wildcard \
        $(HURD_PREFIX)/lib/libstore_bunzip2.a \
        $(HURD_PREFIX)/lib/libstore_gunzip.a))

LIBS = diskfs iohelp fshelp store pager ports ihash shouldbeinlibc
LDLIBS = -lpthread $(if $(filter gunzip,$(STORE_TYPES)),-lz) \
         $(if $(filter bunzip2,$(STORE_TYPES)),-lbz2)

EXFATFS_CFLAGS = -std=gnu99 -Wall -fno-strict-aliasing -D_GNU_SOURCE \
                 -D_FILE_OFFSET_BITS=64 -I. -I$(HURD_PREFIX)/include
# --help names the Hurd's bug address, from libhurdbugaddr, as the
# in-tree translators do.
BUGADDR = -Wl,-u,argp_program_bug_address -l:libhurdbugaddr.a

OBJS = main.o volume.o alloc.o map.o entry.o dir.o inode.o vinode.o pager.o

all: exfatfs exfatfs.static

exfatfs: $(OBJS)
	$(CC) $(CFLAGS) -L$(HURD_PREFIX)/lib $(BUGADDR) $^ -o $@ \
	    -Wl,--start-group $(LIBS:%=-l%) $(LDLIBS) -Wl,--end-group

exfatfs.static: $(OBJS)
	$(CC) $(CFLAGS) -static -L$(HURD_PREFIX)/lib $(BUGADDR) $^ -o $@ \
	    -Wl,--start-group $(LIBS:%=-l%) $(STORE_TYPES:%=-lstore_%) \
	    $(LDLIBS) -Wl,--end-group

# Prototypes of the libdiskfs RPC routines inode.c overrides.
fs_S.h: mig-mutate.h
	echo '#include <hurd/fs.defs>' | $(MIG) -n -cc '$(CC) -E -x c' \
	    -I$(HURD_PREFIX)/include -DSERVERPREFIX=S_ -DHURD_SERVER=1 \
	    -imacros mig-mutate.h -prefix diskfs_ -sheader $@ /dev/stdin

inode.o: fs_S.h

%.o: %.c exfatfs.h
	$(CC) $(CFLAGS) $(EXFATFS_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) fs_S.h exfatfs exfatfs.static

.PHONY: all clean
