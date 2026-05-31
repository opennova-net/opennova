#ifndef SCR_H
#define SCR_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define SCR_EXPORT __declspec(dllexport)
#  else
#    define SCR_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define SCR_EXPORT __attribute__((visibility("default")))
#  else
#    define SCR_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SCR_HEADER_SIZE 4

/* Known encryption keys used by NovaLogic games */
#define SCR_KEY_DEFAULT  0xABEEFACEu  /* JO Demo, most .def files */
#define SCR_KEY_JO_DFX2  0x2A5A8EADu  /* JO/DFX2 Combined Arms */
#define SCR_KEY_SHADERS  0xA55B1EEDu  /* .fx shader files */

/* Check if data starts with "SCR" magic.
   Returns 1 if SCR, 0 otherwise. */
SCR_EXPORT int scr_is_scr(const uint8_t *data, size_t size);

/* Get version byte from SCR header.
   Returns version byte, or 0 if data is too small. */
SCR_EXPORT uint8_t scr_get_version(const uint8_t *data, size_t size);

/* Decrypt payload in-place (caller must strip the 4-byte header first).
   Reverses bytes, then XORs with keystream derived from key. */
SCR_EXPORT void scr_decrypt(uint8_t *data, size_t size, uint32_t key);

/* Strip SCR header and decrypt into caller-provided buffer.
   On entry, *out_size is the buffer capacity.
   On success, *out_size is set to the actual decrypted size.
   Returns  0 on success,
           -1 if data is not SCR,
           -2 if output buffer is too small. */
SCR_EXPORT int scr_decrypt_buf(const uint8_t *data, size_t size,
                    uint8_t *out, size_t *out_size, uint32_t key);

/* Decrypt a NovaLogic MUS-style encrypted blob: no SCR magic header on disk,
 * apply XOR keystream then reverse, prepend literal "SCR0" magic.
 * This is the music-file (gamemus.bin / menumus.bin) variant of SCR encryption,
 * distinct from .def / .fx files which carry a leading "SCR\xVV" header.
 * Witnessed: dfvas!Scr_DecryptBuffer @ 0x4cc250 with key 0x2A5A8EAD.
 *
 * Allocates output buffer at *out (size: in_size + 4). Caller frees with
 * scr_free_buffer(). Returns 0 on success, -1 on input/output errors,
 * -3 on allocation failure. */
SCR_EXPORT int scr_decrypt_mus(const uint8_t *in, size_t in_size,
                                uint8_t **out, size_t *out_size, uint32_t key);

/* Free a buffer allocated by scr_decrypt_mus. Safe to call with NULL. */
SCR_EXPORT void scr_free_buffer(uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* SCR_H */
