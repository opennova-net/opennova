#pragma once

#include <stdint.h>

#include <base/vfs/vfs_decode.h> // VfsScrPolicy, a profile's SCR policy


namespace opennova::gameprofile {

/* Per-game profiles. A single "game" choice drives how an existing archive's contents are decoded
   (the SCR payload-codec policy) and how a new/edited archive is written. It is the one
   source of truth for game identity across the whole engine: tools pick a game by id and the
   runtime picks one by short `code` (the `/game <code>` launch flag). Dependency-free by design
   (no pff.h include): the archive format is a plain int whose values mirror PffFormat. */

/* The container format a NEW archive of any profile is written in, as a PffFormat value (0 =
   PFF_FORMAT_PFF3): PFF3, ADR 0008's default for a newly created archive, which every JO-family
   title opens [orig: PFF_Open @ 0x7682e0]. One value for every profile: a title that ships
   another flavour makes it a GameProfile column. */
inline constexpr int GAMEPROFILE_PFF_NEW_ARCHIVE_FORMAT = 0;

typedef enum GameId {
    GAME_JO = 0,    /* Joint Operations: Typhoon Rising            */
    GAME_JO_DEMO,   /* Joint Operations demo                        */
    GAME_DFX,       /* Delta Force: Xtreme                          */
    GAME_DFX2,      /* Delta Force: Xtreme 2 (Combined Arms era)    */
    GAME_BHD,       /* Delta Force: Black Hawk Down                 */
    GAME_COUNT
} GameId;

typedef struct GameProfile {
    int         id;             /* GameId                                                    */
    const char *code;           /* short launch-flag token, lowercase (e.g. "jo", "jodemo") */
    const char *display_name;   /* artist-facing label                                           */
    int         scr_policy;     /* VfsScrPolicy (base/vfs/vfs_decode.h): how SCR payloads key    */
    const char *version_text;   /* the version text the profile's binary prints at startup, its
                                   build's four numbers through sprintf("V%i.%i.%i.%i") (the
                                   STARTUP screen's VERSION label, the joiner's VERSIONSTRING);
                                   NULL where no binary of the profile has been read            */
} GameProfile;

/* Number of profiles in the table. */
int gameprofile_count(void);

/* Profile at table index in [0, gameprofile_count()); NULL if out of range. */
const GameProfile *gameprofile_at(int index);

/* Profile for a GameId; NULL if unknown. */
const GameProfile *gameprofile_by_id(int game_id);

/* Profile whose `code` matches (case-insensitive); NULL for NULL/unknown code. */
const GameProfile *gameprofile_by_code(const char *code);

/* The SCR policy (VfsScrPolicy) for a game `code`. Returns VFS_SCR_VERSION_DETECT for a
   NULL/unknown code, so a missing or bad `/game` value safely behaves like the JO default.
   This is the engine's single game-to-policy seam. */
int gameprofile_scr_policy_for_code(const char *code);

/* The version text (GameProfile::version_text) for a game `code`: JO's for a NULL/unknown code,
   as gameprofile_scr_policy_for_code defaults; NULL for a profile whose binary is unwitnessed,
   which then shows no version rather than another game's. */
const char *gameprofile_version_text_for_code(const char *code);

} // namespace opennova::gameprofile
