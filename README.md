# PrismPack

A packed install floppy for AmigaOS programs. The whole install drawer goes
on the disk as one compressed archive, next to a 10 KB unpacker and a
one-click start script; the floppy then carries everything the `.lha` does.
Made for [PrismNet](https://github.com/lainejones/PrismNet), whose package
had outgrown an 880 KB disk, and usable for any drawer with an Installer
script.

- `PrismUnpack` (AmigaOS 3.1 or newer, any 68k CPU): unpacks a `.ppk`
  archive with every file's protection bits, dates and comments, checks each
  file's CRC-32, and refuses to start when the disk hasn't room for it all.
  About 1 MB in 70 s on a 7 MHz 68000, 15 s on a 68030.
- `host/pack.py` (Python 3, no extra modules): packs a drawer, lists and
  unpacks archives on the host, and makes the floppy set.

## The floppy set

```
python host/pack.py floppy dist/MyApp floppy --icon icons/Install_MyApp.info --info dist/MyApp.info
```

writes to `floppy/`:

| File | What |
|---|---|
| `MyApp.ppk` | the drawer `dist/MyApp` (and its icon) as one archive, deflate per file |
| `PrismUnpack` | the unpacker |
| `Install_MyApp` | the start script: unpacks to `SYS:PrismPack-MyApp`, runs `Install_MyApp` from there with the Installer, deletes the drawer again |
| `Install_MyApp.info` | your icon, retargeted to IconX with a console window |

Put the four on a floppy (xdftool, or copy them in an emulator); the user
double-clicks the icon as before. The drawer is unpacked on the hard disk,
not in RAM, so 1 MB machines install from it too. `--installer NAME` for an
Installer script not called `Install_<drawer>`, `--temp DIR` for another
temporary drawer.

The installer script sees exactly the files the `.lha` user gets, with one
difference it can check: the unpacked drawer is temporary, so a program it
starts must not keep that drawer as its current directory (a daemon started
with `run` inherits it; PrismNet 1.0.9 moves to `SYS:` for that reason). The
start script says so when it couldn't remove the drawer.

## The archive

`PPK1`: a 16-byte header, then one entry per drawer and file, drawers before
their contents. Each entry has the type, the method (stored or raw deflate),
the Amiga protection bits, DateStamp and comment, the sizes, the CRC-32 of
the unpacked data, the `/`-separated name and the packed data. Names are
checked against the 30-character limit of the Amiga file systems when
packing. `pack.py list` and `pack.py unpack` read it on the host.

Compression is zlib's deflate at level 9, file by file; the decoder in
`src/inflate.c` is written from RFC 1951 with a 10-bit lookup table for the
common codes. `make test` (gcc and Python in WSL) runs it against zlib on
every file in a drawer and on corrupted streams.

## Building

```
wsl -e bash -lc 'cd /mnt/c/projects/PrismPack && PATH=/opt/amiga/bin:$PATH make'
```

with the amiga-gcc toolchain (Bebbo). `make test TESTDIR=some/drawer` for
the decoder tests.

## Licence

GPL-3.0-only, see LICENSE.
