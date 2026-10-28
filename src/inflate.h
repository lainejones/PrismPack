/*
 * inflate.h - raw deflate (RFC 1951) decoder for PrismPack.
 * Part of PrismPack. GPL-3.0-only, see LICENSE.
 */
#ifndef PRISMPACK_INFLATE_H
#define PRISMPACK_INFLATE_H

/* Unpack the raw deflate stream in[0..inlen) into out[0..outlen).
 * Returns 0 when the stream ends having filled out exactly, else an
 * error code < 0 (see inflate.c). Not reentrant: the code tables are static. */
long pp_inflate(unsigned char *out, unsigned long outlen,
                const unsigned char *in, unsigned long inlen);

#endif
