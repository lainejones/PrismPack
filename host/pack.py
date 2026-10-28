#!/usr/bin/env python3
"""
pack.py - PrismPack's host side: packs a drawer into one PPK1 archive (raw
deflate per file) and builds an install floppy drawer around it.

    pack.py pack   SRC OUT.ppk [--info SRC.info]  a drawer (with its icon) into an archive
    pack.py list   ARCHIVE.ppk                     the entries
    pack.py unpack ARCHIVE.ppk DIR                 unpack on the host (tests the format)
    pack.py floppy SRC OUTDIR --icon INSTALL.info [--info SRC.info] [--installer NAME]
                   [--unpacker PrismUnpack] [--temp SYS:PrismPack-<name>]

"floppy" writes OUTDIR/<name>.ppk, OUTDIR/PrismUnpack and OUTDIR/<installer>,
an AmigaDOS script with an IconX icon: it unpacks the archive to a drawer on
the hard disk, runs the Installer script of the same name in it, and deletes
the drawer again. <name> is SRC's last path component, <installer> defaults
to Install_<name>.

PPK1 format (big-endian):
  header  "PPK1", u16 version (1), u16 flags (0), u32 entries, u32 bytes unpacked
  entry   u8 type (1 drawer, 2 file), u8 method (0 stored, 8 raw deflate),
          u16 name length, u32 protection, u32 days, u32 minute, u32 tick,
          u16 comment length, u16 reserved (0), u32 size, u32 packed size,
          u32 CRC-32 of the unpacked data; then the name ("/" separated,
          ISO-8859-1), the comment and the packed data.
  Drawers come before what is in them.

Part of PrismPack. GPL-3.0-only, see LICENSE.
"""
import argparse, os, shutil, struct, sys, time, zlib

MAGIC = b'PPK1'
HEAD = struct.Struct('>4sHHII')
ENTRY = struct.Struct('>BBHIIIIHHIII')
T_DIR, T_FILE = 1, 2
M_STORED, M_DEFLATE = 0, 8
AMIGA_EPOCH = 252460800         # 1978-01-01 in Unix time
FIBF_SCRIPT = 1 << 6


def datestamp(t):
    """Unix time -> Amiga DateStamp (days, minute, tick); the Amiga clock is local time"""
    t = int(t - AMIGA_EPOCH - time.timezone + (3600 if time.localtime(t).tm_isdst > 0 else 0))
    t = max(t, 0)
    return t // 86400, t % 86400 // 60, t % 60 * 50


def deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return c.compress(data) + c.flush()


def walk(src):
    """(relative path with /, full path, is_dir) for the drawer, drawers first"""
    out = []
    for d, dirs, files in os.walk(src):
        dirs.sort(key=str.lower)
        rel = os.path.relpath(d, src)
        if rel != '.':
            out.append((rel.replace(os.sep, '/'), d, True))
        for f in sorted(files, key=str.lower):
            out.append((os.path.relpath(os.path.join(d, f), src).replace(os.sep, '/'), os.path.join(d, f), False))
    return out


def pack(src, dst, info=None, script_names=()):
    name = os.path.basename(os.path.normpath(src))
    items = [(name, src, True)] + [(name + '/' + r, p, isdir) for r, p, isdir in walk(src)]
    if info:
        items.append((name + '.info', info, False))
    entries, total = [], 0
    for rel, path, isdir in items:
        for part in rel.split('/'):
            if len(part) > 30:              # the Amiga's file systems take 30 characters
                sys.exit('%s: name longer than 30 characters: %s' % (src, part))
            if not part or ':' in part:
                sys.exit('%s: bad name: %s' % (src, rel))
        st = os.stat(path)
        days, minute, tick = datestamp(st.st_mtime)
        prot = FIBF_SCRIPT if os.path.basename(rel) in script_names else 0
        n = rel.encode('latin-1')
        if isdir:
            entries.append(ENTRY.pack(T_DIR, M_STORED, len(n), prot, days, minute, tick, 0, 0, 0, 0, 0) + n)
            continue
        data = open(path, 'rb').read()
        packed = deflate(data)
        method = M_DEFLATE
        if len(packed) >= len(data):
            packed, method = data, M_STORED
        total += len(data)
        entries.append(ENTRY.pack(T_FILE, method, len(n), prot, days, minute, tick, 0, 0,
                                  len(data), len(packed), zlib.crc32(data) & 0xffffffff) + n + packed)
    with open(dst, 'wb') as f:
        f.write(HEAD.pack(MAGIC, 1, 0, len(entries), total))
        for e in entries:
            f.write(e)
    return len(entries), total, os.path.getsize(dst)


