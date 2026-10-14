/* Avatars.def parser + writer — pure C API.
 * Stable plain C structs for native and Godot runtime consumers.
 *
 * Avatars.def defines selectable player characters in Joint Operations: a pool
 * of modular head/body/arms PARTS composed into COMBO entries grouped under a
 * NATIONALITY -> DIVISION tree. Faithful port of the original engine loader;
 * the witnessed format/struct spec and [orig:] citations live in
 * docs/playerinfo/avatars-re.md.
 *
 * Model note: the original runtime DENORMALIZES each combo's resolved part data
 * into the combo and drops the referenced part names (docs/playerinfo/avatars-re.md
 * D-PLAYERINFO-4). This authoring model keeps the head/body/arms NAME
 * references for writing, but also stores parse-time resolved snapshots so
 * consumers see the same duplicate/forward-reference behavior as the original,
 * and where each nationality stands among the parts, which decides what its
 * combos' names bind (AvatarNationality::parts_below, D-PLAYERINFO-35).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace opennova::avatars {

/* ========================================================================= */
/* Enums (witnessed; see docs/playerinfo/avatars-re.md)                      */
/* ========================================================================= */

enum AvatarPartKind {
    AVATAR_PART_HEAD = 0,
    AVATAR_PART_BODY = 1,
    AVATAR_PART_ARMS = 2
};

enum AvatarSex {
    AVATAR_SEX_MALE = 0,
    AVATAR_SEX_FEMALE = 1
};

enum AvatarAlignment {
    AVATAR_ALIGN_GOOD = 0,
    AVATAR_ALIGN_EVIL = 1
};

enum AvatarDiagnosticSeverity {
    AVATAR_DIAG_WARNING = 1,
    AVATAR_DIAG_ERROR = 2
};

typedef struct AvatarDiagnostic {
    size_t line;              /* 1-based source line; 0 when synthesized */
    int severity;             /* AvatarDiagnosticSeverity */
    char code[32];            /* stable machine-readable code */
    char message[192];        /* short human-readable summary */
} AvatarDiagnostic;

/* ========================================================================= */
/* Part: `define head|body|arms <name> { ... }`                              */
/* ========================================================================= */

typedef struct AvatarPart {
    int kind;                 /* AvatarPartKind */
    char name[64];            /* the `define` identifier (combo lookup key)      [orig @ 0x57a4ed] */
    char display_name[64];    /* `name` keyword — an "Avatars" RTXT string key   [orig @ 0x57ab09] */
    char graphic[128];        /* `graphic` (and `graphic_d` alias, D-PLAYERINFO-3) [orig @ 0x57ab47] */
    char graphic_j[128];      /* `graphic_j`                                      [orig @ 0x57abc3] */
    char graphic_s[128];      /* `graphic_s`                                      [orig @ 0x57ac03] */
    int camo[3];              /* `camo r g b` (small variant indices)            [orig @ 0x57ac50] */
    int voice;                /* `voice`                                          [orig @ 0x57acc0] */
    int sex;                  /* AvatarSex                                        [orig @ 0x57acec] */
} AvatarPart;

typedef struct AvatarPartSnapshot {
    int kind;                 /* AvatarPartKind */
    char name[64];
    char display_name[64];
    char graphic[128];
    char graphic_j[128];
    char graphic_s[128];
    int camo[3];
    int voice;
    int sex;
} AvatarPartSnapshot;

/* ========================================================================= */
/* Combo: `combo <id> <head> <body> <arms>` (arms optional)                  */
/* ========================================================================= */

typedef struct AvatarCombo {
    int id;                   /* the id token (token 1, kept @ 0x57a815)'s atol, written as its
                                 decimal                                          [orig @ 0x57a90d] */
    char head_name[64];       /* reference part names (D-PLAYERINFO-4)           [orig @ 0x57a804..0x57a809] */
    char body_name[64];
    char arms_name[64];       /* empty if the combo has no arms */
    AvatarPartSnapshot head;  /* parse-time resolved part snapshots */
    AvatarPartSnapshot body;
    AvatarPartSnapshot arms;
    int has_arms;
} AvatarCombo;

