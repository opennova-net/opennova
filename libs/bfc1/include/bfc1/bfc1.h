#ifndef BFC1_H
#define BFC1_H

#include <stddef.h>
#include <stdint.h>

#include <io/export.h>
#define BFC1_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

#define BFC1_MAGIC       0x31434642u  /* "BFC1" little-endian */
#define BFC1_HEADER_SIZE 8

/* Check if data starts with BFC1 magic. */
BFC1_EXPORT int bfc1_is_bfc1(const uint8_t *data, size_t size);

/* Decompress BFC1 data from memory.
   out_buf must be at least *out_size bytes.
   On entry, *out_size is the buffer capacity.
   On success, *out_size is set to the actual decompressed size.
   Returns 0 on success, non-zero on error. */
BFC1_EXPORT int bfc1_decompress(const uint8_t *data, size_t size,
                    uint8_t *out_buf, size_t *out_size);

/* Get the uncompressed size from a BFC1 header without decompressing.
   Returns 0 on success (writes to *out_size), non-zero if not BFC1. */
BFC1_EXPORT int bfc1_uncompressed_size(const uint8_t *data, size_t size,
                           uint32_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* BFC1_H */
