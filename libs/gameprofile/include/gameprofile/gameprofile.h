#ifndef GAMEPROFILE_H
#define GAMEPROFILE_H

#include <stdint.h>

#include <io/export.h>
#define GAMEPROFILE_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

/* Per-game profiles. A single "game" choice drives how an existing archive's contents are decoded
   (container key + SCR payload-codec policy) and how a new/edited archive is written. It is the one
   source of truth for game identity across the whole engine: the editor PFF tool picks a game by id,
   the runtime picks one by short `code` (the `/game <code>` launch flag), and the Python importer
   picks one by code over FFI — all resolve to the same `scr_policy` here. Dependency-free by design
   (no pff.h include): the format field is a plain int whose values mirror PffFormat. */

typedef enum NovaGameId {
    NOVA_GAME_JO = 0,    /* Joint Operations: Typhoon Rising            */
    NOVA_GAME_JO_DEMO,   /* Joint Operations demo                        */
    NOVA_GAME_DFX,       /* Delta Force: Xtreme                          */
    NOVA_GAME_DFX2,      /* Delta Force: Xtreme 2 (Combined Arms era)    */
    NOVA_GAME_BHD,       /* Delta Force: Black Hawk Down                 */
    NOVA_GAME_COUNT
} NovaGameId;

/* How SCR-wrapped payloads are keyed. Every shipping game decodes by the SCR version byte
   (libs/scr + opennova::vfs_decode_payload); the FORCE_* values are headroom for a title that
   ever needs a fixed key. */
typedef enum ScrPolicy {
    SCR_POLICY_VERSION_DETECT = 0,
    SCR_POLICY_FORCE_DEFAULT,
    SCR_POLICY_FORCE_JO_DFX2,
    SCR_POLICY_FORCE_SHADERS
} ScrPolicy;

typedef struct NovaGameProfile {
    int         id;             /* NovaGameId                                                    */
    const char *code;           /* short launch-flag / FFI token, lowercase (e.g. "jo", "jodemo") */
    const char *display_name;   /* artist-facing label                                           */
    uint32_t    container_key;  /* ROL7 XOR seed for PFF_FLAG_ENCRYPTED entries (all = 0x0312A4CE
                                   today; verified vs PFF_LoadFileToMemory @ 0x768920)            */
    int         scr_policy;     /* ScrPolicy                                                     */
    int         bfc1_compress;  /* always 0 — no BFC1 compressor exists                          */
    int         default_format; /* PffFormat for a NEW archive (0=PFF3, 1=PFF4, 2=BHD)            */
} NovaGameProfile;

/* Number of profiles in the table. */
GAMEPROFILE_EXPORT int gameprofile_count(void);

/* Profile at table index in [0, gameprofile_count()); NULL if out of range. */
GAMEPROFILE_EXPORT const NovaGameProfile *gameprofile_at(int index);

/* Profile for a NovaGameId; NULL if unknown. */
GAMEPROFILE_EXPORT const NovaGameProfile *gameprofile_by_id(int game_id);

/* Profile whose `code` matches (case-insensitive); NULL for NULL/unknown code. */
GAMEPROFILE_EXPORT const NovaGameProfile *gameprofile_by_code(const char *code);

/* The SCR policy (ScrPolicy) for a game `code`. Returns SCR_POLICY_VERSION_DETECT for a
   NULL/unknown code, so a missing or bad `/game` value safely behaves like the JO default.
   This is the single game->policy seam the runtime and the Python importer share. */
GAMEPROFILE_EXPORT int gameprofile_scr_policy_for_code(const char *code);

#ifdef __cplusplus
}
#endif

#endif /* GAMEPROFILE_H */
