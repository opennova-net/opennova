#ifndef GAMEPROFILE_H
#define GAMEPROFILE_H

#include <stdint.h>

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define GAMEPROFILE_EXPORT __declspec(dllexport)
#  else
#    define GAMEPROFILE_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define GAMEPROFILE_EXPORT __attribute__((visibility("default")))
#  else
#    define GAMEPROFILE_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Per-game profiles for the PFF archive tool. A single "game" choice has to drive both how an
   existing archive's contents are decoded and how a new/edited archive is written, so the
   profile bundles the container-encryption key, the SCR payload-codec policy, and the default
   container format. Dependency-free by design (no pff.h include): the format field is a plain
   int whose values mirror PffFormat, so a future CLI / export packer can share this table. */

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

#ifdef __cplusplus
}
#endif

#endif /* GAMEPROFILE_H */
