/*
 * PrismUnpack - unpacks a PrismPack (PPK1) archive: drawers, files, their
 * protection bits, dates and comments. Raw deflate per file, CRC-32 checked.
 * AmigaOS 3.1 or newer, any 68k CPU.
 *
 *   PrismUnpack ARCHIVE/A [TO dir] [LIST] [TEST] [QUIET]
 *
 * TO is created if it is missing (one level). Before writing anything it
 * checks that TO's volume has room for the whole archive. LIST shows the
 * entries, TEST checks every file without writing. Return codes: 0 done,
 * 10 an error (the message says which), 20 bad arguments.
 *
 * Part of PrismPack. GPL-3.0-only, see LICENSE.
 */
#include <string.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "inflate.h"

static const char ver[] __attribute__((used)) = "$VER: PrismUnpack 1.0 (28.10.2026) PrismPack";

#define TEMPLATE "ARCHIVE/A,TO/K,LIST/S,TEST/S,QUIET/S"
enum { A_ARCHIVE, A_TO, A_LIST, A_TEST, A_QUIET, A_N };

#define T_DIR   1
#define T_FILE  2
#define M_STORED  0
#define M_DEFLATE 8

#define ENTRY_SIZE 36

static ULONG crctab[256];

static void crc_init(void)
{
    ULONG c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (ULONG)n;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crctab[n] = c;
    }
}

static ULONG crc32(const UBYTE *p, ULONG n)
{
    ULONG c = 0xffffffffUL;
    while (n--)
        c = crctab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffUL;
}

static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }
static UWORD be16(const UBYTE *p) { return (UWORD)((p[0] << 8) | p[1]); }

static BPTR arch;
static char name[256], comment[84], path[512];

static BOOL readn(void *buf, LONG n)
{
    return n == 0 || Read(arch, buf, n) == n;
}

static BOOL skipn(LONG n)
{
    return n == 0 || Seek(arch, n, OFFSET_CURRENT) != -1;
}

/* the volume TO is on has room for need bytes plus a block per entry */
static BOOL room(BPTR lock, ULONG need, ULONG entries)
{
    struct InfoData *id = AllocVec(sizeof(*id), MEMF_PUBLIC);
    ULONG freeb, needb;
    BOOL ok = TRUE;

    if (!id)
        return TRUE;                    /* can't tell: the writes will */
    /* the RAM disk grows as needed and always reports itself full: no check */
    if (Info(lock, id) && id->id_NumBlocksUsed < id->id_NumBlocks) {
        ULONG bpb = id->id_BytesPerBlock ? id->id_BytesPerBlock : 512;
        freeb = id->id_NumBlocks - id->id_NumBlocksUsed;
        needb = need / bpb + 2 * entries + 16;
        if (freeb < needb) {
            Printf("PrismUnpack: not enough room: needs %lu KB, there are %lu KB free\n",
                   (needb * bpb + 1023) / 1024, (freeb * bpb) / 1024);
            ok = FALSE;
        }
    }
    FreeVec(id);
    return ok;
}