def read(path):
    b = open(path, 'rb').read()
    magic, ver, flags, count, total = HEAD.unpack_from(b, 0)
    if magic != MAGIC or ver != 1:
        sys.exit('%s: not a PPK1 archive' % path)
    o = HEAD.size
    for _ in range(count):
        (typ, method, nlen, prot, days, minute, tick, clen, _r, size, psize, crc) = ENTRY.unpack_from(b, o)
        o += ENTRY.size
        name = b[o:o + nlen].decode('latin-1'); o += nlen
        comment = b[o:o + clen].decode('latin-1'); o += clen
        data = b[o:o + psize]; o += psize
        yield dict(type=typ, method=method, name=name, prot=prot, date=(days, minute, tick),
                   comment=comment, size=size, packed=psize, crc=crc, data=data)


def unpack(path, dst):
    for e in read(path):
        out = os.path.join(dst, *e['name'].split('/'))
        if e['type'] == T_DIR:
            os.makedirs(out, exist_ok=True)
            continue
        data = e['data'] if e['method'] == M_STORED else zlib.decompress(e['data'], -15)
        if len(data) != e['size'] or zlib.crc32(data) & 0xffffffff != e['crc']:
            sys.exit('%s: damaged' % e['name'])
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, 'wb').write(data)


# ---- icons: a classic DiskObject with its DefaultTool and ToolTypes replaced ----

def _u16(b, o): return struct.unpack_from('>H', b, o)[0]
def _u32(b, o): return struct.unpack_from('>I', b, o)[0]


