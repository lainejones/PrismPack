/*
 * inflate.c - raw deflate (RFC 1951) decoder for PrismPack: one whole stream
 * from a buffer into a buffer whose size is known (the archive records it).
 * Written from the RFC. Codes up to FASTBITS long are looked up in a small
 * table; longer ones (rare) are decoded a bit at a time with the canonical
 * Huffman counts.
 *
 * Part of PrismPack. GPL-3.0-only, see LICENSE.
 */
#include "inflate.h"

#define MAXBITS   15        /* longest code */
#define MAXLCODES 286       /* literal/length codes */
#define MAXDCODES 30        /* distance codes */
#define FIXLCODES 288       /* fixed literal/length table size */

/* errors */
#define E_INPUT   -1        /* ran out of input */
#define E_OUTPUT  -2        /* output larger than the recorded size */
#define E_STORED  -3        /* stored block length check failed */
#define E_BTYPE   -4        /* reserved block type */
#define E_CODE    -5        /* invalid code */
#define E_TABLE   -6        /* bad code lengths in a dynamic block */
#define E_DIST    -7        /* distance before the start of the output */
#define E_SHORT   -8        /* stream ended before the output was full */

struct state {
    unsigned char *out;
    unsigned long outlen, outcnt;
    const unsigned char *in;
    unsigned long inlen, incnt;
    unsigned long bitbuf;
    int bitcnt;
    int err;
};

struct huffman {
    short *count;           /* codes of each length, [0..MAXBITS] */
    short *symbol;          /* symbols in canonical order */
    unsigned short *fast;   /* [1 << FASTBITS]: symbol | length << FASTBITS, 0 = longer code */
};

#define FASTBITS 10

/* need bits, LSB first */
static int bits(struct state *s, int need)
{
    unsigned long val = s->bitbuf;

    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) {
            s->err = E_INPUT;
            return 0;
        }
        val |= (unsigned long)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1UL << need) - 1));
}