int main(void)
{
    LONG args[A_N] = { 0 };
    struct RDArgs *rda;
    UBYTE head[16], e[ENTRY_SIZE];
    ULONG count, total, i, files = 0, bytes = 0;
    const char *to;
    BPTR tolock = 0;
    BOOL list, test, quiet;
    int rc = RETURN_OK;

    if (!(rda = ReadArgs((CONST_STRPTR)TEMPLATE, args, NULL))) {
        PrintFault(IoErr(), (CONST_STRPTR)"PrismUnpack");
        return RETURN_FAIL;
    }
    to = args[A_TO] ? (const char *)args[A_TO] : "";
    list = args[A_LIST] != 0;
    test = args[A_TEST] != 0;
    quiet = args[A_QUIET] != 0;
    crc_init();

    if (!(arch = Open((CONST_STRPTR)args[A_ARCHIVE], MODE_OLDFILE))) {
        PrintFault(IoErr(), (CONST_STRPTR)args[A_ARCHIVE]);
        FreeArgs(rda);
        return RETURN_ERROR;
    }
    if (!readn(head, 16) || memcmp(head, "PPK1", 4) || be16(head + 4) != 1) {
        Printf("PrismUnpack: %s is not a PrismPack archive\n", args[A_ARCHIVE]);
        rc = RETURN_ERROR;
        goto out;
    }
    count = be32(head + 8);
    total = be32(head + 12);

    if (!list && !test) {
        if (to[0]) {
            tolock = Lock((CONST_STRPTR)to, SHARED_LOCK);
            if (!tolock && (tolock = CreateDir((CONST_STRPTR)to)) != 0) {
                UnLock(tolock);
                tolock = Lock((CONST_STRPTR)to, SHARED_LOCK);
            }
            if (!tolock) {
                PrintFault(IoErr(), (CONST_STRPTR)to);
                rc = RETURN_ERROR;
                goto out;
            }
        } else {
            tolock = Lock((CONST_STRPTR)"", SHARED_LOCK);
        }
        if (tolock && !room(tolock, total, count)) {
            rc = RETURN_ERROR;
            goto out;
        }
    }

    for (i = 0; i < count; i++) {
        ULONG nlen, clen, size, psize, crc, prot;
        UBYTE type, method;
        struct DateStamp ds;

        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
            PrintFault(ERROR_BREAK, (CONST_STRPTR)"PrismUnpack");
            rc = RETURN_ERROR;
            break;
        }
        if (!readn(e, ENTRY_SIZE))
            goto damaged;
        type = e[0];
        method = e[1];
        nlen = be16(e + 2);
        prot = be32(e + 4);
        ds.ds_Days = be32(e + 8);
        ds.ds_Minute = be32(e + 12);
        ds.ds_Tick = be32(e + 16);
        clen = be16(e + 20);
        size = be32(e + 24);
        psize = be32(e + 28);
        crc = be32(e + 32);
        if (nlen == 0 || nlen >= sizeof(name) || clen >= sizeof(comment) || !readn(name, nlen) || !readn(comment, clen))
            goto damaged;
        name[nlen] = 0;
        comment[clen] = 0;

        if (list) {
            Printf("%-4s %8lu %8lu  %s\n", (ULONG)(type == T_DIR ? "dir" : "file"), size, psize, (ULONG)name);
            if (!skipn(psize))
                goto damaged;
            continue;
        }
        path[0] = 0;
        if (to[0])
            strncpy(path, to, sizeof(path) - 1);
        if (!AddPart((STRPTR)path, (STRPTR)name, sizeof(path))) {
            Printf("PrismUnpack: name too long: %s\n", (ULONG)name);
            rc = RETURN_ERROR;
            break;
        }

        if (type == T_DIR) {
            if (!test) {
                BPTR l = Lock((CONST_STRPTR)path, SHARED_LOCK);
                if (!l && !(l = CreateDir((CONST_STRPTR)path))) {
                    PrintFault(IoErr(), (CONST_STRPTR)path);
                    rc = RETURN_ERROR;
                    break;
                }
                UnLock(l);
                SetProtection((CONST_STRPTR)path, prot);
                SetFileDate((CONST_STRPTR)path, &ds);
                if (clen)
                    SetComment((CONST_STRPTR)path, (CONST_STRPTR)comment);
            }
            if (!skipn(psize))
                goto damaged;
            continue;
        }
        if (type != T_FILE || (method != M_STORED && method != M_DEFLATE) ||
            (method == M_STORED && psize != size))
            goto damaged;

        {
            UBYTE *pk = NULL, *data = NULL;
            BOOL ok = FALSE;

            if (psize && !(pk = AllocVec(psize, MEMF_ANY))) {
                Printf("PrismUnpack: out of memory for %s (%lu bytes)\n", (ULONG)name, psize);
                rc = RETURN_ERROR;
                break;
            }
            if (!readn(pk, psize)) {
                FreeVec(pk);
                goto damaged;
            }
            if (method == M_STORED) {
                data = pk;
            } else if (size && !(data = AllocVec(size, MEMF_ANY))) {
                Printf("PrismUnpack: out of memory for %s (%lu bytes)\n", (ULONG)name, size);
                FreeVec(pk);
                rc = RETURN_ERROR;
                break;
            }
            if (method == M_STORED || pp_inflate(data, size, pk, psize) == 0)
                ok = crc32(data, size) == crc;
            if (data != pk)
                FreeVec(pk);
            if (!ok) {
                Printf("PrismUnpack: %s is damaged in the archive\n", (ULONG)name);
                FreeVec(data);
                rc = RETURN_ERROR;
                break;
            }
            if (!test) {
                BPTR fh = Open((CONST_STRPTR)path, MODE_NEWFILE);
                if (!fh || Write(fh, data, size) != (LONG)size) {
                    LONG err = IoErr();
                    if (fh)
                        Close(fh);
                    PrintFault(err, (CONST_STRPTR)path);
                    FreeVec(data);
                    rc = RETURN_ERROR;
                    break;
                }
                Close(fh);
                SetProtection((CONST_STRPTR)path, prot);
                SetFileDate((CONST_STRPTR)path, &ds);
                if (clen)
                    SetComment((CONST_STRPTR)path, (CONST_STRPTR)comment);
            }
            FreeVec(data);
        }
        files++;
        bytes += size;
        if (!quiet)
            Printf("  %s\n", (ULONG)name);
    }
    if (rc == RETURN_OK && !list)
        Printf("PrismUnpack: %lu files, %lu KB %s\n", files, (bytes + 1023) / 1024,
               (ULONG)(test ? "checked, all intact" : "unpacked"));
    goto out;

damaged:
    Printf("PrismUnpack: %s is damaged or cut short\n", args[A_ARCHIVE]);
    rc = RETURN_ERROR;
out:
    if (tolock)
        UnLock(tolock);
    Close(arch);
    FreeArgs(rda);
    return rc;
}