/* ========================================================================= */
/* Division: `division <id> <nameKey> { combo... }`                          */
/* (a token after the name key, e.g. the shipped "skipdemo", is read for      */
/* nothing [orig: CAvatarDefs_ParseConfigLine @ 0x57a73b..0x57a7ab, tokens 1  */
/* and 2 alone] and not kept)                                                 */
/* ========================================================================= */

typedef struct AvatarDivision {
    int id;                   /* the id token's lenient atol, 0..15 (written as its decimal) [orig @ 0x57a74e..0x57a757] */
    char name_key[64];        /* "Avatars" RTXT string key                        [orig @ 0x57a7ab] */
    AvatarCombo *combos;
    size_t combos_count;
} AvatarDivision;

/* ========================================================================= */
/* Nationality: `nationality <id> <nameKey> { alignment; division... }`       */
/* (a token after the name key is read for nothing [orig: @ 0x57a615..0x57a681, */
/* tokens 1 and 2 alone] and not kept)                                        */
/* ========================================================================= */

typedef struct AvatarNationality {
    int id;                   /* the id token's lenient atol, 0..31 (written as its decimal) [orig @ 0x57a628..0x57a631] */
    char name_key[64];        /* "Avatars" RTXT string key                        [orig @ 0x57a681] */
    int alignment;            /* AvatarAlignment (good/evil)                      [orig @ 0x57a6e9] */
    int has_alignment;        /* whether an `alignment` line was present */
    AvatarDivision *divisions;
    size_t divisions_count;
    /* Where the nationality stands among the parts: how many of the file's parts (its last ones) are defined
     * below its line. A combo binds the last part of its name and kind among the parts defined above it, the
     * walk's part count at the combo line [orig: CAvatarDefs_ParseConfigLine @ 0x57a830..0x57a8d3, each scan
     * up to g_AvatarPartCount], and no part is defined inside a nationality's block, so this is what its combos
     * bind (D-PLAYERINFO-35). 0: every part above it (a model built in code). The writer puts each nationality
     * after the parts above it; the nationalities' counts never grow down the file. */
    size_t parts_below;
} AvatarNationality;

/* ========================================================================= */
/* File                                                                      */
/* ========================================================================= */

typedef struct AvatarsFile {
    AvatarPart *parts;
    size_t parts_count;
    AvatarNationality *nationalities;
    size_t nationalities_count;
    AvatarDiagnostic *diagnostics;
    size_t diagnostics_count;
} AvatarsFile;

/* ========================================================================= */
/* API                                                                       */
/* ========================================================================= */

/* Parse Avatars.def text. Returns 0 on success, nonzero on error. The text is
 * expected to be plaintext (the original decrypts with key 0x2A56F6AD upstream
 * of the parser — docs/playerinfo/avatars-re.md). `out` is zero-filled then
 * populated; free with avatars_free(). */
int avatars_parse(const char *path, AvatarsFile *out);
int avatars_parse_memory(const void *data, size_t size, AvatarsFile *out);
void avatars_free(AvatarsFile *file);

/* Serialize `file` to a canonical Avatars.def from scratch (never raw
 * passthrough — docs/adr/0003, policy docs/adr/0021; no original writer of the
 * file is witnessed, so the form is OpenNova's). On success returns 0 and
 * sets *out_data (malloc'd, NUL-terminated) and *out_size (length excluding the
 * NUL). Free with avatars_free_buffer(). The write is deterministic, from
 * the model alone: every id its decimal, every name as the model holds it, the
 * parts in order with each nationality after the parts above it
 * (AvatarNationality::parts_below), so every combo binds the part it bound; a
 * parse->write->parse->write round-trip is byte-identical on the second write.
 * Returns 2, writing nothing, when a combo line would hold an empty name ahead
 * of a filled one (its head ahead of its body, its body ahead of its arms):
 * the game's tokenizer reads the `""` it would be written as as no token, so
 * the later names would read back shifted. Returns 3, writing nothing, when the
 * nationalities' places are no file's order: a parts_below past the part count,
 * or past the nationality before it's. */
int avatars_write(const AvatarsFile *file, char **out_data, size_t *out_size);
void avatars_free_buffer(char *data);

/* Whether two models read the same (their diagnostics and the parts' resolved snapshots aside). */
bool avatars_equal(const AvatarsFile &a, const AvatarsFile &b);

} // namespace opennova::avatars
