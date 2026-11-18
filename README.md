# exfatfs

`exfatfs` is a GNU Hurd translator that reads and writes exFAT volumes.
exFAT is the filesystem on most SD cards larger than 32 GB and on many USB
drives. The translator is built on libdiskfs, like the Hurd's `ext2fs` and
`fatfs`.

```
settrans -ac /mnt /hurd/exfatfs /dev/sd1s1
```

## Status

The translator reads and writes files and directories, including renames.
It was first written inside the Hurd source tree as
[hurdkit/hurd#51](https://github.com/hurdkit/hurd/pull/51), and this
repository keeps that history. hurdkit's guest test runs 19 exFAT checks
against it in a Hurd VM, and all of them pass. Read
[Known limits](#known-limits) before you trust it with data you can't
lose.

## Build

The build cross-compiles for x86_64-gnu against an installed prefix. The
prefix must hold the cross compiler, `mig`, glibc, and these Hurd libraries
with their headers: libdiskfs, libstore and its `libstore_<type>.a` store
types, libpager, libports, libfshelp, libiohelp, libihash,
libshouldbeinlibc, and libhurdbugaddr. To build `exfatfs` and
`exfatfs.static`, run:

```
make
```

These variables change the build:

| Variable | Default | Meaning |
|---|---|---|
| `HURD_PREFIX` | `$(HOME)/opt/x86_64-gnu` | The cross prefix. |
| `CC` | `$(HURD_PREFIX)/bin/x86_64-gnu-gcc` | The cross compiler. |
| `MIG` | `$(HURD_PREFIX)/bin/mig` | The Mach interface generator. |
| `CFLAGS` | `-g -O2` | Optimization and debug flags. |
| `STORE_TYPES` | `device remap`, plus `bunzip2` and `gunzip` if the prefix has them | The store types that `exfatfs.static` links in. |

`exfatfs.static` cannot load store types at run time, so it can open only
the types in `STORE_TYPES`. The default is the set that the Hurd links into
its static filesystems. libstore has `bunzip2` only if libbz2 was in the
prefix when libstore was built, and `gunzip` only if zlib was.

hurdkit's toolchain installs the prefix in `~/opt/x86_64-gnu`. Its base
`hurd` stage does not install libdiskfs, libstore, or libhurdbugaddr. Until
it does, install them from a configured Hurd build with
`make -C <dir> install` for each of `libhurdbugaddr`, `libstore`, and
`libdiskfs`.

## Use

Copy `exfatfs.static` to `/hurd/exfatfs` on the Hurd system. `exfatfs`
is the same translator linked against the Hurd's shared libraries. The
guest test uses only `exfatfs.static`. Set the translator on a node, with
the device or image as its argument:

```
settrans -ac /mnt /hurd/exfatfs /dev/sd1s1
```

To unmount the volume cleanly, run `settrans -g /mnt`.

Names are UTF-8. Lookups ignore case, as on other systems. A name longer
than 255 bytes in UTF-8 cannot be represented on the Hurd. The translator
does not list such a name and cannot look it up, but the rest of the
directory works.

### Options

`exfatfs` takes the standard libdiskfs options, such as `--readonly`, and
these:

| Option | Meaning |
|---|---|
| `-U uid`, `--uid=uid` | The owner of every file, because exFAT does not store one. |
| `-G gid`, `--gid=gid` | The group of every file, because exFAT does not store one. |

## How exfatfs keeps your data safe

- `exfatfs` sets the volume's VolumeDirty flag before its first write. It
  clears the flag on a clean unmount, so `fsck.exfat` checks the volume
  after a crash.
- The translator keeps VolumeDirty set through a clean unmount if a store
  write failed, or if files that were removed while open still held
  clusters.
- A volume that was not unmounted cleanly is mounted read-write, as Linux
  does. The translator warns that the volume may be damaged and leaves
  VolumeDirty set until `fsck.exfat` has checked it. If you can, run
  `fsck.exfat` before you write to such a volume. If a crash left two files
  that share clusters, removing one of them frees clusters that the other
  still uses.
- The translator also warns when the volume records that its medium
  reported failures.
- Some volumes are served read-only, because the translator cannot write
  them safely. These are volumes with a revision or up-case table that the
  specification does not allow, with two FATs (TexFAT), or with a bitmap
  that marks the system clusters free. Linux writes some of these and
  damages them.
- A workload that only reads never writes to the volume, access times
  included.
- A rename does every step that can fail before its first disk write.
- When an entry of a kind the translator does not know is removed, the
  clusters it names stay allocated. Vendor allocations and benign primary
  entries are such kinds. Section 8.2 of the exFAT specification says to
  free those clusters, and the Linux driver frees them. On crafted images,
  freeing them lost another file's data under Linux as well, and
  `fsck.exfat` cannot tell such a volume from a good one. Leaking the
  clusters loses nothing.

## Known limits

- A crash while a file that already has a FAT chain grows can leave more
  clusters chained than the file's size uses. `fsck.exfat` repairs the
  chain, and the old contents stay intact.
- A crash partway through some updates can leak clusters. `fsck.exfat`
  neither reports nor frees them.
- On a store without `DEV_FLUSH_CACHE`, write ordering depends on the
  medium.

## Test

The test lives in [hurdkit](https://github.com/hurdkit/hurdkit), because it
needs hurdkit's Hurd VM image and boot library. `scripts/exfatfs_test.sh`,
added by [hurdkit/hurdkit#28](https://github.com/hurdkit/hurdkit/pull/28),
makes exFAT images with the Linux exfat driver, mounts each image in the
guest with `exfatfs`, and compares every path's type, size, mtime, and
sha256 with the Linux view of it. It then checks that corrupt images are
refused without a crash. Last, it applies scripted writes and checks the
result with `fsck.exfat -n` and a Linux readback. The loop mounts need
sudo.

The script takes the translator from `$TRANSLATOR_DIR/exfatfs/exfatfs.static`.
If your checkout is a directory named `exfatfs`, its parent works as
`TRANSLATOR_DIR`. To run the exFAT checks against this repository's
build, run this in a hurdkit checkout that has built its VM image:

```
make -C ../exfatfs
FS=exfat TRANSLATOR_DIR=.. SSH_PORT=2222 \
	BASE_IMG=img/hurd.packages.qcow2 SSH_KEY_DIR=img/keys \
	bash scripts/exfatfs_test.sh
```

## License

GPL version 2 or later. See `LICENSE`.