/* one symbol: Huffman codes are sent most significant bit first */
static int decode(struct state *s, const struct huffman *h)
{
    int code = 0, first = 0, index = 0, len, count;

    while (s->bitcnt <= 24 && s->incnt < s->inlen) {
        s->bitbuf |= (unsigned long)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    if (s->bitcnt >= FASTBITS) {
        unsigned short e = h->fast[s->bitbuf & ((1 << FASTBITS) - 1)];
        if (e) {
            len = e >> FASTBITS;
            s->bitbuf >>= len;
            s->bitcnt -= len;
            return e & ((1 << FASTBITS) - 1);
        }
    }
    for (len = 1; len <= MAXBITS; len++) {
        if (s->bitcnt == 0) {
            if (s->incnt == s->inlen) {
                s->err = E_INPUT;
                return -1;
            }
            s->bitbuf = s->in[s->incnt++];
            s->bitcnt = 8;
        }
        code |= (int)(s->bitbuf & 1);
        s->bitbuf >>= 1;
        s->bitcnt--;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = E_CODE;
    return -1;
}

/* Canonical code from lengths. Returns 0 for a complete code, > 0 for an
 * incomplete one (allowed for single-code distance tables), < 0 if
 * over-subscribed. */
static int construct(struct huffman *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    int symbol, len, left;

    for (symbol = 0; symbol < (1 << FASTBITS); symbol++)
        h->fast[symbol] = 0;
    for (len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++)
        h->count[length[symbol]]++;
    if (h->count[0] == n)
        return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (short)symbol;

    /* the lookup table: each short code's bit-reversed value (the stream sends
     * it LSB first), repeated for every value of the bits after it */
    {
        unsigned short next[MAXBITS + 1];
        int i;

        next[1] = 0;
        for (len = 2; len <= MAXBITS; len++)
            next[len] = (unsigned short)((next[len - 1] + h->count[len - 1]) << 1);
        for (symbol = 0; symbol < n; symbol++) {
            int l = length[symbol], c, r = 0, k;
            if (l == 0)
                continue;
            c = next[l]++;
            if (l > FASTBITS)
                continue;
            for (k = 0; k < l; k++)
                r |= ((c >> k) & 1) << (l - 1 - k);
            for (i = r; i < (1 << FASTBITS); i += 1 << l)
                h->fast[i] = (unsigned short)(symbol | (l << FASTBITS));
        }
    }
    return left;
}

static const short lbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const short dext[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* Literals and length/distance pairs until end-of-block: the hot loop. The
 * bit buffer and the pointers live in locals; only a code longer than
 * FASTBITS (rare) goes through decode() with the state written back. */
static int codes(struct state *s, const struct huffman *lencode, const struct huffman *distcode)
{
    const unsigned char *in = s->in + s->incnt, *inend = s->in + s->inlen;
    unsigned char *out = s->out + s->outcnt, *outend = s->out + s->outlen, *outstart = s->out;
    const unsigned short *lfast = lencode->fast, *dfast = distcode->fast;
    unsigned long bitbuf = s->bitbuf;
    int bitcnt = s->bitcnt;
    int symbol, len, cl, n, err = 0;
    unsigned long dist;
    unsigned short e;

/* two bytes at a time while they are there: one variable 32-bit shift
 * (expensive on a 68000) per 16 bits instead of per 8 */
#define REFILL() \
    if (bitcnt <= 16 && inend - in >= 2) { \
        bitbuf |= ((unsigned long)in[0] | ((unsigned long)in[1] << 8)) << bitcnt; in += 2; bitcnt += 16; \
    } \
    while (bitcnt <= 24 && in < inend) { bitbuf |= (unsigned long)*in++ << bitcnt; bitcnt += 8; }
#define SAVE()    (s->incnt = in - s->in, s->outcnt = out - outstart, s->bitbuf = bitbuf, s->bitcnt = bitcnt)
#define RESTORE() (in = s->in + s->incnt, bitbuf = s->bitbuf, bitcnt = s->bitcnt)
/* a code: the table, else the bit-by-bit decoder */
#define DECODE(tab, h) \
    REFILL(); \
    e = bitcnt >= FASTBITS ? tab[bitbuf & ((1 << FASTBITS) - 1)] : 0; \
    if (e) { cl = e >> 10; bitbuf >>= cl; bitcnt -= cl; symbol = e & 1023; } \
    else { SAVE(); symbol = decode(s, h); RESTORE(); if (s->err) { err = s->err; goto done; } }
/* n extra bits (at most 13; REFILL left 25+ unless the input ended) */
#define EXTRA(n, v) \
    if (bitcnt < (n)) { err = E_INPUT; goto done; } \
    v = bitbuf & ((1UL << (n)) - 1); bitbuf >>= (n); bitcnt -= (n)

    for (;;) {
        DECODE(lfast, lencode);
        if (symbol < 256) {
            if (out == outend) { err = E_OUTPUT; break; }
            *out++ = (unsigned char)symbol;
        } else if (symbol == 256) {
            break;
        } else {
            symbol -= 257;
            if (symbol >= 29) { err = E_CODE; break; }
            n = lext[symbol];
            EXTRA(n, len);
            len += lbase[symbol];
            DECODE(dfast, distcode);
            if (symbol >= 30) { err = E_CODE; break; }
            n = dext[symbol];
            REFILL();
            EXTRA(n, dist);
            dist += dbase[symbol];
            if (dist > (unsigned long)(out - outstart)) { err = E_DIST; break; }
            if ((unsigned long)(outend - out) < (unsigned long)len) { err = E_OUTPUT; break; }
            {
                const unsigned char *f = out - dist;
                if (dist >= (unsigned long)len) {
                    do *out++ = *f++; while (--len);     /* no overlap */
                } else if (dist == 1) {
                    unsigned char c = *f;
                    do *out++ = c; while (--len);        /* a run */
                } else {
                    do *out++ = *f++; while (--len);     /* overlapping: byte by byte */
                }
            }
        }
    }
done:
    SAVE();
    return err;
#undef REFILL
#undef SAVE
#undef RESTORE
#undef DECODE
#undef EXTRA
}

static short lencnt[MAXBITS + 1], lensym[FIXLCODES];
static short distcnt[MAXBITS + 1], distsym[MAXDCODES];
static unsigned short lfast[1 << FASTBITS], dfast[1 << FASTBITS];
static short lengths[MAXLCODES + MAXDCODES];

static int stored(struct state *s)
{
    unsigned long len;

    /* to a byte boundary: drop the rest of this byte and give back the
     * whole bytes decode() read ahead */
    s->incnt -= (unsigned long)(s->bitcnt >> 3);
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen)
        return E_INPUT;
    len = s->in[s->incnt] | ((unsigned long)s->in[s->incnt + 1] << 8);
    if (s->in[s->incnt + 2] != (~len & 0xff) || s->in[s->incnt + 3] != ((~len >> 8) & 0xff))
        return E_STORED;
    s->incnt += 4;
    if (s->incnt + len > s->inlen)
        return E_INPUT;
    if (s->outcnt + len > s->outlen)
        return E_OUTPUT;
    while (len--)
        s->out[s->outcnt++] = s->in[s->incnt++];
    return 0;
}

static int fixed(struct state *s)
{
    struct huffman lencode = { lencnt, lensym, lfast }, distcode = { distcnt, distsym, dfast };
    int symbol;

    for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
    for (; symbol < 256; symbol++)           lengths[symbol] = 9;
    for (; symbol < 280; symbol++)           lengths[symbol] = 7;
    for (; symbol < FIXLCODES; symbol++)     lengths[symbol] = 8;
    construct(&lencode, lengths, FIXLCODES);
    for (symbol = 0; symbol < MAXDCODES; symbol++)
        lengths[symbol] = 5;
    construct(&distcode, lengths, MAXDCODES);
    return codes(s, &lencode, &distcode);
}

static int dynamic(struct state *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    struct huffman lencode = { lencnt, lensym, lfast }, distcode = { distcnt, distsym, dfast };
    int nlen, ndist, ncode, index, err;

    nlen  = bits(s, 5) + 257;
    ndist = bits(s, 5) + 1;
    ncode = bits(s, 4) + 4;
    if (s->err)
        return s->err;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return E_TABLE;
    for (index = 0; index < ncode; index++)
        lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    if (s->err)
        return s->err;
    if (construct(&lencode, lengths, 19) != 0)
        return E_TABLE;                     /* the code length code must be complete */

    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode), len = 0;
        if (s->err)
            return s->err;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
            continue;
        }
        if (symbol == 16) {
            if (index == 0)
                return E_TABLE;
            len = lengths[index - 1];
            symbol = 3 + bits(s, 2);
        } else if (symbol == 17) {
            symbol = 3 + bits(s, 3);
        } else {
            symbol = 11 + bits(s, 7);
        }
        if (s->err)
            return s->err;
        if (index + symbol > nlen + ndist)
            return E_TABLE;
        while (symbol--)
            lengths[index++] = (short)len;
    }
    if (lengths[256] == 0)
        return E_TABLE;                     /* no end-of-block code */

    err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
        return E_TABLE;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
        return E_TABLE;
    return codes(s, &lencode, &distcode);
}

long pp_inflate(unsigned char *out, unsigned long outlen,
                const unsigned char *in, unsigned long inlen)
{
    struct state s;
    int last, type, err;

    s.out = out; s.outlen = outlen; s.outcnt = 0;
    s.in = in;   s.inlen = inlen;   s.incnt = 0;
    s.bitbuf = 0; s.bitcnt = 0; s.err = 0;
    do {
        last = bits(&s, 1);
        type = bits(&s, 2);
        if (s.err)
            return s.err;
        if (type == 0)      err = stored(&s);
        else if (type == 1) err = fixed(&s);
        else if (type == 2) err = dynamic(&s);
        else                err = E_BTYPE;
        if (err)
            return err;
    } while (!last);
    return s.outcnt == outlen ? 0 : E_SHORT;
}
