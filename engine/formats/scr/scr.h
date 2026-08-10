#ifndef SCR_H
#define SCR_H

#include <stddef.h>
#include <stdint.h>

#include <io/export.h>
#define SCR_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

#define SCR_HEADER_SIZE 4

/* Known encryption keys used by NovaLogic games. In Jointops.exe the key is
   per CALL SITE, not derived from the version byte (D-SCR-2): every
   File_ParseASCIIFile caller passes 0x2A5A8EAD (27 sites, .def text data),
   and the HLSL .fx loader hardcodes 0xA55B1EED
   [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060, key at 0x5AE0C0].
   0xABEEFACE does not appear in Jointops.exe at all (earlier-title key). */
#define SCR_KEY_DEFAULT  0xABEEFACEu  /* JO Demo, most .def files */
#define SCR_KEY_JO_DFX2  0x2A5A8EADu  /* JO/DFX2 Combined Arms */
#define SCR_KEY_SHADERS  0xA55B1EEDu  /* .fx shader files */

/* Check if data starts with a valid SCR container header.
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

#ifdef __cplusplus
}
#endif

#endif /* SCR_H */