def _image_end(b, o):
    w, h, depth, data = _u16(b, o + 4), _u16(b, o + 6), _u16(b, o + 8), _u32(b, o + 10)
    return o + 20 + (((w + 15) // 16) * 2 * h * depth if data else 0)


def _string(b, o):
    n = _u32(b, o)
    return b[o + 4:o + 4 + n], o + 4 + n


def retool(icon, default_tool, tool_types):
    """the icon with a new DefaultTool and ToolTypes; images and the colour
    icon data after the classic part are kept as they are"""
    b = open(icon, 'rb').read()
    if _u16(b, 0) != 0xE310:
        sys.exit('%s: not an icon' % icon)
    select = _u32(b, 4 + 22)                  # gg_SelectRender
    has_dt, has_tt, drawer, toolwin = _u32(b, 50), _u32(b, 54), _u32(b, 66), _u32(b, 70)
    o = 78 + (56 if drawer else 0)
    o = _image_end(b, o)
    if select:
        o = _image_end(b, o)
    images_end = o
    if has_dt:
        _, o = _string(b, o)
    if has_tt:
        n = _u32(b, o) // 4 - 1
        o += 4
        for _ in range(n):
            _, o = _string(b, o)
    if toolwin:
        _, o = _string(b, o)
    if drawer and _u32(b, 4 + 40) == 1:      # gg_UserData 1: OS 2 DrawerData2
        o += 6
    head = bytearray(b[:78])
    struct.pack_into('>I', head, 50, 1)       # do_DefaultTool
    struct.pack_into('>I', head, 54, 1 if tool_types else 0)
    struct.pack_into('>I', head, 70, 0)       # no ToolWindow
    struct.pack_into('>I', head, 74, 8192)    # do_StackSize
    out = bytes(head) + b[78:images_end]
    s = default_tool.encode('latin-1') + b'\0'
    out += struct.pack('>I', len(s)) + s
    if tool_types:
        out += struct.pack('>I', (len(tool_types) + 1) * 4)
        for t in tool_types:
            s = t.encode('latin-1') + b'\0'
            out += struct.pack('>I', len(s)) + s
    return out + b[o:]


STAGE1 = """; {installer} - unpacks {name} to a drawer on the hard disk, runs its
; installer from there and deletes the drawer again (PrismPack).
FailAt 21
Echo "Unpacking {name} to {temp} ..."
If EXISTS {temp}
  Delete >NIL: {temp} ALL QUIET FORCE
EndIf
PrismUnpack {name}.ppk TO {temp}
If WARN
  Echo "*NUnpacking failed - nothing was installed."
  Echo "{name} needs about {need} KB free on the hard disk for a moment."
  Delete >NIL: {temp} ALL QUIET FORCE
  Ask "Press RETURN to close this window."
  Quit 20
EndIf
CD {temp}/{name}
If EXISTS SYS:System/Installer
  SYS:System/Installer {installer} APPNAME "{name}" MINUSER AVERAGE
Else
  Which >NIL: Installer
  If WARN
    Echo "*NThis Amiga has no Installer program (C:Installer or SYS:System/Installer)."
    Echo "It comes with Workbench 3.1 and newer, and on Aminet as util/misc/Installer-43_3.lha."
    Ask "Press RETURN to close this window."
  Else
    Installer {installer} APPNAME "{name}" MINUSER AVERAGE
  EndIf
EndIf
CD SYS:
Delete >NIL: {temp} ALL QUIET FORCE
If EXISTS {temp}
  Echo "*N{temp} could not be removed yet: a program the installer started is"
  Echo "still using it. Delete that drawer after the next reboot."
  Ask "Press RETURN to close this window."
EndIf
"""


def floppy(src, outdir, icon, info, installer, unpacker, temp):
    name = os.path.basename(os.path.normpath(src))
    installer = installer or 'Install_' + name
    temp = temp or 'SYS:PrismPack-' + name
    os.makedirs(outdir, exist_ok=True)
    count, total, size = pack(src, os.path.join(outdir, name + '.ppk'), info)
    shutil.copyfile(unpacker, os.path.join(outdir, 'PrismUnpack'))
    script = STAGE1.format(name=name, installer=installer, temp=temp, need=(total + 1023) // 1024 + 64)
    open(os.path.join(outdir, installer), 'w', newline='\n').write(script)
    open(os.path.join(outdir, installer + '.info'), 'wb').write(
        retool(icon, 'IconX', ['WINDOW=CON:0/20/640/150/' + name + ' installer/AUTO/CLOSE', 'STACK=8192']))
    return count, total, size


def main():
    ap = argparse.ArgumentParser(description='PrismPack host tool')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pack'); p.add_argument('src'); p.add_argument('out'); p.add_argument('--info')
    p = sub.add_parser('list'); p.add_argument('archive')
    p = sub.add_parser('unpack'); p.add_argument('archive'); p.add_argument('dir')
    p = sub.add_parser('floppy'); p.add_argument('src'); p.add_argument('outdir')
    p.add_argument('--icon', required=True); p.add_argument('--info'); p.add_argument('--installer')
    p.add_argument('--unpacker', default=os.path.join(os.path.dirname(__file__), '..', 'out', 'PrismUnpack'))
    p.add_argument('--temp')
    a = ap.parse_args()
    if a.cmd == 'pack':
        n, total, size = pack(a.src, a.out, a.info)
        print('%s: %d entries, %d bytes -> %d (%.1f%%)' % (a.out, n, total, size, 100.0 * size / max(total, 1)))
    elif a.cmd == 'list':
        for e in read(a.archive):
            print('%-4s %8d %8d %08x  %s' % ('dir' if e['type'] == T_DIR else 'file', e['size'], e['packed'],
                                             e['crc'], e['name']))
    elif a.cmd == 'unpack':
        unpack(a.archive, a.dir)
    else:
        n, total, size = floppy(a.src, a.outdir, a.icon, a.info, a.installer, a.unpacker, a.temp)
        print('%s: %d entries, %d bytes -> %d (%.1f%%)' % (a.outdir, n, total, size, 100.0 * size / max(total, 1)))


if __name__ == '__main__':
    main()
