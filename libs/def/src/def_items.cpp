#include "def/def.h"

// Split out of def.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// ITEMS.DEF: one record per world item, the largest of the families.

#include "def_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace defscan; // the shared .def scanner, unqualified as before

/* ========================================================================= */
/* Items Parsing                                                             */
/* ========================================================================= */

/* The engine's `type` token -> ItemDef+0x5C values, case-insensitive like the
   original's _stricmp chain. Non-injective by engine design: decoration and
   foliage share 2, powerup and object share 6; an unknown token leaves 0
   (unset), and 7 is unused. [orig: ItemDef_ParseProperty @ 0x49eb00;
   docs/world/itemdef-re.md D-ITEMDEF-1] */
static int item_type_from_string(const char *s, size_t len) {
    char low[16];
    size_t ll = len < 15 ? len : 15;
    to_lower_buf(low, s, ll);
    if (ll == 7 && memcmp(low, "vehicle", 7) == 0) return DEF_ITEM_TYPE_VEHICLE;
    if (ll == 10 && memcmp(low, "decoration", 10) == 0) return DEF_ITEM_TYPE_DECORATION;
    if (ll == 7 && memcmp(low, "foliage", 7) == 0) return DEF_ITEM_TYPE_FOLIAGE;
    if (ll == 6 && memcmp(low, "person", 6) == 0) return DEF_ITEM_TYPE_PERSON;
    if (ll == 6 && memcmp(low, "marker", 6) == 0) return DEF_ITEM_TYPE_MARKER;
    if (ll == 8 && memcmp(low, "building", 8) == 0) return DEF_ITEM_TYPE_BUILDING;
    if (ll == 7 && memcmp(low, "powerup", 7) == 0) return DEF_ITEM_TYPE_POWERUP;
    if (ll == 6 && memcmp(low, "object", 6) == 0) return DEF_ITEM_TYPE_OBJECT;
    if (ll == 6 && memcmp(low, "effect", 6) == 0) return DEF_ITEM_TYPE_EFFECT;
    return DEF_ITEM_TYPE_UNSET;
}

/* Anchored particle-effect slot args: <effect> <userpoint> [<secondary_effect>].
   The original copies argv[1]/argv[2] unguarded into 32-char slots and reads the
   third token only when the line carries more than 3 tokens (argc > 3, key
   included); extra tokens beyond those are ignored. `with_secondary` is 0 for
   particlefx/particlefxw3/particlefxw4, which never read a third token.
   [orig: ItemDef_ParseProperty @ 0x49eb00, particlefx chain @ 0x4a13ad..0x4a15eb] */
static void parse_item_particle_slot(const char *v, size_t vl, DefItemParticleFx *slot,
                                     int with_secondary) {
    Token tok[MAX_TOKENS];
    int n = tokenize(v, vl, tok, MAX_TOKENS);
    if (n >= 1) safe_copy(slot->effect, sizeof(slot->effect), tok[0].s, tok[0].len);
    if (n >= 2) safe_copy(slot->userpoint, sizeof(slot->userpoint), tok[1].s, tok[1].len);
    if (with_secondary && n >= 3)
        safe_copy(slot->secondary_effect, sizeof(slot->secondary_effect), tok[2].s, tok[2].len);
}

/* Shared items.def parser over an in-memory buffer. The caller owns `buf` and must have
   zeroed `out` first. Lets both the path loader and the VFS/PFF byte loader share one parser. */
