#include <formats/def/def.h>
#include <base/io/crt_ftol.h>

// Split out of def.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// ITEMS.DEF: one record per world item, the largest of the families.

#include "def_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

using namespace opennova::defscan; // the shared .def scanner, unqualified as before

namespace opennova::def {

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

/* One byte (0 = low) of a little-endian ItemDef dword the byte-writing keys
   share (the +0x890 door/death dword, the +0x894 clipsize dword). */
static void set_def_byte(int32_t &dword, int byte_index, uint8_t value) {
    const unsigned shift = static_cast<unsigned>(byte_index) * 8u;
    dword = static_cast<int32_t>((static_cast<uint32_t>(dword) & ~(0xFFu << shift)) |
                                 (static_cast<uint32_t>(value) << shift));
}

/* Retail's per-block ItemDef defaults. Every items.def `begin` allocates a slot
   through ItemDef_AllocateWithDefaults, which zeroes the record and then stamps
   this physics block BEFORE any key is parsed, so an item that declares none of
   these keys still runs on these values — not on zero.
   [orig: ItemDef_AllocateWithDefaults @0x49E3B0; the
    `begin` arm calls it at ItemDef_ParseProperty @0x49EB00]

   Why this matters (AI-PARITY-CONCEPT §6.15h): DTruck1 (id 101294) declares NO
   spring_comp, so retail runs it at springComp 20 -> suspension travel 13108,
   while our zeroed record gave travel 0, which pinned every wheel oscillator's
   amplitude to 0 and froze the compressions for the whole run.

   EXCEPTION, stated rather than guessed: retail also defaults `unk591` to 5,
   but the key that writes unk591 is an unresolved indirect string in the
   decompilation (`off_7C7D78`), so its identity with our `bob`
   field is NOT witnessed for defaulting purposes and `bob` is deliberately
   left at 0. */
void def_init_item(DefItemDef &value) {
    memset(&value, 0, sizeof(value));
    DefItemDef *d = &value;
    d->climb_speed = 1;    /* [orig: @0x0049E3B0 climbSpeed] */
    d->torque = 3;         /* [orig: torque] */
    d->mass = 5;           /* [orig: mass] */
    d->shock = 4;          /* [orig: shock] */
    d->spring = 0;         /* [orig: spring — explicit in retail, kept explicit here] */
    d->spring_comp = 20;   /* [orig: springComp — the freeze root, §6.15h] */
    d->top_heavy = 0;      /* [orig: topHeavy — explicit in retail] */
    d->lean = 5;           /* [orig: lean] */
    d->lean_velocity = 5;  /* [orig: leanVelocity] */
    d->pitch = 1;          /* [orig: pitch] */
    d->pitch_velocity = 5; /* [orig: pitchVelocity] */
    d->flip = 45;          /* [orig: flip] */
    d->hand_brake = 1;     /* [orig: handBrake] */
    d->tire_slip = 5;      /* [orig: tireSlip] */
}

/* Shared items.def parser over an in-memory buffer. The caller owns `buf` and must have
   zeroed `out` first. Lets both the path loader and the VFS/PFF byte loader share one parser. */
static int parse_items_buf(const char *buf, size_t file_len, DefItemsFile *out, DefParseReport *report) {
    size_t entries_cap = 0;
    DefItemDef current;
    memset(&current, 0, sizeof(current));
    def_init_item(current);
    int in_block = 0;
    bool powerup_branch = false, numeric_branch = false;
    size_t emplacement_attachments_cap = 0;

    LineIter it = {buf, file_len, 0};
    const char *line; size_t line_len;
    char lower[1024];

    while (next_line(&it, &line, &line_len)) {
        size_t tlen;
        const char *trimmed = trim_def_line(line, line_len, &tlen);
        if (tlen == 0) continue;

        size_t ll = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
        to_lower_buf(lower, trimmed, ll);

        if (!in_block) {
            if (lower_starts_with(lower, ll, "begin", 5)) {
                memset(&current, 0, sizeof(current));
                def_init_item(current); /* [orig: the begin arm calls
                    ItemDef_AllocateWithDefaults @0x49E3B0 from ItemDef_ParseProperty @0x49EB00] */

                emplacement_attachments_cap = 0;
                powerup_branch = numeric_branch = false;
                extract_quoted(trimmed, tlen, current.display_name, sizeof(current.display_name));
                validate_header(trimmed, tlen, 5, sizeof(current.display_name), DefHeaderName::Quoted, current.unmodeled_count, report, it.line, current.display_name);
                in_block = 1;
            } else authoring_issue(out->unmodeled_count, report, it.line, "", trimmed, tlen);
            continue;
        }

        if (ll == 3 && memcmp(lower, "end", 3) == 0) {
            if (powerup_branch && numeric_branch)
                authoring_issue(current.unmodeled_count, report, it.line, current.display_name,
                    "powerupdef", 10, DefIssueCode::Unrepresentable);
            DA_PUSH(out->entries, out->count, entries_cap, current);
            memset(&current, 0, sizeof(current));
            def_init_item(current);

            emplacement_attachments_cap = 0;
            in_block = 0;
            continue;
        }

        // These are alternate meanings of the original's type-specific union.
        // Mixing both cannot be represented by a symbolic powerup definition.
        powerup_branch |= lower_match_key(lower, ll, "powerupdef", 10) != 0;
        for (const char *key : {"deathtime", "clipsize", "num_doors", "first_door", "first_subobject",
                "door_type", "door_dir", "open_rate", "max_angle", "sqb_rate", "sqb_distance", "sqb_error",
                "rotor_parts", "aux_parts", "door_open_sound_id", "door_close_sound_id"})
            numeric_branch |= lower_match_key(lower, ll, key, strlen(key)) != 0;
        int parsed = 0;

        if (lower_match_key(lower, ll, "powerupdef", 10)) {
            // A symbolic powerup definition, not a death/door numeric value.
            // [orig: ItemDef_ParseProperty @0x49EB00, powerupdef copies to the
            // type-specific union at deathTime and sets attrib POWERUP]
            consume_value_str(trimmed, tlen, 10, current.powerup_def, sizeof(current.powerup_def));
            current.attrib |= DEF_ITEM_ATTRIB_POWERUP;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "graphicenemy", 12)) {
            consume_value_str(trimmed, tlen, 12, current.graphic_enemy, sizeof(current.graphic_enemy));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "textid", 6)) {
            consume_value_str(trimmed, tlen, 6, current.text_id, sizeof(current.text_id));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sqb_rate", 8) ||
                lower_match_key(lower, ll, "sqb_distance", 12) ||
                lower_match_key(lower, ll, "sqb_error", 9)) {
            // These share the door/death fields, including last-write order.
            // Each value goes through _ftol2_sse's SSE2 leg (io/crt_ftol.h).
            // [orig: ItemDef_ParseProperty @0x49EB00, squib arms @0x49F06F (the
            // _ftol2_sse calls @0x49F093 / @0x49F0DC / @0x49F125)]
            const bool rate = lower_match_key(lower, ll, "sqb_rate", 8);
            const bool distance = lower_match_key(lower, ll, "sqb_distance", 12);
            size_t vl;
            const char *v = consume_value_span(trimmed, tlen, rate ? 8 : distance ? 12 : 9, &vl);
            char value[128];
            safe_copy(value, sizeof(value), v, vl);
            const double number = atof(value);
            if (rate) current.deathtime_ticks = io::retail_ftol_sse2(62.0 / number);
            else if (distance) current.clipsize = io::retail_ftol_sse2(number * 65536.0);
            else current.door_type = static_cast<uint32_t>(io::retail_ftol_sse2(number * 65536.0));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "num_doors", 9) ||
                lower_match_key(lower, ll, "first_door", 10) ||
                lower_match_key(lower, ll, "first_subobject", 15)) {
            // One byte of the polymorphic +0x890 dword each: the low byte of
            // atol read signed, first_door/first_subobject one less (`sub
            // al,1`), negative -> 0, above 30 -> 30. num_doors -> +0x890 and
            // first_door -> +0x891 also set the Door attrib; first_subobject
            // -> +0x892 does not. [orig: ItemDef_ParseProperty — num_doors
            // @0x49F766..0x49F78A, first_door @0x49F7BA..0x49F7DE,
            // first_subobject @0x49F9B0..0x49F9CC]
            const bool door = lower_match_key(lower, ll, "first_door", 10);
            const bool subobject = lower_match_key(lower, ll, "first_subobject", 15);
            size_t vl;
            const char *v = consume_value_span(trimmed, tlen, subobject ? 15 : door ? 10 : 9, &vl);
            int8_t value = static_cast<int8_t>(static_cast<uint8_t>(parse_int_n(v, vl)));
            if (door || subobject) value = static_cast<int8_t>(static_cast<uint8_t>(value - 1));
            if (value < 0) value = 0;
            else if (value > 30) value = 30;
            set_def_byte(current.deathtime_ticks, subobject ? 2 : door ? 1 : 0,
                    static_cast<uint8_t>(value));
            if (!subobject) current.attrib |= DEF_ITEM_ATTRIB_DOOR;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "rotor_parts", 11) ||
                lower_match_key(lower, ll, "aux_parts", 9)) {
            // Four raw atol low bytes into the +0x890..+0x897 byte run the door
            // (+0x890 dword) and clipsize (+0x894 dword) fields share:
            // rotor_parts -> +0x890, +0x891, +0x894, +0x895; aux_parts ->
            // +0x896, +0x897, +0x892, +0x893. [orig: ItemDef_ParseProperty —
            // rotor_parts @0x49EF5D..0x49EFB6, aux_parts @0x49EFF3..0x49F04C]
            const bool rotor = lower_match_key(lower, ll, "rotor_parts", 11);
            size_t vl; const char *v = consume_value_span(trimmed, tlen, rotor ? 11 : 9, &vl);
            Token tok[4];
            const int n = tokenize(v, vl, tok, 4);
            uint8_t bytes[4] = {};
            for (int i = 0; i < n; ++i)
                bytes[i] = static_cast<uint8_t>(parse_int_n(tok[i].s, tok[i].len));
            if (rotor) {
                set_def_byte(current.deathtime_ticks, 0, bytes[0]);
                set_def_byte(current.deathtime_ticks, 1, bytes[1]);
                set_def_byte(current.clipsize, 0, bytes[2]);
                set_def_byte(current.clipsize, 1, bytes[3]);
            } else {
                set_def_byte(current.clipsize, 2, bytes[0]);
                set_def_byte(current.clipsize, 3, bytes[1]);
                set_def_byte(current.deathtime_ticks, 2, bytes[2]);
                set_def_byte(current.deathtime_ticks, 3, bytes[3]);
            }
            parsed = 1;
        } else if (lower_match_key(lower, ll, "door_type", 9) ||
                lower_match_key(lower, ll, "door_dir", 8)) {
            const bool type = lower_match_key(lower, ll, "door_type", 9);
            size_t vl; const char *v = consume_value_span(trimmed, tlen, type ? 9 : 8, &vl);
            Token tok[30];
            const int n = tokenize(v, vl, tok, 30);
            uint32_t bits = 0;
            for (int i = 0; i < n; ++i)
                if (parse_int_n(tok[i].s, tok[i].len) != 0) bits |= 1u << (i + 1);
            if (type) current.door_type = bits;
            else current.clipsize = static_cast<int32_t>(bits);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "open_rate", 9) ||
                lower_match_key(lower, ll, "max_angle", 9)) {
            const bool rate = lower_match_key(lower, ll, "open_rate", 9);
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            char value[128];
            safe_copy(value, sizeof(value), v, vl);
            const double number = atof(value);
            // [orig: ItemDef_ParseProperty @0x49EB00 (the _ftol2_sse calls
            // @0x49F91E for open_rate, @0x49F96D for max_angle)]
            if (rate) current.door_open_rate_q16 = io::retail_ftol_sse2(65536.0 / (number * 62.0));
            else current.door_max_angle_bam = io::retail_ftol_sse2(number * (1.0 / 360.0) * 4294967295.0);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "door_open_sound_id", 18) ||
                lower_match_key(lower, ll, "door_close_sound_id", 19)) {
            const bool opening = lower_match_key(lower, ll, "door_open_sound_id", 18);
            size_t vl; const char *v = consume_value_span(trimmed, tlen, opening ? 18 : 19, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) > 0)
                safe_copy(opening ? current.door_open_sound : current.door_close_sound,
                        25, tok[0].s, tok[0].len);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "mana", 4)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 4, &vl);
            current.mana = signed_i16_value(parse_int_n(v, vl));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "music", 5)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
            current.music_location = signed_i16_value(parse_int_n(v, vl));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "score", 5)) {
            /* atol of the first value token, stored as a signed word
               [orig: ItemDef_ParseProperty @0x4A0228..0x4A0242] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
            current.score = signed_i16_value(parse_int_n(v, vl));
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "id ", 3)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 3, &vl);
            current.id = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "pcvehicle_spawnlist", 19)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 19, &vl);
			Token tok[128];
			const int n = tokenize(v, vl, tok, 128);
			current.vehicle_spawn_mask = 0;
			for (int i = 0; i < n; ++i) {
				const int id = parse_int_n(tok[i].s, tok[i].len);
				int slot = 0;
				while (slot < out->vehicle_spawn_id_count && out->vehicle_spawn_ids[slot] != id)
					++slot;
				if (slot == out->vehicle_spawn_id_count && slot < DEF_VEHICLE_SPAWN_SLOTS)
					out->vehicle_spawn_ids[out->vehicle_spawn_id_count++] = id;
				if (slot < DEF_VEHICLE_SPAWN_SLOTS)
					current.vehicle_spawn_mask |= uint32_t(1) << slot;
			}
			parsed = 1;
		} else if (lower_match_key(lower, ll, "sid", 3)) {
			consume_value_str(trimmed, tlen, 3, current.sid, sizeof(current.sid));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "type", 4)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 4, &vl);
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
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 3, &vl);
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
		} else if (lower_match_key(lower, ll, "default_aip", 11)) {
			/* Authoring the profile also RAISES AIData: retail ORs 0x100000 in
			   the same arm, so an item carrying default_aip is an AI item
			   whether or not its attrib line lists AIData.
			   [orig: ItemDef_ParseProperty @0x49eb00 -- the strcpy into
				itemDef+0x8B8 followed by `attrib |= 0x100000`] */
			consume_value_str(trimmed, tlen, 11, current.default_aip,
                              sizeof(current.default_aip));
            current.attrib |= DEF_ITEM_ATTRIB_AIDATA;
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
        } else if (lower_match_key(lower, ll, "destroy_timing", 14)) {
            size_t vl;
            const char *v = consume_value_span(trimmed, tlen, 14, &vl);
            Token values[3] = {};
            const int n = tokenize(v, vl, values, 3);
            for (int column = 0; column < 3; ++column) {
                const double seconds = column < n
                        ? atof(std::string(values[column].s, values[column].len).c_str()) : 0.0;
                // [orig: ItemDef_ParseProperty @0x49EB00 (the _ftol2_sse calls
                // @0x49EE7E / @0x49EEA5 / @0x49EECF)]
                current.destroy_timing_ticks[column] = io::retail_ftol_sse2(seconds * 62.0);
            }
            parsed = 1;
        } else if (lower_match_key(lower, ll, "dawnshot", 8) ||
                   lower_match_key(lower, ll, "dayshot", 7) ||
                   lower_match_key(lower, ll, "duskshot", 8) ||
                   lower_match_key(lower, ll, "nightshot", 9) ||
                   lower_match_key(lower, ll, "particletesttime", 16)) {
            const bool particle_time = lower[0] == 'p';
            const int region = lower[0] == 'n' ? 3 : lower[1] == 'u' ? 2 :
                    lower[2] == 'y' ? 1 : 0;
            const size_t key_len = particle_time ? 16 : region == 3 ? 9 : region == 1 ? 7 : 8;
            size_t vl;
            const char *v = consume_value_span(trimmed, tlen, key_len, &vl);
            Token values[3] = {};
            const int n = tokenize(v, vl, values, 3);
            if (!particle_time && n > 0) {
                char *names[] = {current.dawnshot, current.dayshot, current.duskshot, current.nightshot};
                safe_copy(names[region], 25, values[0].s, values[0].len);
            }
            for (int column = 0; column < 2; ++column) {
                const int token = column + (particle_time ? 0 : 1);
                const double seconds = token < n
                        ? atof(std::string(values[token].s, values[token].len).c_str()) : 0.0;
                // [orig: ItemDef_ParseProperty @0x49EB00 (the _ftol2_sse calls
                // @0x49FAD1 for particletesttime, @0x49FC79..0x49FE5C for the shots)]
                current.shot_delay_ticks[region][column] = io::retail_ftol_sse2(seconds * 62.0);
            }
            parsed = 1;
		} else if (lower_match_key(lower, ll, "ai_function", 11)) {
			consume_value_str(trimmed, tlen, 11, current.ai_function, sizeof(current.ai_function));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "move_function", 13)) {
			consume_value_str(
					trimmed, tlen, 13, current.move_function, sizeof(current.move_function));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "render_function", 15)) {
			consume_value_str(
					trimmed, tlen, 15, current.render_function, sizeof(current.render_function));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "disk_function", 13)) {
			consume_value_str(
					trimmed, tlen, 13, current.disk_function, sizeof(current.disk_function));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "input_function", 14)) {
			consume_value_str(
					trimmed, tlen, 14, current.input_function, sizeof(current.input_function));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "virtualdisplay", 14)) {
			/* [orig: ItemDef_ParseProperty @0x49F4E0 -- token 2 -> def+0xD0,
			   token 3 -> def+0xE0] */
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 14, &vl);
			Token display[2];
			const int n = tokenize(v, vl, display, 2);
			if (n >= 1)
				safe_copy(current.virtual_display, sizeof(current.virtual_display),
						display[0].s, display[0].len);
			if (n >= 2)
				safe_copy(current.virtual_display_userpoint,
						sizeof(current.virtual_display_userpoint), display[1].s, display[1].len);
			parsed = 1;
        /* [orig: ItemDef_ParseProperty @ 0x4A1823, def+0x56B / +0x58B] */
		} else if (lower_match_key(lower, ll, "ammo_closeattack", 16)) {
			consume_value_str(
					trimmed, tlen, 16, current.ammo_closeattack, sizeof(current.ammo_closeattack));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "ammo_marker3", 12)) {
			consume_value_str(
					trimmed, tlen, 12, current.ammo_marker3, sizeof(current.ammo_marker3));
			parsed = 1;
        } else if (lower_match_key(lower, ll, "ammo_easyrocket", 15)) {
            consume_value_str(trimmed, tlen, 15, current.ammo_easyrocket,
                    sizeof(current.ammo_easyrocket));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "ammo_advancedrocket", 19)) {
            consume_value_str(trimmed, tlen, 19, current.ammo_advancedrocket,
                    sizeof(current.ammo_advancedrocket));
            parsed = 1;
        /* The closeattack launch USERPOINT name (the AI muzzle; see def.h)
           [orig: ItemDef_ParseProperty launchups_* -> def+0x5EB/+0x5FB] */
		} else if (lower_match_key(lower, ll, "launchups_closeattack", 21)) {
			consume_value_str(trimmed, tlen, 21, current.launchups_closeattack,
					sizeof(current.launchups_closeattack));
			parsed = 1;
        } else if (lower_match_key(lower, ll, "launchups_rocket", 16)) {
            consume_value_str(trimmed, tlen, 16, current.launchups_rocket,
                    sizeof(current.launchups_rocket));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "launchups_marker3", 17)) {
            consume_value_str(trimmed, tlen, 17, current.launchups_marker3,
                    sizeof(current.launchups_marker3));
            parsed = 1;
        /* The twelve weapon userpoint names (def.h weapon_userpoints; the vehicle/eweap
           fire/flash/casing anchors) [orig: ItemDef_ParseProperty @ 0x4a0ff2..0x4a1301] */
		} else if (lower_match_key(lower, ll, "weaplbup2", 9)) {
			consume_value_str(trimmed, tlen, 9, current.weapon_userpoints[6],
					sizeof(current.weapon_userpoints[6]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaplmup2", 9)) {
			consume_value_str(trimmed, tlen, 9, current.weapon_userpoints[7],
					sizeof(current.weapon_userpoints[7]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaplcup2", 9)) {
			consume_value_str(trimmed, tlen, 9, current.weapon_userpoints[8],
					sizeof(current.weapon_userpoints[8]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaprbup2", 9)) {
			consume_value_str(trimmed, tlen, 9, current.weapon_userpoints[9],
					sizeof(current.weapon_userpoints[9]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaprmup2", 9)) {
			consume_value_str(trimmed, tlen, 9, current.weapon_userpoints[10],
					sizeof(current.weapon_userpoints[10]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaprcup2", 9)) {
			consume_value_str(trimmed, tlen, 9, current.weapon_userpoints[11],
					sizeof(current.weapon_userpoints[11]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaplbup", 8)) {
			consume_value_str(trimmed, tlen, 8, current.weapon_userpoints[0],
					sizeof(current.weapon_userpoints[0]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaplmup", 8)) {
			consume_value_str(trimmed, tlen, 8, current.weapon_userpoints[1],
					sizeof(current.weapon_userpoints[1]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaplcup", 8)) {
			consume_value_str(trimmed, tlen, 8, current.weapon_userpoints[2],
					sizeof(current.weapon_userpoints[2]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaprbup", 8)) {
			consume_value_str(trimmed, tlen, 8, current.weapon_userpoints[3],
					sizeof(current.weapon_userpoints[3]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaprmup", 8)) {
			consume_value_str(trimmed, tlen, 8, current.weapon_userpoints[4],
					sizeof(current.weapon_userpoints[4]));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "weaprcup", 8)) {
			consume_value_str(trimmed, tlen, 8, current.weapon_userpoints[5],
					sizeof(current.weapon_userpoints[5]));
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
               produce authoring diagnostics instead of inventing missing limits. */
            if (n != 2 && n < 6) {
                authoring_issue(current.unmodeled_count, report, it.line, current.display_name, trimmed, tlen, DefIssueCode::InvalidValue);
                continue;
            }
            {
                parsed = 1;
                if (current.emplacement_attachments_count >= 4) {
                    authoring_issue(current.unmodeled_count, report, it.line, current.display_name, trimmed, tlen, DefIssueCode::Unrepresentable);
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
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
			/* Plain signed atol -> target itemDef+0x86C. Presence cannot be
               represented by the zero-initialized value because config 0 is real.
               [orig: @ 0x49F9DB..0x49FA0A] */
            current.phrase_set = parse_int_n(v, vl);
            current.phrase_set_valid = 1;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "reverb", 6)) {
            size_t vl;
            const char *v = consume_value_span(trimmed, tlen, 6, &vl);
            current.reverb = static_cast<int16_t>(parse_int_n(v, vl));
            parsed = 1;
		} else if (lower_match_key(lower, ll, "light_transfer", 14)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 14, &vl);
			/* atoi, clamp 0..100, then percent -> float at ItemDef+0x218.
               [orig: @0x4A1A12..0x4A1A50; scale 0.01f @0x7C56A8] */
            int transfer = parse_int_n(v, vl);
            if (transfer < 0) transfer = 0;
            if (transfer > 100) transfer = 100;
            current.light_transfer = static_cast<float>(transfer) * 0.01f;
            parsed = 1;
		} else if (lower_match_key(lower, ll, "clipsize", 8)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 8, &vl);
			/* plain atol -> def+0x894, the entity+0x35C magazine reseed source
               [orig: @ 0x49fa2e-0x49fa48; Entity_ResetToSpawnState @ 0x4b97a9] */
            current.clipsize = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "deathtime", 9)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 9, &vl);
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
		} else if (lower_match_key(lower, ll, "climb_speed", 11)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 11, &vl);
			current.climb_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: 293*atol @0x49db4a] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "turnroll", 8)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 8, &vl);
			current.turn_roll = parse_int_n(v, vl); /* raw [orig: @0x49dd2a]; air roll-rate cap = token*192426 at use */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "speedpitch", 10)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
			current.speed_pitch = parse_int_n(v, vl); /* raw [orig: @0x49dd66]; air pitch-rate cap = token*192426 at use */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "turn_rate2", 10)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
			current.turn_rate2 = parse_int_n(v, vl) * 192426; /* deg/s -> BAM/tick [orig: @0x49d8dc] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "turn_rate", 9)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 9, &vl);
			current.turn_rate = parse_int_n(v, vl) * 192426; /* deg/s -> BAM/tick [orig: @0x49d89a] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "torque", 6)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 6, &vl);
			current.torque = parse_int_n(v, vl); /* raw shift count [orig: @0x49dcca] */
            parsed = 1;
        /* The platform-solve tuning block — raw atol like the original parser
           [orig: mass @0x49dc76, lean @0x49ddde, lean_velocity @0x49de1a,
            pitch @0x49de56, pitch_velocity @0x49de92, bob @0x49dece,
            flip @0x49df82]. lean_velocity/pitch_velocity must match before
           their prefixes. */
		} else if (lower_match_key(lower, ll, "lean_velocity", 13)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 13, &vl);
			current.lean_velocity = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "pitch_velocity", 14)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 14, &vl);
			current.pitch_velocity = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "mass", 4)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 4, &vl);
			current.mass = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "weathervane", 11)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 11, &vl);
			current.weathervane = parse_int_n(v, vl); /* raw [orig: @0x49d8f2] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "minai", 5)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 5, &vl);
			current.min_ai = parse_int_n(v, vl); /* raw [orig: @0x49d95e] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "lean", 4)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 4, &vl);
			current.lean = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "pitch", 5)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 5, &vl);
			current.pitch = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "bob", 3)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 3, &vl);
			current.bob = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "flip", 4)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 4, &vl);
			current.flip = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "hand_brake", 10)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
			current.hand_brake = parse_int_n(v, vl); /* raw +0x944 [orig: key @0x7c7d60] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "tire_slip", 9)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 9, &vl);
			current.tire_slip = parse_int_n(v, vl); /* raw +0x940 [orig: key @0x7c7d6c] */
            parsed = 1;
        /* The suspension spring block — raw atol [orig: spring_comp @0x49dbd4,
           spring @0x49db5c, shock @0x49dc10, top_heavy @0x49db98 — the last is
           parsed for PARITY only: retail never reads +0x918 outside the parser,
           the allocator and the debug item editor]. spring_comp must match
           before its prefix `spring`. */
		} else if (lower_match_key(lower, ll, "spring_comp", 11)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 11, &vl);
			current.spring_comp = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "spring", 6)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 6, &vl);
			current.spring = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "shock", 5)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 5, &vl);
			current.shock = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "top_heavy", 9)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 9, &vl);
			current.top_heavy = parse_int_n(v, vl);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "max_slope", 9)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 9, &vl);
			current.max_slope = parse_int_n(v, vl) * 11930464; /* deg -> BAM [orig: @0x49d91e] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "slip_slope", 10)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
			current.slip_slope = parse_int_n(v, vl) * 11930464; /* deg -> BAM [orig: @0x49d960] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "player_speed", 12)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 12, &vl);
			current.player_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: @0x49d9a2] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "water_speed", 11)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 11, &vl);
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
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 12, &vl);
			current.deceleration = parse_int_n(v, vl) * 4; /* [orig: @0x49da84] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "slip_speed", 10)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
			current.slip_speed = parse_int_n(v, vl) * 4; /* [orig: @0x49dafd] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "physics", 7)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 7, &vl);
			current.physics = parse_int_n(v, vl); /* raw selector [orig: @0x49dac8] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "criticalhp", 10)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
			current.critical_hp = parse_int_n(v, vl); /* i16 raw at +0x180 */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "criticaldrain", 13)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 13, &vl);
			current.critical_drain = parse_int_n(v, vl); /* i16 raw at +0x182 */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "noncriticalregen", 16)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 16, &vl);
			current.non_critical_regen = parse_int_n(v, vl); /* i16 raw at +0x184 */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "radarsig", 8)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 8, &vl);
			current.radar_sig = parse_int_n(v, vl) & 0xFFFF; /* u16 at +0x178 */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "heatsig", 7)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 7, &vl);
			current.heat_sig = parse_int_n(v, vl) & 0xFFFF; /* u16 at +0x17A */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "damage_reduc_pp", 15)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 15, &vl);
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
		} else if (lower_match_key(lower, ll, "hud_image", 9)) {
			consume_value_str(trimmed, tlen, 9, current.hud_image, sizeof(current.hud_image));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "unit_type", 9)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 9, &vl);
			/* The low byte of its atol, read unsigned [orig: ItemDef_ParseProperty @0x49ee31 atol
			   -> byte +0x196 @0x49ee47]; the atol saturates at 32 bits first (parse_int_n), so
			   2147483651 is 2147483647, byte 255. A number past a byte reported, the record holding
			   what the game holds. The minimap icon class [orig: @0x50FA70] and the death-dispatch row
			   key [orig: Entity_DispatchDeathCallback @0x493f23 vs table @0x815410]. */
			const int read = parse_int_n(v, vl);
			current.unit_type = static_cast<unsigned char>(read);
			if (current.unit_type != read)
				authoring_issue(current.unmodeled_count, report, it.line, current.display_name, trimmed, tlen,
				                DefIssueCode::Reinterpreted, std::to_string(current.unit_type).c_str());
            parsed = 1;
		} else if (lower_match_key(lower, ll, "shadow", 6)) {
			/* 'shadow <name> <w> <l> <ox> <oy>' — the authored ground-shadow
			   blob decal: name -> +0xA0 (a 16-byte slot; retail copies
			   unguarded, we truncate), four atof floats ->
			   +0x11C/+0x120/+0x124/+0x128 (width/length world units, planar
			   offset x/y). Absent trailing tokens read as atof("") = 0 in
			   retail; zero-init matches. [orig: ItemDef_ParseProperty
			   @ 0x49f3a5..0x49f44c; consumer
			   RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0] */
			size_t vl; const char *v = consume_value_span(trimmed, tlen, 6, &vl);
            Token tok[5];
            const int count = split_values(v, vl, tok, 5);
            if (count >= 1)
                safe_copy(current.shadow_texture, sizeof(current.shadow_texture),
                          tok[0].s, tok[0].len);
            if (count >= 2)
                current.shadow_width = (float)parse_float_n(tok[1].s, tok[1].len);
            if (count >= 3)
                current.shadow_length = (float)parse_float_n(tok[2].s, tok[2].len);
            if (count >= 4)
                current.shadow_offset_x = (float)parse_float_n(tok[3].s, tok[3].len);
            if (count >= 5)
                current.shadow_offset_y = (float)parse_float_n(tok[4].s, tok[4].len);
            parsed = 1;
        /* --- the destruction/husk block [orig: ItemDef_ParseProperty @ 0x49eb00] --- */
		} else if (lower_match_key(lower, ll, "huskfinal", 9)) {
			consume_value_str(trimmed, tlen, 9, current.huskfinal, sizeof(current.huskfinal));
			parsed = 1;
		} else if (lower_match_key(lower, ll, "sounddeath", 10)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
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
		} else if (lower_match_key(lower, ll, "scale", 5)) {
			/* Signed Q16.16: atof's double times 65536, truncated toward zero
			   by `fistp qword` under a temporary round-toward-zero control
			   word, the low dword stored to def+0x1B8 (so a value past int32
			   wraps). [orig: ItemDef_ParseProperty @0x49EB00 (the scale arm
			   @0x49F6E0..0x49F73D: the _atof call @0x49F6F9, fmul by
			   dbl_7C3CC0 = 65536.0 @0x49F6FE, RC=truncate @0x49F710, `fistp
			   qword` @0x49F728, the store @0x49F736)] */
			size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
			char value[128];
			safe_copy(value, sizeof(value), v, vl);
            current.scale_q16 = io::retail_fistp_truncate_low_dword(atof(value) * 65536.0);
            parsed = 1;
		} else if (lower_match_key(lower, ll, "debris_scale", 12)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 12, &vl);
			current.debris_scale = (float)parse_float_n(v, vl); /* -> def+0x1BC */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "husk_sub_parts", 14)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 14, &vl);
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
                if (b) {
                    current.attrib |= (unsigned)b;
                    /* `Door` also defaults the door count (the low byte of the
                       polymorphic +0x890 dword, num_doors) to 1 when it is still 0;
                       an authored num_doors before or after keeps its count.
                       [orig: @0x4a0cb0..0x4a0cb9] */
                    if (b == (int)DEF_ITEM_ATTRIB_DOOR && (current.deathtime_ticks & 0xFF) == 0)
                        current.deathtime_ticks |= 1;
                } else if (k == 6 && memcmp(lo, "parent", 6) == 0) {
                    /* `Parent` is a byte, not a bit: ItemDef+0x548, the gunner-attachment
                       gate (VehicleTraits::attrib_parent). [orig: @0x4a0cd6..0x4a0ce2] */
                    current.attrib_parent = 1;
                } else {
                    int b2 = lookup_item_attrib2(lo, k);
                    if (b2) current.attrib2 |= (unsigned)b2;
                }
            }
            parsed = 1;
        /* Per-item particle-effect keys, matched in the original's chain order
           (lower_match_key requires a separator after the key, so the shared
           'particlefx' prefix cannot shadow the longer keys). All names are
           copied verbatim as strings [orig: ItemDef_ParseProperty @ 0x49eb00]. */
		} else if (lower_match_key(lower, ll, "particlefx", 10)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 10, &vl);
			parse_item_particle_slot(v, vl, &current.particlefx, 0); /* +0x278/+0x298 [orig: @ 0x4a13ad] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particlefxs", 11)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 11, &vl);
			parse_item_particle_slot(v, vl, &current.particlefxs, 1); /* +0x2AE/+0x2EE, 2nd +0x2CE [orig: @ 0x4a140b] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particlefxw1", 12)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 12, &vl);
			parse_item_particle_slot(v, vl, &current.particlefxw1, 1); /* +0x304/+0x344, 2nd +0x324 [orig: @ 0x4a148b] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particlefxw2", 12)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 12, &vl);
			parse_item_particle_slot(v, vl, &current.particlefxw2, 1); /* +0x35A/+0x39A, 2nd +0x37A [orig: @ 0x4a150b] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particlefxw3", 12)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 12, &vl);
			parse_item_particle_slot(v, vl, &current.particlefxw3, 0); /* +0x3AE/+0x3CE, NO secondary [orig: @ 0x4a158b] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particlefxw4", 12)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 12, &vl);
			parse_item_particle_slot(v, vl, &current.particlefxw4, 0); /* +0x3E2/+0x402, NO secondary [orig: @ 0x4a15eb] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particledeath", 13)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 13, &vl);
			Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particledeath, sizeof(current.particledeath), tok[0].s,
                          tok[0].len); /* +0x416 [orig: @ 0x4a164b] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particleh2odeath", 16)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 16, &vl);
			Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particleh2odeath, sizeof(current.particleh2odeath), tok[0].s,
                          tok[0].len); /* +0x44A [orig: @ 0x4a168e] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particlefire", 12)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 12, &vl);
			Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlefire, sizeof(current.particlefire), tok[0].s,
                          tok[0].len); /* +0x47E [orig: @ 0x4a16d0] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particleother", 13)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 13, &vl);
			Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particleother, sizeof(current.particleother), tok[0].s,
                          tok[0].len); /* +0x4B2 [orig: @ 0x4a1713] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particlefinale", 14)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 14, &vl);
			Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlefinale, sizeof(current.particlefinale), tok[0].s,
                          tok[0].len); /* +0x4E4 [orig: @ 0x4a175b] */
            parsed = 1;
		} else if (lower_match_key(lower, ll, "particlespawn", 13)) {
			size_t vl;
			const char *v = consume_value_span(trimmed, tlen, 13, &vl);
			Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlespawn, sizeof(current.particlespawn), tok[0].s,
                          tok[0].len); /* +0x506 [orig: @ 0x4a179d] */
            parsed = 1;
		}

		if (parsed) validate_property(DefRecordKind::Item, trimmed, tlen, current.unmodeled_count, report, it.line, current.display_name);
		if (!parsed) {
			authoring_issue(current.unmodeled_count, report, it.line, current.display_name, trimmed, tlen);
		}
	}

    if (in_block) {
        authoring_issue(out->unmodeled_count, report, it.line, current.display_name, "end", 3, DefIssueCode::MalformedBlock);
        free(current.emplacement_attachments);
    }
    return 0;
}

int def_parse_items(const char *path, DefItemsFile *out, DefParseReport *report) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_items_buf(buf, file_len, out, report);
    free(buf);
    return rc;
}

int def_parse_items_memory(const uint8_t *data, size_t size, DefItemsFile *out, DefParseReport *report) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    return parse_items_buf((const char *)data, size, out, report);
}

void def_free_items(DefItemsFile *f) {
    if (!f) return;
    for (size_t i = 0; i < f->count; ++i) {
        free(f->entries[i].emplacement_attachments);
    }
    free(f->entries);
    memset(f, 0, sizeof(*f));
}

} // namespace opennova::def
