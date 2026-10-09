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
 * consumers see the same duplicate/forward-reference behavior as the original.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <formats/textlayout/text_layout.h>

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
    char (*raw_lines)[512];   /* unrecognized lines inside the block (superset) */
    size_t raw_lines_count;
    uint64_t note;            /* its lines in the file's modeled layout (textlayout); 0 none */
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
    char raw_id[32];          /* id token verbatim, e.g. "001" (round-trip)      [orig @ 0x57a815] */
    int id;                   /* parsed numeric id (atol, leading non-digit skipped) */
    char head_name[64];       /* reference part names (D-PLAYERINFO-4)           [orig @ 0x57a804..0x57a809] */
    char body_name[64];
    char arms_name[64];       /* empty if the combo has no arms */
    AvatarPartSnapshot head;  /* parse-time resolved part snapshots */
    AvatarPartSnapshot body;
    AvatarPartSnapshot arms;
    int has_arms;
    uint64_t note;            /* its line in the file's modeled layout; 0 none */
} AvatarCombo;

/* ========================================================================= */
/* Division: `division <id> <nameKey> [flags] { combo... }`                  */
/* ========================================================================= */

typedef struct AvatarDivision {
    char raw_id[32];          /* id token verbatim, e.g. "D00" */
    int id;                   /* parsed numeric id (0..15)                        [orig @ 0x57a757] */
    char name_key[64];        /* "Avatars" RTXT string key                        [orig @ 0x57a7ab] */
    char flags[128];          /* trailing tokens after name_key (e.g. "skipdemo"); "" if none */
    AvatarCombo *combos;
    size_t combos_count;
    char (*raw_lines)[512];   /* unrecognized lines inside the division block */
    size_t raw_lines_count;
    uint64_t note;            /* its lines in the file's modeled layout; 0 none */
} AvatarDivision;

/* ========================================================================= */
/* Nationality: `nationality <id> <nameKey> [flags] { alignment; division... }`*/
/* ========================================================================= */

typedef struct AvatarNationality {
    char raw_id[32];          /* id token verbatim, e.g. "N00" */
    int id;                   /* parsed numeric id (0..31)                        [orig @ 0x57a631] */
    char name_key[64];        /* "Avatars" RTXT string key                        [orig @ 0x57a681] */
    char flags[128];          /* trailing tokens after name_key (e.g. "skipdemo"); "" if none */
    int alignment;            /* AvatarAlignment (good/evil)                      [orig @ 0x57a6e9] */
    int has_alignment;        /* whether an `alignment` line was present */
    AvatarDivision *divisions;
    size_t divisions_count;
    char (*raw_lines)[512];   /* unrecognized lines directly in the nationality block (outside divisions) */
    size_t raw_lines_count;
    uint64_t note;            /* its lines in the file's modeled layout; 0 none */
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
    uint64_t note;            /* the file's own record in its modeled layout; 0 none */
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
 * passthrough — docs/adr/0003, policy docs/adr/0021). On success returns 0 and
 * sets *out_data (malloc'd, NUL-terminated) and *out_size (length excluding the
 * NUL). Free with avatars_free_buffer(). The write is deterministic: a
 * parse->write->parse->write round-trip is byte-identical on the second write.
 * Returns 2, writing nothing, when a line would hold an empty field ahead of a
 * filled one (a nationality's or division's id or name key ahead of its flags,
 * a combo's id, head or body ahead of the next): the game's tokenizer reads
 * the `""` it would be written as as no token, so the later fields would read
 * back shifted. */
int avatars_write(const AvatarsFile *file, char **out_data, size_t *out_size);
void avatars_free_buffer(char *data);

/* The parse with the file's layout modeled (textlayout, ADR 0003: "model it, generate it"): each record's
 * lines (a part's, a nationality's, a division's, a combo's) its own, every line the walk reads nothing of
 * (a comment, a blank, an unrecognized key, a refused block, a combo whose parts are not defined yet) read for
 * nothing where it stands; each record's note set. */
int avatars_parse_memory(const void *data, size_t size, AvatarsFile *out, textlayout::Notes &notes);

/* avatars_write over the file's modeled layout: the file as it was but for the lines of a changed value (its
 * comments, its spacing, a word's own spelling, the lines the walk reads nothing of); a line or a record put
 * down anew after the one before it in the writer's order, in the writer's form; the records of a kind (the
 * parts, the nationalities, a nationality's divisions, a division's combos) in the model's order. The text is
 * read again: one that would not read back as the model is written in the writer's form, `rewritten` set.
 * Returns as avatars_write. */
int avatars_write(const AvatarsFile *file, const textlayout::Notes *notes, char **out_data, size_t *out_size,
                  bool *rewritten);

/* Whether two models read the same (their notes, diagnostics and the parts' resolved snapshots aside). */
bool avatars_equal(const AvatarsFile &a, const AvatarsFile &b);

} // namespace opennova::avatars
