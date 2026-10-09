#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>


namespace opennova::scr {

inline constexpr int SCR_HEADER_SIZE = 4;

/* Known encryption keys used by NovaLogic games. In Jointops.exe the key is
   per CALL SITE, not derived from the version byte (D-SCR-2): every
   File_ParseASCIIFile caller passes 0x2A5A8EAD (27 sites, .def text data),
   and the HLSL .fx loader hardcodes 0xA55B1EED
   [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060, key at 0x5AE0C0].
   0xABEEFACE does not appear in Jointops.exe at all (earlier-title key). */
inline constexpr uint32_t SCR_KEY_DEFAULT = 0xABEEFACEu;  /* JO Demo, most .def files */
inline constexpr uint32_t SCR_KEY_JO_DFX2 = 0x2A5A8EADu;  /* JO/DFX2 Combined Arms */
inline constexpr uint32_t SCR_KEY_SHADERS = 0xA55B1EEDu;  /* .fx shader files */

/* Check if data starts with a valid SCR container header.
   Returns 1 if SCR, 0 otherwise. */
int scr_is_scr(const uint8_t *data, size_t size);

/* Get version byte from SCR header.
   Returns version byte, or 0 if data is too small. */
uint8_t scr_get_version(const uint8_t *data, size_t size);

/* Decrypt payload in-place (caller must strip the 4-byte header first).
   Reverses bytes, then XORs with keystream derived from key. */
void scr_decrypt(uint8_t *data, size_t size, uint32_t key);

/* Encrypt payload in-place: scr_decrypt's inverse (the keystream XOR, then the bytes
   reversed), so scr_decrypt of what it writes gives the payload back. The game has no
   writer of its own: this is what a tool writes a file in the SCR form with (a shader,
   which the shader loader takes in that form alone: scr_shader_encode). */
void scr_encrypt(uint8_t *data, size_t size, uint32_t key);

/* Strip SCR header and decrypt into caller-provided buffer.
   On entry, *out_size is the buffer capacity.
   On success, *out_size is set to the actual decrypted size.
   Returns  0 on success,
           -1 if data is not SCR,
           -2 if output buffer is too small. */
int scr_decrypt_buf(const uint8_t *data, size_t size,
                    uint8_t *out, size_t *out_size, uint32_t key);

/* The HLSL shader loader's form: "SCR", version 1 alone, then the text and one NUL after it under
   SCR_KEY_SHADERS [orig: ScriptFile_LoadAndDecrypt @ 0x5AE060: the sniff for 'S','C','R',1 @ 0x5AE0A9,
   the key 0xA55B1EED at 0x5AE0C0, one trailing NUL dropped]. The loader rejects anything else, the
   version-detected key table's version 2 included (D-SCR-1/2: scr_is_scr's superset and the vfs
   payload decode's version map serve the other readers and titles). */
inline constexpr uint8_t SCR_SHADER_VERSION = 1;

/* A shader file's text as the shader loader reads it: false for bytes not in its form (not SCR, or a
   version other than 1). `nul`, when given, says whether one trailing NUL was dropped. */
bool scr_shader_decode(const uint8_t *data, size_t size, std::string &text, bool *nul = nullptr);

/* The file in the shader loader's form holding `text`, the NUL after it where `nul` (every shipped
   shader ends its text with one). The game has no writer of its own: scr_shader_decode reads back
   `text` and `nul`. */
std::vector<uint8_t> scr_shader_encode(const std::string &text, bool nul = true);

} // namespace opennova::scr
