#pragma once

namespace opennova::gameprofile {

/* The witnessed boot-required resource manifest (ENG-6): every file
   Jointops.exe demands by literal name to boot to the main menu and start a
   mission, with the witnessed failure class for each. Generated from the R8
   record — docs/required-resources.md is the witness source; this table is
   the engine-side manifest consumed by the game's boot validation and test
   diagnostics. Rows are ordered by the witnessed boot sequence
   (phase-major).

   These functions are internal to the native engine (ADR 0024). */

/* The witnessed load phase [orig: Game_Run @ 0x4a7fb0 -> Game_InitSubsystems
   @ 0x4a6cd0; MainMenu enter Menu_InitShellResources @ 0x552500; Game_StartMission
   @ 0x524360]. */
typedef enum BootPhase {
    BOOT_PHASE_BOOT = 0,   /* WinMain -> Game_InitSubsystems            */
    BOOT_PHASE_MENU,       /* main-menu enter                           */
    BOOT_PHASE_MISSION     /* Game_StartMission                         */
} BootPhase;

/* Witnessed failure class when the resource is missing. */
typedef enum ResourceSeverity {
    RES_FATAL = 0,  /* boot exits, or dead-ends with no path forward (main.mnu) */
    RES_DIALOG,     /* error dialog, then boot continues (gameerr.bin)          */
    RES_REQUIRED,   /* loads unchecked / silent-skips but the feature is        */
                         /* non-functional or visibly degraded without it            */
    RES_SOFT,       /* logged to _errlog.txt, continues                         */
    RES_OPTIONAL    /* silent skip / graceful fallback / lazy                   */
} ResourceSeverity;

enum {
    /* `name` is a pattern or template (wildcards / <placeholders>), not a
       literal probeable filename. */
    RES_F_PATTERN = 1 << 0,
    /* Member of the witnessed boot archive table: fatal only when ZERO table
       archives open [orig: PFF_OpenAllArchives @ 0x4a4310 over the name
       table @ 0x829f90; fatal check @ 0x4a6f44]. Any individual archive may
       be absent. */
    RES_F_PFF_TABLE_ANY = 1 << 1,
    /* The player's or this machine's own file, read from the working directory
       and written there by the game or by its setup: a save, a configuration,
       the stored NovaWorld credentials, the high-score table. Never a resource
       a game is made of: the editor never imports one nor packs one into a
       build (ADR 0046 S14), and its checklist does not list one. */
    RES_F_PLAYER_FILE = 1 << 2,
    /* A file an expansion's own name forms, which the game opens only under
       `/exp <n>` [orig: Expansion_LoadAssets @ 0x4a4730, the expansion arm
       @ 0x4a4858..0x4a49de]: `name` spells it with `<n>` (with RES_F_PATTERN,
       so no reader takes it as a literal). The editor's checklist forms it for
       a project that builds as an expansion (ADR 0046 S16,
       editor/project/expansion_files). */
    RES_F_EXPANSION = 1 << 3
};

typedef struct RequiredResource {
    const char *name;     /* the literal (or pattern when F_PATTERN)         */
    int phase;            /* BootPhase                                   */
    int severity;         /* ResourceSeverity                            */
    int flags;            /* RES_F_*                                    */
    const char *failure;  /* the witnessed failure behavior, quotable in an  */
                          /* honest missing-resource error                   */
    const char *orig;     /* the [orig: ...] witness citation                */
    const char *role;     /* the stable snake_case token the editor keys the */
                          /* row by (ADR 0046 d5/d7): unique, never renamed, */
                          /* independent of the file name it requires        */
} RequiredResource;

/* Number of manifest rows. */
int gameprofile_required_resource_count(void);

/* Row at index in [0, count); NULL if out of range. Rows are phase-major in
   the witnessed load order. */
const RequiredResource *gameprofile_required_resource_at(int index);

/* Row whose `name` matches case-insensitively (the engine's own name
   handling is case-insensitive); NULL for NULL/unknown names. Pattern rows
   only match their literal spelling. */
const RequiredResource *gameprofile_required_resource_find(const char *name);

/* Row whose `role` token matches exactly; NULL for NULL/unknown roles. */
const RequiredResource *gameprofile_required_resource_by_role(const char *role);

/* The boot report's marker: the game names each missing fatal-set file on a line of its
   log where the file's name follows this text (godot/game/boot_root_mount.gd writes it
   through ResourceRoot.boot_resource_missing_marker()), and the editor's Play reads the
   names back from that log (editor/session/project_session). */
inline constexpr const char *kBootResourceMissingMarker = "boot-required resource missing: ";

/* The launch mission's report: a game started in a mission (`--mission`) that does not load
   says so on a line of its log where the mission's file name, a space and the reason follow
   this text (godot/game/main_game.gd writes it through
   ResourceRoot.launch_mission_failed_marker()), and the editor's Play reads it back from that
   log (editor/session/play_controller). */
inline constexpr const char *kLaunchMissionFailedMarker = "launch mission failed: ";

} // namespace opennova::gameprofile
