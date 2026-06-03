#ifndef MUS_SNIFF_H
#define MUS_SNIFF_H

/* Shared MUS detection/decryption used by the Godot loader, the VFS decode path,
   and the resource index, so the three can't drift apart on what counts as a
   "music script". A .bin can hold a MUS file in three on-disk forms:
     - plaintext "SCR0" (e.g. JO_CLIENT's PFF entries),
     - SCR-container wrapped ("SCR\xVV" header, scr_decrypt_buf),
     - headerless retail-disk form (no magic, scr_decrypt_mus).
   These helpers recognize all three. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if `data` is a MUS file in any of the three forms above, else 0. */
int mus_is_mus(const uint8_t *data, size_t size);

/* If `data` is a MUS file in any form, malloc `*out` with the plaintext "SCR0"
   form, set `*out_size`, and return 1. Otherwise return 0 and leave the outputs
   untouched. Free `*out` with mus_free() (plain free()). */
int mus_decode_to_scr0(const uint8_t *data, size_t size,
                       uint8_t **out, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif /* MUS_SNIFF_H */
