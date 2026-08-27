#ifndef GAMEPROFILE_H
#define GAMEPROFILE_H

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

/* Per-game profiles. A single "game" choice drives how an existing archive's contents are decoded
   (container key + SCR payload-codec policy) and how a new/edited archive is written. It is the one
   source of truth for game identity across the whole engine: tools pick a game by id and the
   runtime picks one by short `code` (the `/game <code>` launch flag). Dependency-free by design
   (no pff.h include): the format field is a plain int whose values mirror PffFormat. */

/* The universal PFF container key: the ROL7 XOR keystream seed for
   PFF_FLAG_ENCRYPTED entries, identical in every reversed NovaLogic title
   [orig: PFF_LoadFileToMemory @ 0x768920]. Per-game rows carry it so a future
   title only needs a new table entry; consumers with no resolved profile fall
   back to this named default (never to a raw literal). */
#define GAMEPROFILE_PFF_CONTAINER_KEY_DEFAULT 0x0312A4CEu

typedef enum GameId {
    GAME_JO = 0,    /* Joint Operations: Typhoon Rising            */
    GAME_JO_DEMO,   /* Joint Operations demo                        */
    GAME_DFX,       /* Delta Force: Xtreme                          */
    GAME_DFX2,      /* Delta Force: Xtreme 2 (Combined Arms era)    */
    GAME_BHD,       /* Delta Force: Black Hawk Down                 */
    GAME_COUNT
} GameId;

/* How SCR-wrapped payloads are keyed. Every shipping game decodes by the SCR version byte
   (engine/formats/scr + opennova::vfs_decode_payload); the FORCE_* values are headroom for a title that
   ever needs a fixed key. */
typedef enum ScrPolicy {
    SCR_POLICY_VERSION_DETECT = 0,
    SCR_POLICY_FORCE_DEFAULT,
    SCR_POLICY_FORCE_JO_DFX2,
    SCR_POLICY_FORCE_SHADERS
} ScrPolicy;

typedef struct GameProfile {
    int         id;             /* GameId                                                    */
    const char *code;           /* short launch-flag token, lowercase (e.g. "jo", "jodemo") */
    const char *display_name;   /* artist-facing label                                           */
    uint32_t    container_key;  /* ROL7 XOR seed for PFF_FLAG_ENCRYPTED entries (all = 0x0312A4CE
                                   today; verified vs PFF_LoadFileToMemory @ 0x768920)            */
    int         scr_policy;     /* ScrPolicy                                                     */
    int         bfc1_compress;  /* always 0 — no BFC1 compressor exists                          */
    int         default_format; /* PffFormat for a NEW archive (0=PFF3, 1=PFF4, 2=BHD)            */
} GameProfile;

/* Number of profiles in the table. */
int gameprofile_count(void);

/* Profile at table index in [0, gameprofile_count()); NULL if out of range. */
const GameProfile *gameprofile_at(int index);

/* Profile for a GameId; NULL if unknown. */
const GameProfile *gameprofile_by_id(int game_id);

/* Profile whose `code` matches (case-insensitive); NULL for NULL/unknown code. */
const GameProfile *gameprofile_by_code(const char *code);

/* The SCR policy (ScrPolicy) for a game `code`. Returns SCR_POLICY_VERSION_DETECT for a
   NULL/unknown code, so a missing or bad `/game` value safely behaves like the JO default.
   This is the engine's single game-to-policy seam. */
int gameprofile_scr_policy_for_code(const char *code);

#ifdef __cplusplus
}
#endif

#endif /* GAMEPROFILE_H */