static int parse_items_buf(const char *buf, size_t file_len, DefItemsFile *out) {
    size_t entries_cap = 0;
    DefItemDef current;
    memset(&current, 0, sizeof(current));
    int in_block = 0;
    size_t raw_cap = 0;
    size_t emplacement_attachments_cap = 0;

    LineIter it = {buf, file_len, 0};
    const char *line; size_t line_len;
    char lower[1024];

    while (next_line(&it, &line, &line_len)) {
        size_t tlen;
        const char *trimmed = trim_span(line, line_len, &tlen);
        if (tlen == 0) continue;

        size_t ll = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
        to_lower_buf(lower, trimmed, ll);

        if (!in_block) {
            if (lower_starts_with(lower, ll, "begin", 5)) {
                memset(&current, 0, sizeof(current));
                raw_cap = 0;
                emplacement_attachments_cap = 0;
                extract_quoted(trimmed, tlen, current.display_name, sizeof(current.display_name));
                in_block = 1;
            }
            continue;
        }

        if (ll == 3 && memcmp(lower, "end", 3) == 0) {
            DA_PUSH(out->entries, out->count, entries_cap, current);
            memset(&current, 0, sizeof(current));
            raw_cap = 0;
            emplacement_attachments_cap = 0;
            in_block = 0;
            continue;
        }

        int parsed = 0;

        if (lower_starts_with(lower, ll, "id ", 3)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 3, &vl);
            current.id = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sid", 3)) {
            consume_value_str(trimmed, tlen, 3, current.sid, sizeof(current.sid));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "type", 4)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 4, &vl);
            current.type = item_type_from_string(v, vl);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "graphic", 7)) {
            consume_value_str(trimmed, tlen, 7, current.graphic, sizeof(current.graphic));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "anim_def", 8)) {
            consume_value_str(trimmed, tlen, 8, current.anim_def, sizeof(current.anim_def));
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "husk ", 5)) {
            consume_value_str(trimmed, tlen, 5, current.husk, sizeof(current.husk));
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hp ", 3)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 3, &vl);
            current.hp = signed_i16_value(parse_int_n(v, vl)); /* healthMax i16 @+0x17C */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sound_profilefemale", 19)) {
            /* The female-variant profile [orig: ItemDef_ParseProperty
               "sound_profileFemale" @ 0x49fb76 -> def+0x26C]. */
            consume_value_str(trimmed, tlen, 19, current.sound_profile_female,
                              sizeof(current.sound_profile_female));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sound_profile", 13)) {
            /* "sound_profile" retargets the female slot too while it still
               tracks the primary (both seed to the "default" profile at alloc;
               an explicit sound_profileFemale detaches it). The original
               compares the two resolved profile POINTERS and rewrites +0x26C
               only when equal [orig: @ 0x49fb0f-0x49fb64 (the +0x26C==+0x268
               gate); alloc seed @ 0x49e3f5-0x49e408]; the name compare is the
               same rule over our unresolved names. */
            if (strcmp(current.sound_profile_female, current.sound_profile) == 0) {
                consume_value_str(trimmed, tlen, 13, current.sound_profile_female,
                                  sizeof(current.sound_profile_female));
            }
            consume_value_str(trimmed, tlen, 13, current.sound_profile, sizeof(current.sound_profile));
            parsed = 1;
        /* [orig: ItemDef_ParseProperty @ 0x49eb00 -- "soundloop_" prefix @ 0x49fec4,
           nightshot/duskshot/dawnshot @ 0x49fdee; the 7-slot range matches the
           engine's Soundloop_1..7 sound-type table @ 0x7d0788] */
        } else if (lower_starts_with(lower, ll, "soundloop_", 10) && ll > 10) {
            char idx_char = lower[10];
            if (idx_char >= '1' && idx_char <= '7') {
                int idx = idx_char - '1';
                consume_value_str(trimmed, tlen, 11, current.soundloops[idx], sizeof(current.soundloops[idx]));
                parsed = 1;
            }
        } else if (lower_match_key(lower, ll, "nightshot", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            /* Extract first token only */
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.nightshot, sizeof(current.nightshot), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "dawnshot", 8)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.dawnshot, sizeof(current.dawnshot), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "duskshot", 8)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.duskshot, sizeof(current.duskshot), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "dayshot", 7)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 7, &vl);
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.dayshot, sizeof(current.dayshot), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "ai_function", 11)) {
            consume_value_str(trimmed, tlen, 11, current.ai_function, sizeof(current.ai_function));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "move_function", 13)) {
            consume_value_str(trimmed, tlen, 13, current.move_function, sizeof(current.move_function));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "render_function", 15)) {
            consume_value_str(trimmed, tlen, 15, current.render_function, sizeof(current.render_function));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "disk_function", 13)) {
            consume_value_str(trimmed, tlen, 13, current.disk_function, sizeof(current.disk_function));
            parsed = 1;
        /* The person-item anim-fire weapon family (world-wac-ai-re §17.4, D-AI-5): only the
           closeattack name is kept — JO riflemen author all four ammo_* slots to the same
           rifle round. [orig: ItemDef_ParseProperty 'ammo_closeattack' @ 0x4a1823 -> def+0x56B] */
        } else if (lower_match_key(lower, ll, "ammo_closeattack", 16)) {
            consume_value_str(trimmed, tlen, 16, current.ammo_closeattack, sizeof(current.ammo_closeattack));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "primary_weapon", 14)) {
            /* The ewep emplacement's mounted weapon.def entry (the gun entity's slot-0
               weapon; the attach label's text source) [orig: -> def+0x54B primaryWeapon,
               docs/world/itemdef-re.md +0x54b] */
            consume_value_str(trimmed, tlen, 14, current.primary_weapon, sizeof(current.primary_weapon));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "addeweapg", 9) ||
                   lower_match_key(lower, ll, "addeweapc", 9) ||
                   lower_match_key(lower, ll, "addeweap", 8)) {
            /* Authored child-emplacement attachment:
                 <userpoint> <item-def id> [down up right left]
               Packed JOX has three deliberately distinct key spellings. Retain
               the variant instead of folding G/C into the ordinary record. */
            const int kind =
                    lower_match_key(lower, ll, "addeweapg", 9)
                            ? DEF_ITEM_EMPLACEMENT_ADDEWEAP_G
                    : lower_match_key(lower, ll, "addeweapc", 9)
                            ? DEF_ITEM_EMPLACEMENT_ADDEWEAP_C
                            : DEF_ITEM_EMPLACEMENT_ADDEWEAP;
            const size_t key_len =
                    kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP ? 8u : 9u;
            size_t vl;
            const char *v = consume_value_span(trimmed, tlen, key_len, &vl);
            Token tok[6];
            const int n = tokenize(v, vl, tok, 6);
            /* Retail has four fixed slots. A fifth valid record is recognized but
               silently ignored. The optional arc is all-or-none: partial tails
               remain raw diagnostics instead of inventing missing limits. */
            if (n == 2 || n >= 6) {
                parsed = 1;
                if (current.emplacement_attachments_count >= 4) {
                    continue;
                }
                DefItemEmplacementAttachment attachment;
                memset(&attachment, 0, sizeof(attachment));
                safe_copy(attachment.userpoint, sizeof(attachment.userpoint),
                          tok[0].s, tok[0].len);
                attachment.item_id = parse_int_n(tok[1].s, tok[1].len);
                attachment.kind = kind;
                attachment.angle_count = n >= 6 ? 4 : 0;
                if (n >= 6) {
                    constexpr int kBamPerDegree = 11930464;
                    attachment.down_angle =
                            parse_int_n(tok[2].s, tok[2].len) * kBamPerDegree;
                    attachment.up_angle =
                            -parse_int_n(tok[3].s, tok[3].len) * kBamPerDegree;
                    attachment.right_angle =
                            parse_int_n(tok[4].s, tok[4].len) * kBamPerDegree;
                    attachment.left_angle =
                            -parse_int_n(tok[5].s, tok[5].len) * kBamPerDegree;
                }
                DA_PUSH(current.emplacement_attachments,
                        current.emplacement_attachments_count,
                        emplacement_attachments_cap, attachment);
                const int stored_slot =
                        static_cast<int>(current.emplacement_attachments_count);
                if (kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G)
                    current.emplacement_g_slot = stored_slot;
                else if (kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C)
                    current.emplacement_c_slot = stored_slot;
            }
        } else if (lower_match_key(lower, ll, "phrase_set", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            /* Plain signed atol -> target itemDef+0x86C. Presence cannot be
               represented by the zero-initialized value because config 0 is real.
               [orig: @ 0x49F9DB..0x49FA0A] */
            current.phrase_set = parse_int_n(v, vl);
            current.phrase_set_valid = 1;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "light_transfer", 14)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 14, &vl);
            /* atoi, clamp 0..100, then percent -> float at ItemDef+0x218.
               [orig: @0x4A1A12..0x4A1A50; scale 0.01f @0x7C56A8] */
            int transfer = parse_int_n(v, vl);
            if (transfer < 0) transfer = 0;
            if (transfer > 100) transfer = 100;
            current.light_transfer = static_cast<float>(transfer) * 0.01f;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "clipsize", 8)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
            /* plain atol -> def+0x894, the entity+0x35C magazine reseed source
               [orig: @ 0x49fa2e-0x49fa48; Entity_ResetToSpawnState @ 0x4b97a9] */
            current.clipsize = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "deathtime", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            /* seconds -> ticks at parse: 62*v, an explicit 0 -> 496, +62 grace; the
               corpse timer's seed (entity+0x148 at the death edge @ 0x4b9c97)
               [orig: ItemDef_ParseProperty @ 0x49fa6c-0x49faa0 -> def+0x890] */
            int dt = parse_int_n(v, vl) * 62;
            if (dt == 0) dt = 496;
            current.deathtime_ticks = dt + 62;
            parsed = 1;
        /* Vehicle physics-property block, scaled at parse exactly like the original loader
           [orig: ItemDef_ParsePhysicsProperty @0x49d870]. turn_rate2 is matched before
           turn_rate only for clarity — lower_match_key requires a separator after the key. */
        } else if (lower_match_key(lower, ll, "turn_rate2", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.turn_rate2 = parse_int_n(v, vl) * 192426; /* deg/s -> BAM/tick [orig: @0x49d8dc] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "turn_rate", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            current.turn_rate = parse_int_n(v, vl) * 192426; /* deg/s -> BAM/tick [orig: @0x49d89a] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "torque", 6)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 6, &vl);
            current.torque = parse_int_n(v, vl); /* raw shift count [orig: @0x49dcca] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "max_slope", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            current.max_slope = parse_int_n(v, vl) * 11930464; /* deg -> BAM [orig: @0x49d91e] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "slip_slope", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.slip_slope = parse_int_n(v, vl) * 11930464; /* deg -> BAM [orig: @0x49d960] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "player_speed", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.player_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: @0x49d9a2] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "water_speed", 11)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
            current.water_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: @0x49d9e4] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "acceleration", 12)) {
            /* token*4; a still-unset deceleration defaults to 2*acceleration (8*token) at
               THIS parse site, mirroring the original's ordering semantics [orig: @0x49da32,
               decel default @0x49da4b]. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            int a4 = parse_int_n(v, vl) * 4;
            current.acceleration = a4;
            if (current.deceleration == 0) current.deceleration = a4 * 2;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "deceleration", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.deceleration = parse_int_n(v, vl) * 4; /* [orig: @0x49da84] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "slip_speed", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.slip_speed = parse_int_n(v, vl) * 4; /* [orig: @0x49dafd] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "physics", 7)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 7, &vl);
            current.physics = parse_int_n(v, vl); /* raw selector [orig: @0x49dac8] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "criticalhp", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.critical_hp = parse_int_n(v, vl); /* i16 raw at +0x180 */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "criticaldrain", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            current.critical_drain = parse_int_n(v, vl); /* i16 raw at +0x182 */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "damage_reduc_pp", 15)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 15, &vl);
            Token tok[2];
            const int count = split_values(v, vl, tok, 2);
            if (count >= 1) {
                current.damage_reduc_pp = parse_float_n(tok[0].s, tok[0].len);
                current.damage_reduc_max = 1.0f - current.damage_reduc_pp;
            }
            if (count >= 2)
                current.damage_reduc_max = parse_float_n(tok[1].s, tok[1].len);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "armor", 5)) {
            /* 'armor A [B]': +0x190 impact = A then overwritten by B;
               +0x192 blast = A. The historical armor_kz API name is an alias
               for blast armor, not a separate parsed field. Both retail words
               are signed i16 (-1 is the 0xFFFF invulnerable value).
               [orig: @ 0x4a00e7-0x4a0147] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
            Token tok[2];
            const int count = split_values(v, vl, tok, 2);
            if (count >= 1) {
                const int armor = signed_i16_value(parse_int_n(tok[0].s, tok[0].len));
                current.armor_impact = armor;
                current.armor_blast = armor;
                current.armor_kz = armor;
            }
            if (count >= 2)
                current.armor_impact =
                    signed_i16_value(parse_int_n(tok[1].s, tok[1].len));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "unit_type", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            current.unit_type = parse_int_n(v, vl); /* minimap icon class [orig: @0x50FA70]
                                                       + the death-dispatch row key
                                                       [orig: Entity_DispatchDeathCallback
                                                       @0x493f23 vs table @0x815410] */
            parsed = 1;
        /* --- the destruction/husk block [orig: ItemDef_ParseProperty @ 0x49eb00] --- */
        } else if (lower_match_key(lower, ll, "huskfinal", 9)) {
            consume_value_str(trimmed, tlen, 9, current.huskfinal, sizeof(current.huskfinal));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sounddeath", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.sounddeath, sizeof(current.sounddeath), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "kz", 2)) {
            /* Death-blast radius in units, plain float -> def+0x198. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 2, &vl);
            current.kz = (float)parse_float_n(v, vl);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "husk_swap_at_sec", 16)) {
            /* seconds*62 ticks; an authored 0 stores 1.0. [orig: @ 0x49f1ce-0x49f228] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 16, &vl);
            float sec = (float)(parse_float_n(v, vl) * 62.0);
            current.husk_swap_at_sec = (sec == 0.0f) ? 1.0f : sec;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "husk_swap_at", 12)) {
            /* Dual-unit: while +0x1A0 is still 0 the value is a PERCENT (atol*0.01),
               else seconds*62 — the witnessed parse-order dependence.
               [orig: @ 0x49f242-0x49f2c2] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            if (current.husk_swap_at_sec == 0.0f)
                current.husk_swap_at = (float)(parse_int_n(v, vl) * 0.01);
            else
                current.husk_swap_at = (float)(parse_float_n(v, vl) * 62.0);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "debris_scale", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.debris_scale = (float)parse_float_n(v, vl); /* -> def+0x1BC */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "husk_sub_parts", 14)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 14, &vl);
            current.husk_sub_parts = parse_int_n(v, vl); /* -> +0x100 count byte */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "husk_sub_part_types", 19)) {
            /* Each value 'NN_NAME': split at the FIRST '_', slot = NN-1 (0..15), the
               remainder (internal underscores kept) resolved case-insensitively against
               the engine debris-type table names; at most 16 values processed.
               [orig: @ 0x49f314-0x49f396; DeathPieceType_FindByName @ 0x57b310] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 19, &vl);
            Token tok[MAX_TOKENS];
            int ntok = split_values(v, vl, tok, MAX_TOKENS);
            int processed = 0;
            for (int ti = 0; ti < ntok && processed < 16; ++ti) {
                const char *us = NULL;
                for (size_t k = 0; k < tok[ti].len; ++k) {
                    if (tok[ti].s[k] == '_') { us = tok[ti].s + k; break; }
                }
                if (us == NULL) continue;
                int slot = parse_int_n(tok[ti].s, (size_t)(us - tok[ti].s)) - 1;
                if (slot < 0 || slot > 15) continue;
                ++processed;
                const char *nm = us + 1;
                size_t nl = tok[ti].len - (size_t)(nm - tok[ti].s);
                current.husk_sub_part_types[slot] =
                        (unsigned char)death_piece_type_index(nm, nl);
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "attrib:", 7)) {
            /* Space-separated capability tokens -> ItemDefAttrib/Attrib2 bits. Unknown tokens
               (exp1, pilotonly, forceasset, neutral, ...) are not in the witnessed map and stay
               unmapped. [orig: ItemDef_ParseProperty @0x49eb00; docs/world/itemdef-re.md] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 7, &vl);
            Token tok[MAX_TOKENS];
            int ntok = split_values(v, vl, tok, MAX_TOKENS);
            for (int ti = 0; ti < ntok; ++ti) {
                char lo[32];
                size_t k = tok[ti].len < sizeof(lo) - 1 ? tok[ti].len : sizeof(lo) - 1;
                to_lower_buf(lo, tok[ti].s, k);
                int b = lookup_item_attrib(lo, k);
                if (b) { current.attrib |= (unsigned)b; }
                else { int b2 = lookup_item_attrib2(lo, k); if (b2) current.attrib2 |= (unsigned)b2; }
            }
            parsed = 1;
        /* Per-item particle-effect keys, matched in the original's chain order
           (lower_match_key requires a separator after the key, so the shared
           'particlefx' prefix cannot shadow the longer keys). All names are
           copied verbatim as strings [orig: ItemDef_ParseProperty @ 0x49eb00]. */
        } else if (lower_match_key(lower, ll, "particlefx", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            parse_item_particle_slot(v, vl, &current.particlefx, 0); /* +0x278/+0x298 [orig: @ 0x4a13ad] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxs", 11)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxs, 1); /* +0x2AE/+0x2EE, 2nd +0x2CE [orig: @ 0x4a140b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxw1", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxw1, 1); /* +0x304/+0x344, 2nd +0x324 [orig: @ 0x4a148b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxw2", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxw2, 1); /* +0x35A/+0x39A, 2nd +0x37A [orig: @ 0x4a150b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxw3", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxw3, 0); /* +0x3AE/+0x3CE, NO secondary [orig: @ 0x4a158b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxw4", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxw4, 0); /* +0x3E2/+0x402, NO secondary [orig: @ 0x4a15eb] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particledeath", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particledeath, sizeof(current.particledeath), tok[0].s,
                          tok[0].len); /* +0x416 [orig: @ 0x4a164b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particleh2odeath", 16)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 16, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particleh2odeath, sizeof(current.particleh2odeath), tok[0].s,
                          tok[0].len); /* +0x44A [orig: @ 0x4a168e] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefire", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlefire, sizeof(current.particlefire), tok[0].s,
                          tok[0].len); /* +0x47E [orig: @ 0x4a16d0] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particleother", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particleother, sizeof(current.particleother), tok[0].s,
                          tok[0].len); /* +0x4B2 [orig: @ 0x4a1713] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefinale", 14)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 14, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlefinale, sizeof(current.particlefinale), tok[0].s,
                          tok[0].len); /* +0x4E4 [orig: @ 0x4a175b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlespawn", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlespawn, sizeof(current.particlespawn), tok[0].s,
                          tok[0].len); /* +0x506 [orig: @ 0x4a179d] */
            parsed = 1;
        }

        if (!parsed) {
            DA_PUSH_RAW(current.raw_lines, current.raw_lines_count, raw_cap, line, line_len);
        }
    }

    return 0;
}

DEF_EXPORT int def_parse_items(const char *path, DefItemsFile *out) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_items_buf(buf, file_len, out);
    free(buf);
    return rc;
}

DEF_EXPORT int def_parse_items_memory(const uint8_t *data, size_t size, DefItemsFile *out) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    return parse_items_buf((const char *)data, size, out);
}

DEF_EXPORT void def_free_items(DefItemsFile *f) {
    if (!f) return;
    for (size_t i = 0; i < f->count; ++i) {
        free(f->entries[i].emplacement_attachments);
        free(f->entries[i].raw_lines);
    }
    free(f->entries);
    memset(f, 0, sizeof(*f));
}
