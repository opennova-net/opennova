#include <formats/def/def.h>
#include <base/io/crt_ftol.h>

// ITEMS.DEF: one record per world item, the largest of the families.

#include "def_notes.h"
#include "def_scan.h"

#include <base/io/strutil.h>

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

/* Which of a value's two words the token is (DefItemDef::type_word): 1 for foliage and object, the
   second the chain reads for 2 and 6, else 0. */
static uint8_t item_type_word(const char *s, size_t len) {
    char low[16];
    size_t ll = len < 15 ? len : 15;
    to_lower_buf(low, s, ll);
    return ((ll == 7 && memcmp(low, "foliage", 7) == 0) || (ll == 6 && memcmp(low, "object", 6) == 0)) ? 1 : 0;
}

/* The twelve weapon userpoint keys in slot order (def.h weapon_userpoints), -1
   for any other key [orig: ItemDef_ParseProperty @ 0x4a0ff2..0x4a12e1] */
static int weapon_userpoint_slot(const char *key) {
    static const char *const k_keys[12] = {
        "weaplbup",  "weaplmup",  "weaplcup",  "weaprbup",  "weaprmup",  "weaprcup",
        "weaplbup2", "weaplmup2", "weaplcup2", "weaprbup2", "weaprmup2", "weaprcup2",
    };
    for (int i = 0; i < 12; ++i)
        if (key_is(key, k_keys[i])) return i;
    return -1;
}

/* Anchored particle-effect slot args: <effect> <userpoint> [<secondary_effect>].
   The original copies tokens 1 and 2 unguarded into 32-char slots and reads the
   third value only when the line carries more than 3 tokens (argc > 3, key
   included); extra tokens beyond those are ignored. `with_secondary` is 0 for
   particlefx/particlefxw3/particlefxw4, which never read a third token. A token
   past the line's count reads as "": the tokenizer resets the first three tokens
   to "" for every line, so a missing one stores an empty name over what an
   earlier line of the slot left (jox01's `particlefx fx_Mosquitos_2m_L`: no
   userpoint).
   [orig: ItemDef_ParseProperty @ 0x49eb00, particlefx chain @ 0x4a13ad..0x4a15eb,
   the copies @ 0x4a13bf..0x4a13fc; Terrain_TokenizeConfigLine @ 0x53cb71..0x53cb81] */
static void parse_item_particle_slot(const io::ConfigTokens &tokens, DefItemParticleFx *slot,
                                     int with_secondary) {
    copy_token(slot->effect, sizeof(slot->effect), tokens, 1);
    copy_token(slot->userpoint, sizeof(slot->userpoint), tokens, 2);
    if (with_secondary && tokens.count > 3)
        copy_token(slot->secondary_effect, sizeof(slot->secondary_effect), tokens, 3);
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

   `bob` is ItemDef +0x93C (the IDB's `bob`, ex unk591): the key
   ItemDef_ParsePhysicsProperty compares for it is the string aBob @0x7C7D78
   (62 6F 62 00; the IDB typed it as the pointer off_7C7D78 until 2026-10-04)
   [orig: the compare @0x49DEA4..0x49DEAA, the store to +0x93C @0x49DECE; the
   allocator's 5 to the same +0x93C @0x49E4F0]. */
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
    d->bob = 5;            /* [orig: bob @0x49E4F0] */
    d->flip = 45;          /* [orig: flip] */
    d->hand_brake = 1;     /* [orig: handBrake] */
    d->tire_slip = 5;      /* [orig: tireSlip] */
    /* The mission editor's AI keys as its parse seeds them at each `begin` (the game keeps none)
       [orig: JOTACmed.exe ItemsDef_ParseToken @0x431159..0x431179]. */
    d->max_attack_dist = 16;
    d->max_engagement_dist = 320;
    d->min_engagement_dist = 16;
    d->fire_timer = 10;
}

/* Shared items.def parser over an in-memory buffer. The caller owns `buf` and must have
   zeroed `out` first. Lets both the path loader and the VFS/PFF byte loader share one parser.

   Every line reaches it as the retail tokenizer cuts it (defscan::for_each_def_line), every
   key is the whole first token compared without case, and a single-text key takes its first
   value token, not the rest of the line [orig: ItemDef_ParseProperty @0x49EB00 — _stricmp
   on tokens[1] throughout, the strcpy of tokens[2] in every name arm].

   A record is allocated at its `begin` and only closed by `end`
   [orig: the `begin` arm @0x49EB88..0x49EBB3, ItemDef_AllocateWithDefaults @0x49E3B0;
   `end` @0x49EB1D clears the open flag]: a `begin` inside an open block keeps the open
   record and starts the next, and a block the file never closes is still an item. Lines
   outside a block are ignored (the open-flag test @0x49EC1A). */
static int parse_items_buf(const char *buf, size_t file_len, DefItemsFile *out, DefParseReport *report,
                           DefTextNoter &noter) {
    size_t entries_cap = 0;
    DefItemDef current;
    def_init_item(current);
    int in_block = 0;
    bool powerup_branch = false, numeric_branch = false;
    bool indent_noted = false;
    size_t emplacement_attachments_cap = 0;
    size_t number = 0; // the line a finding names, counting from 1

    io::ConfigTokens tokens_state;
    for_each_def_line_noted(buf, file_len, tokens_state, [&](const char *at) { noter.line(at); },
                            [&](io::ConfigTokens &tokens, const char *line, size_t line_len,
                                size_t line_index) {
        const char *key = tokens.tokens[0];
        const char *v = tokens.token(1); // the first value token, "" when none
        const size_t vl = strlen(v);
        number = line_index + 1;
        const std::string as_read = line_as_read(tokens);

        if (key_is(key, "end")) {
            if (!in_block) {
                authoring_issue(out->unmodeled_count, report, number, "", as_read.c_str(), as_read.size());
                return;
            }
            if (in_block) {
                if (powerup_branch && numeric_branch)
                    authoring_issue(current.unmodeled_count, report, number, current.display_name,
                        "powerupdef", 10, DefIssueCode::Unrepresentable);
                /* A record closed with no alias takes "S%06i" of its authored
                   id [orig: the `end` arm @0x49EB2F..0x49EB5F, `cmp
                   [esi+30h], bl` then sprintf(alias, "S%06i", [esi+50h] +
                   186A0h)]. +0x50 holds the id less 100000 (the `id` arm's
                   `sub eax, 186A0h` @0x49EC54, stored @0x49EC5A) and the two
                   32-bit offsets cancel, so `id 105310` reads S105310. A
                   record a nested `begin` or the file's end closes keeps it
                   empty. */
                if (current.sid[0] == '\0') {
                    snprintf(current.sid, sizeof(current.sid), "S%06i", current.id);
                    current.sid_derived = 1;
                }
                DA_PUSH(out->entries, out->count, entries_cap, current);
                def_init_item(current);
                noter.close();
                emplacement_attachments_cap = 0;
                in_block = 0;
            }
            return;
        }

        if (key_is(key, "begin")) {
            if (in_block) {
                // The open record, closed by no `end`, which the writer's form of it would add.
                authoring_issue(out->unmodeled_count, report, number, current.display_name, "end", 3,
                                DefIssueCode::MalformedBlock);
                DA_PUSH(out->entries, out->count, entries_cap, current);
            }
            def_init_item(current); /* [orig: the begin arm calls
                ItemDef_AllocateWithDefaults @0x49E3B0 from ItemDef_ParseProperty @0x49EB00] */
            current.note = noter.open(DefRecordKind::Item);
            emplacement_attachments_cap = 0;
            powerup_branch = numeric_branch = false;
            /* The name is token 1, cut to 46 characters in the line buffer
               itself, where a later short line's stale slot still reads the
               cut (io::ConfigTokens::terminate_at) [orig: the strlen compare
               @0x49EBD9, `mov byte ptr [edx+2Eh], 0` @0x49EBFB] */
            if (vl >= 46) tokens.terminate_at(v + 46);
            copy_token(current.display_name, 47, tokens, 1);
            validate_header(as_read.c_str(), as_read.size(), 5, 47, current.unmodeled_count, report, number, current.display_name);
            in_block = 1;
            return;
        }

        if (!in_block) {
            authoring_issue(out->unmodeled_count, report, number, "", as_read.c_str(), as_read.size());
            return;
        }

        // These are alternate meanings of the original's type-specific union at
        // +0x890 (world/itemdef-re.md). Mixing both cannot be represented by a
        // symbolic powerup definition.
        powerup_branch |= key_is(key, "powerupdef");
        for (const char *numeric : {"deathtime", "clipsize", "num_doors", "first_door", "first_subobject",
                "door_type", "door_dir", "open_rate", "max_angle", "sqb_rate", "sqb_distance", "sqb_error",
                "rotor_parts", "aux_parts", "door_open_sound_id", "door_close_sound_id"})
            numeric_branch |= key_is(key, numeric);
        int parsed = 0;
        bool row_line = false; // a row of its own (an attachment), noted as one

        if (key_is(key, "graphicenemy")) {
            copy_token(current.graphic_enemy, sizeof(current.graphic_enemy), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "textid")) {
            copy_token(current.text_id, sizeof(current.text_id), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "sqb_rate") || key_is(key, "sqb_distance") || key_is(key, "sqb_error")) {
            // These share the door/death fields, including last-write order.
            // Each value goes through _ftol2_sse's SSE2 leg (io/crt_ftol.h).
            // [orig: ItemDef_ParseProperty @0x49EB00, squib arms @0x49F06F (the
            // _ftol2_sse calls @0x49F093 / @0x49F0DC / @0x49F125)]
            const double number = io::retail_atof(v);
            if (key_is(key, "sqb_rate")) current.deathtime_ticks = io::retail_ftol_sse2(62.0 / number);
            else if (key_is(key, "sqb_distance")) current.clipsize = io::retail_ftol_sse2(number * 65536.0);
            else current.door_type = static_cast<uint32_t>(io::retail_ftol_sse2(number * 65536.0));
            parsed = 1;
        } else if (key_is(key, "num_doors") || key_is(key, "first_door") ||
                key_is(key, "first_subobject")) {
            // One byte of the polymorphic +0x890 dword each: the low byte of
            // atol read signed, first_door/first_subobject one less (`sub
            // al,1`), negative -> 0, above 30 -> 30. num_doors -> +0x890 and
            // first_door -> +0x891 also set the Door attrib; first_subobject
            // -> +0x892 does not. [orig: ItemDef_ParseProperty — num_doors
            // @0x49F766..0x49F78A, first_door @0x49F7BA..0x49F7DE,
            // first_subobject @0x49F9B0..0x49F9CC]
            const bool door = key_is(key, "first_door");
            const bool subobject = key_is(key, "first_subobject");
            int8_t value = static_cast<int8_t>(static_cast<uint8_t>(parse_int_n(v, vl)));
            if (door || subobject) value = static_cast<int8_t>(static_cast<uint8_t>(value - 1));
            if (value < 0) value = 0;
            else if (value > 30) value = 30;
            set_def_byte(current.deathtime_ticks, subobject ? 2 : door ? 1 : 0,
                    static_cast<uint8_t>(value));
            if (!subobject) current.attrib |= DEF_ITEM_ATTRIB_DOOR;
            parsed = 1;
        } else if (key_is(key, "rotor_parts") || key_is(key, "aux_parts")) {
            // Four raw atol low bytes of tokens 1..4 into the +0x890..+0x897 byte run
            // the door (+0x890 dword) and clipsize (+0x894 dword) fields share:
            // rotor_parts -> +0x890, +0x891, +0x894, +0x895; aux_parts ->
            // +0x896, +0x897, +0x892, +0x893. [orig: ItemDef_ParseProperty —
            // rotor_parts @0x49EF5D..0x49EFB6, aux_parts @0x49EFF3..0x49F04C]
            uint8_t bytes[4];
            for (int i = 0; i < 4; ++i)
                bytes[i] = static_cast<uint8_t>(io::retail_atol(tokens.token(i + 1)));
            if (key_is(key, "rotor_parts")) {
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
        } else if (key_is(key, "door_type") || key_is(key, "door_dir")) {
            // Bit i set by a nonzero token i, for every token past the key up to bit 30
            // [orig: door_type @0x49F7F0..0x49F86C, door_dir @0x49F872..0x49F8EE]
            uint32_t bits = 0;
            for (int i = 1; i < tokens.count && i <= 30; ++i)
                if (io::retail_atol(tokens.tokens[i]) != 0) bits |= 1u << i;
            if (key_is(key, "door_type")) current.door_type = bits;
            else current.clipsize = static_cast<int32_t>(bits);
            parsed = 1;
        } else if (key_is(key, "open_rate") || key_is(key, "max_angle")) {
            const double number = io::retail_atof(v);
            // [orig: ItemDef_ParseProperty @0x49EB00 (the _ftol2_sse calls
            // @0x49F91E for open_rate, @0x49F96D for max_angle)]
            if (key_is(key, "open_rate")) current.door_open_rate_q16 = io::retail_ftol_sse2(65536.0 / (number * 62.0));
            else current.door_max_angle_bam = io::retail_ftol_sse2(number * (1.0 / 360.0) * 4294967295.0);
            parsed = 1;
        } else if (key_is(key, "door_open_sound_id") || key_is(key, "door_close_sound_id")) {
            /* strncpy(.., tokens[2], 0x18) [orig: @0x49FBB4 / @0x49FBF5] */
            copy_token(key_is(key, "door_open_sound_id") ? current.door_open_sound
                                                         : current.door_close_sound,
                    25, tokens, 1);
            parsed = 1;
        } else if (key_is(key, "mana")) {
            current.mana = signed_i16_value(parse_int_n(v, vl));
            parsed = 1;
        } else if (key_is(key, "music")) {
            current.music_location = signed_i16_value(parse_int_n(v, vl));
            parsed = 1;
        } else if (key_is(key, "score")) {
            /* atol of the first value token, stored as a signed word
               [orig: ItemDef_ParseProperty @0x4A0228..0x4A0242] */
            current.score = signed_i16_value(parse_int_n(v, vl));
            parsed = 1;
        } else if (key_is(key, "id")) {
            current.id = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "pcvehicle_spawnlist")) {
            /* Every token past the key [orig: @0x4A0255..0x4A02DE] */
            current.vehicle_spawn_mask = 0;
            for (int i = 1; i < tokens.count; ++i) {
                const int id = io::retail_atol(tokens.tokens[i]);
                int slot = 0;
                while (slot < out->vehicle_spawn_id_count && out->vehicle_spawn_ids[slot] != id)
                    ++slot;
                if (slot == out->vehicle_spawn_id_count && slot < DEF_VEHICLE_SPAWN_SLOTS)
                    out->vehicle_spawn_ids[out->vehicle_spawn_id_count++] = id;
                if (slot < DEF_VEHICLE_SPAWN_SLOTS)
                    current.vehicle_spawn_mask |= uint32_t(1) << slot;
            }
            parsed = 1;
        } else if (key_is(key, "sid")) {
            /* strncpy(alias, tokens[2], 15) [orig: @0x49EC69..0x49EC8D] */
            copy_token(current.sid, 16, tokens, 1);
            parsed = 1;
        } else if (key_is(key, "type")) {
            /* A token the chain does not know leaves the type as it was
               [orig: the type chain @0x4A02E4..0x4A04B7] */
            const int type = item_type_from_string(v, vl);
            if (type != DEF_ITEM_TYPE_UNSET) {
                current.type = type;
                /* The word the file spells the value with, of the two the chain reads for it */
                current.type_word = item_type_word(v, vl);
            }
            parsed = 1;
        } else if (key_is(key, "graphic")) {
            copy_token(current.graphic, sizeof(current.graphic), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "anim_def")) {
            copy_token(current.anim_def, sizeof(current.anim_def), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "husk")) {
            copy_token(current.husk, sizeof(current.husk), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "hp")) {
            current.hp = signed_i16_value(parse_int_n(v, vl)); /* healthMax i16 @+0x17C */
            parsed = 1;
        } else if (key_is(key, "sound_profilefemale")) {
            /* The female-variant profile [orig: ItemDef_ParseProperty
               "sound_profileFemale" @ 0x49fb76 -> def+0x26C]. */
            copy_token(current.sound_profile_female, sizeof(current.sound_profile_female), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "sound_profile")) {
            /* "sound_profile" retargets the female slot too while it still
               tracks the primary (both seed to the "default" profile at alloc;
               an explicit sound_profileFemale detaches it). The original
               compares the two resolved profile POINTERS and rewrites +0x26C
               only when equal [orig: @ 0x49fb0f-0x49fb64 (the +0x26C==+0x268
               gate); alloc seed @ 0x49e3f5-0x49e408]; the name compare is the
               same rule over our unresolved names. */
            if (strcmp(current.sound_profile_female, current.sound_profile) == 0)
                copy_token(current.sound_profile_female, sizeof(current.sound_profile_female), tokens, 1);
            copy_token(current.sound_profile, sizeof(current.sound_profile), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "default_aip")) {
            /* Authoring the profile also RAISES AIData: retail ORs 0x100000 in
               the same arm, so an item carrying default_aip is an AI item
               whether or not its attrib line lists AIData.
               [orig: ItemDef_ParseProperty @0x49eb00 -- the strcpy into
               itemDef+0x8B8 followed by `attrib |= 0x100000`] */
            copy_token(current.default_aip, sizeof(current.default_aip), tokens, 1);
            current.attrib |= DEF_ITEM_ATTRIB_AIDATA;
            parsed = 1;
        } else if (strutil::starts_with_icase(key, "soundloop_")) {
            /* [orig: ItemDef_ParseProperty @ 0x49eb00 -- the "soundloop_" strnicmp
               @ 0x49fec4, then atol of the key past the prefix and
               strncpy(def+0x76B + 24n, tokens[2], 0x18); the 7-slot range matches
               the engine's Soundloop_1..7 sound-type table @ 0x7d0788]. The only
               bounds are n != 0 and n <= 7, signed [orig: `jz` @0x49FEE7, `cmp
               eax, 7; jg` @0x49FEED..0x49FEF0; the address @0x49FF08..0x49FF0D],
               so a negative n lands on the 24-byte sound names ahead of the
               table: -1 duskshot (+0x753), -2 dayshot (+0x73B), -3 dawnshot
               (+0x723), -4 door_close_sound_id (+0x70B), -5 door_open_sound_id
               (+0x6F3), -6 sounddeath (+0x6DB). From -7 down the copy straddles
               the weapon userpoint names and then leaves the record; the port
               writes nothing there (D-ITEMDEF-9). */
            const int32_t slot = io::retail_atol(key + 10);
            char *target = nullptr;
            if (slot >= 1 && slot <= 7) {
                target = current.soundloops[slot - 1];
            } else {
                switch (slot) {
                case -1: target = current.duskshot; break;
                case -2: target = current.dayshot; break;
                case -3: target = current.dawnshot; break;
                case -4: target = current.door_close_sound; break;
                case -5: target = current.door_open_sound; break;
                case -6: target = current.sounddeath; break;
                default: break;
                }
            }
            if (target != nullptr) {
                copy_token(target, 25, tokens, 1);
                parsed = 1;
            }
        } else if (key_is(key, "destroy_timing")) {
            for (int column = 0; column < 3; ++column) {
                // [orig: ItemDef_ParseProperty @0x49EB00 (the _ftol2_sse calls
                // @0x49EE7E / @0x49EEA5 / @0x49EECF)]
                current.destroy_timing_ticks[column] =
                        io::retail_ftol_sse2(io::retail_atof(tokens.token(column + 1)) * 62.0);
            }
            parsed = 1;
        } else if (key_is(key, "dawnshot") || key_is(key, "dayshot") || key_is(key, "duskshot") ||
                key_is(key, "nightshot") || key_is(key, "particletesttime")) {
            /* strncpy(.., tokens[2], 0x18), then two atof seconds
               [orig: the dawnshot / dayshot / duskshot / nightshot arms @0x49FC36 /
               @0x49FCC8 / @0x49FD5B / @0x49FDEE, particletesttime @0x49FAB0] */
            const bool particle_time = key_is(key, "particletesttime");
            const int region = key_is(key, "nightshot") ? 3 : key_is(key, "duskshot") ? 2 :
                    key_is(key, "dayshot") ? 1 : 0;
            if (!particle_time) {
                char *names[] = {current.dawnshot, current.dayshot, current.duskshot, current.nightshot};
                copy_token(names[region], 25, tokens, 1);
            }
            for (int column = 0; column < 2; ++column) {
                const int token = column + (particle_time ? 1 : 2);
                // [orig: ItemDef_ParseProperty @0x49EB00 (the _ftol2_sse calls
                // @0x49FAD1 for particletesttime, @0x49FC79..0x49FE5C for the shots)]
                current.shot_delay_ticks[region][column] =
                        io::retail_ftol_sse2(io::retail_atof(tokens.token(token)) * 62.0);
            }
            parsed = 1;
        } else if (key_is(key, "ai_function")) {
            copy_token(current.ai_function, sizeof(current.ai_function), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "move_function")) {
            copy_token(current.move_function, sizeof(current.move_function), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "render_function")) {
            copy_token(current.render_function, sizeof(current.render_function), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "disk_function")) {
            copy_token(current.disk_function, sizeof(current.disk_function), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "powerupdef")) {
            /* A symbolic powerup definition, not a death/door numeric value: the
               name lands in def+0x890 (the death/door union) and the same arm
               raises the Powerup attrib bit [orig: ItemDef_ParseProperty @0x49F698
               -- the copy @0x49F6C4..0x49F6D0, `or [edx+54h],2` @0x49F6D2]. */
            copy_token(current.powerup_def, sizeof(current.powerup_def), tokens, 1);
            current.attrib |= DEF_ITEM_ATTRIB_POWERUP;
            parsed = 1;
        } else if (key_is(key, "input_function")) {
            copy_token(current.input_function, sizeof(current.input_function), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "virtualdisplay")) {
            /* [orig: ItemDef_ParseProperty @0x49F4E0 -- token 2 -> def+0xD0,
               token 3 -> def+0xE0] */
            copy_token(current.virtual_display, sizeof(current.virtual_display), tokens, 1);
            copy_token(current.virtual_display_userpoint,
                    sizeof(current.virtual_display_userpoint), tokens, 2);
            parsed = 1;
        /* [orig: ItemDef_ParseProperty @ 0x4A1823, def+0x56B / +0x58B] */
        } else if (key_is(key, "ammo_closeattack")) {
            copy_token(current.ammo_closeattack, sizeof(current.ammo_closeattack), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "ammo_marker3")) {
            copy_token(current.ammo_marker3, sizeof(current.ammo_marker3), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "ammo_easyrocket")) {
            copy_token(current.ammo_easyrocket, sizeof(current.ammo_easyrocket), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "ammo_advancedrocket")) {
            copy_token(current.ammo_advancedrocket, sizeof(current.ammo_advancedrocket), tokens, 1);
            parsed = 1;
        /* The closeattack launch USERPOINT name (the AI muzzle; see def.h)
           [orig: ItemDef_ParseProperty launchups_* -> def+0x5EB/+0x5FB] */
        } else if (key_is(key, "launchups_closeattack")) {
            copy_token(current.launchups_closeattack, sizeof(current.launchups_closeattack), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "launchups_rocket")) {
            copy_token(current.launchups_rocket, sizeof(current.launchups_rocket), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "launchups_marker3")) {
            copy_token(current.launchups_marker3, sizeof(current.launchups_marker3), tokens, 1);
            parsed = 1;
        } else if (weapon_userpoint_slot(key) >= 0) {
            /* The twelve weapon userpoint names (def.h weapon_userpoints; the vehicle/eweap
               fire/flash/casing anchors) [orig: ItemDef_ParseProperty @ 0x4a0ff2..0x4a1301] */
            const int slot = weapon_userpoint_slot(key);
            copy_token(current.weapon_userpoints[slot], sizeof(current.weapon_userpoints[slot]),
                    tokens, 1);
            parsed = 1;
        } else if (key_is(key, "primary_weapon")) {
            /* The ewep emplacement's mounted weapon.def entry (the gun entity's slot-0
               weapon; the attach label's text source) [orig: -> def+0x54B primaryWeapon,
               docs/world/itemdef-re.md +0x54b] */
            copy_token(current.primary_weapon, sizeof(current.primary_weapon), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "addeweapg") || key_is(key, "addeweapc") || key_is(key, "addeweap")) {
            /* Authored child-emplacement attachment:
                 <userpoint> <item-def id> [down up right left]
               Packed JOX has three deliberately distinct key spellings. Retain
               the variant instead of folding G/C into the ordinary record. */
            const int kind = key_is(key, "addeweapg") ? DEF_ITEM_EMPLACEMENT_ADDEWEAP_G
                    : key_is(key, "addeweapc")         ? DEF_ITEM_EMPLACEMENT_ADDEWEAP_C
                                                       : DEF_ITEM_EMPLACEMENT_ADDEWEAP;
            Token tok[6];
            const int n = value_tokens(tokens, tok, 6);
            /* Retail has four fixed slots. A fifth valid record is recognized but
               silently ignored. The optional arc is all-or-none: partial tails
               produce authoring diagnostics instead of inventing missing limits. */
            if (n != 2 && n < 6) {
                authoring_issue(current.unmodeled_count, report, number, current.display_name, as_read.c_str(), as_read.size(), DefIssueCode::InvalidValue);
                return;
            }
            {
                parsed = 1;
                if (current.emplacement_attachments_count >= 4) {
                    authoring_issue(current.unmodeled_count, report, number, current.display_name, as_read.c_str(), as_read.size(), DefIssueCode::Unrepresentable);
                    return;
                }
                DefItemEmplacementAttachment attachment;
                memset(&attachment, 0, sizeof(attachment));
                safe_copy(attachment.userpoint, sizeof(attachment.userpoint), tok[0].s, tok[0].len);
                attachment.item_id = parse_int_n(tok[1].s, tok[1].len);
                attachment.kind = kind;
                attachment.angle_count = n >= 6 ? 4 : 0;
                // A row of the item's, one line (its notes its own).
                const int row_step = def_line_step(DefRecordKind::Attachment, key, strlen(key));
                attachment.note = noter.open_nested(DefRecordKind::Attachment, DEF_LINE_ORDER_ROWS, false,
                                                    uint8_t(row_step < 0 ? 0 : row_step));
                row_line = true;
                if (n >= 6) {
                    constexpr int kBamPerDegree = 11930464;
                    attachment.down_angle = parse_int_n(tok[2].s, tok[2].len) * kBamPerDegree;
                    attachment.up_angle = -parse_int_n(tok[3].s, tok[3].len) * kBamPerDegree;
                    attachment.right_angle = parse_int_n(tok[4].s, tok[4].len) * kBamPerDegree;
                    attachment.left_angle = -parse_int_n(tok[5].s, tok[5].len) * kBamPerDegree;
                }
                DA_PUSH(current.emplacement_attachments, current.emplacement_attachments_count,
                        emplacement_attachments_cap, attachment);
                const int stored_slot = static_cast<int>(current.emplacement_attachments_count);
                if (kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G)
                    current.emplacement_g_slot = stored_slot;
                else if (kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C)
                    current.emplacement_c_slot = stored_slot;
            }
        } else if (key_is(key, "phrase_set")) {
            /* Plain signed atol -> target itemDef+0x86C. Presence cannot be
               represented by the zero-initialized value because config 0 is real.
               [orig: @ 0x49F9DB..0x49FA0A] */
            current.phrase_set = parse_int_n(v, vl);
            current.phrase_set_valid = 1;
            parsed = 1;
        } else if (key_is(key, "reverb")) {
            current.reverb = static_cast<int16_t>(parse_int_n(v, vl));
            parsed = 1;
        } else if (key_is(key, "light_transfer")) {
            /* atoi, clamp 0..100, then percent -> float at ItemDef+0x218.
               [orig: @0x4A1A12..0x4A1A50; scale 0.01f @0x7C56A8] */
            int transfer = parse_int_n(v, vl);
            if (transfer < 0) transfer = 0;
            if (transfer > 100) transfer = 100;
            current.light_transfer = static_cast<float>(transfer) * 0.01f;
            parsed = 1;
        } else if (key_is(key, "clipsize")) {
            /* plain atol -> def+0x894, the entity+0x35C magazine reseed source
               [orig: @ 0x49fa2e-0x49fa48; Entity_ResetToSpawnState @ 0x4b97a9] */
            current.clipsize = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "deathtime")) {
            /* seconds -> ticks at parse: 62*v, an explicit 0 -> 496, +62 grace; the
               corpse timer's seed (entity+0x148 at the death edge @ 0x4b9c97)
               [orig: ItemDef_ParseProperty @ 0x49fa6c-0x49faa0 -> def+0x890] */
            int dt = parse_int_n(v, vl) * 62;
            if (dt == 0) dt = 496;
            current.deathtime_ticks = dt + 62;
            parsed = 1;
        /* Vehicle physics-property block, scaled at parse exactly like the original loader
           [orig: ItemDef_ParsePhysicsProperty @0x49d870]. */
        } else if (key_is(key, "climb_speed")) {
            current.climb_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: 293*atol @0x49db4a] */
            parsed = 1;
        } else if (key_is(key, "turnroll")) {
            current.turn_roll = parse_int_n(v, vl); /* raw [orig: @0x49dd2a]; air roll-rate cap = token*192426 at use */
            parsed = 1;
        } else if (key_is(key, "speedpitch")) {
            current.speed_pitch = parse_int_n(v, vl); /* raw [orig: @0x49dd66]; air pitch-rate cap = token*192426 at use */
            parsed = 1;
        } else if (key_is(key, "turn_rate2")) {
            current.turn_rate2 = parse_int_n(v, vl) * 192426; /* deg/s -> BAM/tick [orig: @0x49d8dc] */
            parsed = 1;
        } else if (key_is(key, "turn_rate")) {
            current.turn_rate = parse_int_n(v, vl) * 192426; /* deg/s -> BAM/tick [orig: @0x49d89a] */
            parsed = 1;
        } else if (key_is(key, "torque")) {
            current.torque = parse_int_n(v, vl); /* raw shift count [orig: @0x49dcca] */
            parsed = 1;
        /* The platform-solve tuning block — raw atol like the original parser
           [orig: mass @0x49dc76, lean @0x49ddde, lean_velocity @0x49de1a,
            pitch @0x49de56, pitch_velocity @0x49de92, bob @0x49dece,
            flip @0x49df82]. */
        } else if (key_is(key, "lean_velocity")) {
            current.lean_velocity = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "pitch_velocity")) {
            current.pitch_velocity = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "mass")) {
            current.mass = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "weathervane")) {
            current.weathervane = parse_int_n(v, vl); /* raw [orig: @0x49d8f2] */
            parsed = 1;
        } else if (key_is(key, "minai")) {
            current.min_ai = parse_int_n(v, vl); /* raw [orig: @0x49d95e] */
            parsed = 1;
        } else if (key_is(key, "lean")) {
            current.lean = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "pitch")) {
            current.pitch = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "bob")) {
            current.bob = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "flip")) {
            current.flip = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "max_attack_dist")) {
            /* A key the game knows and keeps nothing of [orig: @0x4a1a7b -> @0x4a1ca6]; the mission
               editor's atol [orig: JOTACmed.exe @0x431a6e -> +0x494]. */
            current.max_attack_dist = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "max_engagement_dist")) {
            current.max_engagement_dist = parse_int_n(v, vl); /* [orig: @0x4a1a94; JOTACmed.exe @0x431a9f -> +0x498] */
            parsed = 1;
        } else if (key_is(key, "min_engagement_dist")) {
            current.min_engagement_dist = parse_int_n(v, vl); /* [orig: @0x4a1aad; JOTACmed.exe @0x431ad0 -> +0x49C] */
            parsed = 1;
        } else if (key_is(key, "fire_timer")) {
            current.fire_timer = parse_int_n(v, vl); /* [orig: @0x4a1ac6; JOTACmed.exe @0x431b01 -> +0x4A0] */
            parsed = 1;
        } else if (key_is(key, "hand_brake")) {
            current.hand_brake = parse_int_n(v, vl); /* raw +0x944 [orig: key @0x7c7d60] */
            parsed = 1;
        } else if (key_is(key, "tire_slip")) {
            current.tire_slip = parse_int_n(v, vl); /* raw +0x940 [orig: key @0x7c7d6c] */
            parsed = 1;
        /* The suspension spring block — raw atol [orig: spring_comp @0x49dbd4,
           spring @0x49db5c, shock @0x49dc10, top_heavy @0x49db98 — the last is
           parsed for PARITY only: retail never reads +0x918 outside the parser,
           the allocator and the debug item editor]. */
        } else if (key_is(key, "spring_comp")) {
            current.spring_comp = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "spring")) {
            current.spring = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "shock")) {
            current.shock = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "top_heavy")) {
            current.top_heavy = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "max_slope")) {
            current.max_slope = parse_int_n(v, vl) * 11930464; /* deg -> BAM [orig: @0x49d91e] */
            parsed = 1;
        } else if (key_is(key, "slip_slope")) {
            current.slip_slope = parse_int_n(v, vl) * 11930464; /* deg -> BAM [orig: @0x49d960] */
            parsed = 1;
        } else if (key_is(key, "player_speed")) {
            current.player_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: @0x49d9a2] */
            parsed = 1;
        } else if (key_is(key, "water_speed")) {
            current.water_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: @0x49d9e4] */
            parsed = 1;
        } else if (key_is(key, "acceleration")) {
            /* token*4; a still-unset deceleration defaults to 2*acceleration (8*token) at
               THIS parse site, mirroring the original's ordering semantics [orig: @0x49da32,
               decel default @0x49da4b]. */
            const int a4 = parse_int_n(v, vl) * 4;
            current.acceleration = a4;
            if (current.deceleration == 0) current.deceleration = a4 * 2;
            parsed = 1;
        } else if (key_is(key, "deceleration")) {
            current.deceleration = parse_int_n(v, vl) * 4; /* [orig: @0x49da84] */
            parsed = 1;
        } else if (key_is(key, "slip_speed")) {
            current.slip_speed = parse_int_n(v, vl) * 4; /* [orig: @0x49dafd] */
            parsed = 1;
        } else if (key_is(key, "physics")) {
            current.physics = parse_int_n(v, vl); /* raw selector [orig: @0x49dac8] */
            parsed = 1;
        } else if (key_is(key, "criticalhp")) {
            current.critical_hp = parse_int_n(v, vl); /* i16 raw at +0x180 */
            parsed = 1;
        } else if (key_is(key, "criticaldrain")) {
            current.critical_drain = parse_int_n(v, vl); /* i16 raw at +0x182 */
            parsed = 1;
        } else if (key_is(key, "noncriticalregen")) {
            current.non_critical_regen = parse_int_n(v, vl); /* i16 raw at +0x184 */
            parsed = 1;
        } else if (key_is(key, "radarsig")) {
            current.radar_sig = parse_int_n(v, vl) & 0xFFFF; /* u16 at +0x178 */
            parsed = 1;
        } else if (key_is(key, "heatsig")) {
            current.heat_sig = parse_int_n(v, vl) & 0xFFFF; /* u16 at +0x17A */
            parsed = 1;
        } else if (key_is(key, "damage_reduc_pp")) {
            /* pp = atof(token 1), max = 1 - pp, then max = atof(token 2) when the
               line carries it [orig: @0x4A0062..0x4A00C3, the count gate
               `cmp [ctx],2; jle` @0x4A00A1]. The subtraction takes the double
               atof left on the x87 stack, not the float it stored: `fst` keeps
               st(0) for `fld1; fsubrp` (@0x4A0089..0x4A0092), so a pp of 0.058
               leaves a max of (float)(1 - 0.058), one float step below
               1.0f - 0.058f. */
            const double pp = io::retail_atof_n(v, vl);
            current.damage_reduc_pp = static_cast<float>(pp);
            current.damage_reduc_max = static_cast<float>(1.0 - pp);
            if (tokens.count > 2) {
                const char *max = tokens.tokens[2];
                current.damage_reduc_max = parse_float_n(max, strlen(max));
            }
            parsed = 1;
        } else if (key_is(key, "armor")) {
            /* 'armor A [B]': +0x190 impact = A then overwritten by B;
               +0x192 blast = A. The historical armor_kz API name is an alias
               for blast armor, not a separate parsed field. Both retail words
               are signed i16 (-1 is the 0xFFFF invulnerable value).
               [orig: @ 0x4a00e7-0x4a0147] */
            const int armor = signed_i16_value(parse_int_n(v, vl));
            current.armor_impact = armor;
            current.armor_blast = armor;
            current.armor_kz = armor;
            if (tokens.count > 2) {
                const char *impact = tokens.tokens[2];
                current.armor_impact = signed_i16_value(parse_int_n(impact, strlen(impact)));
            }
            parsed = 1;
        } else if (key_is(key, "hud_image")) {
            copy_token(current.hud_image, sizeof(current.hud_image), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "unit_type")) {
            /* The low byte of its atol, read unsigned [orig: ItemDef_ParseProperty @0x49ee31 atol
               -> byte +0x196 @0x49ee47]; the atol saturates at 32 bits first (parse_int_n), so
               2147483651 is 2147483647, byte 255. A number past a byte reported, the record holding
               what the game holds. The minimap icon class [orig: @0x50FA70] and the death-dispatch row
               key [orig: Entity_DispatchDeathCallback @0x493f23 vs table @0x815410]. */
            const int read = parse_int_n(v, vl);
            current.unit_type = static_cast<unsigned char>(read);
            if (current.unit_type != read)
                authoring_issue(current.unmodeled_count, report, number, current.display_name, as_read.c_str(), as_read.size(),
                                DefIssueCode::Reinterpreted, std::to_string(current.unit_type).c_str());
            parsed = 1;
        } else if (key_is(key, "shadow")) {
            /* 'shadow <name> <w> <l> <ox> <oy>' — the authored ground-shadow
               blob decal: name -> +0xA0 (a 16-byte slot; retail copies
               unguarded, we truncate), four atof floats ->
               +0x11C/+0x120/+0x124/+0x128 (width/length world units, planar
               offset x/y), read unconditionally: a token past the line's
               count reads as "", atof 0. [orig: ItemDef_ParseProperty
               @ 0x49f3a5..0x49f44c; consumer
               RenderSlot_DrawAuthoredBlobDecal @ 0x5d59d0] */
            copy_token(current.shadow_texture, sizeof(current.shadow_texture), tokens, 1);
            current.shadow_width = (float)io::retail_atof(tokens.token(2));
            current.shadow_length = (float)io::retail_atof(tokens.token(3));
            current.shadow_offset_x = (float)io::retail_atof(tokens.token(4));
            current.shadow_offset_y = (float)io::retail_atof(tokens.token(5));
            parsed = 1;
        /* --- the destruction/husk block [orig: ItemDef_ParseProperty @ 0x49eb00] --- */
        } else if (key_is(key, "huskfinal")) {
            copy_token(current.huskfinal, sizeof(current.huskfinal), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "sounddeath")) {
            /* strncpy(.., tokens[2], 0x18) [orig: @0x49FE81] */
            copy_token(current.sounddeath, 25, tokens, 1);
            parsed = 1;
        } else if (key_is(key, "kz")) {
            /* Death-blast radius in units, plain float -> def+0x198. */
            current.kz = (float)parse_float_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "husk_swap_at_sec")) {
            /* seconds*62 ticks; an authored 0 stores 1.0. [orig: @ 0x49f1ce-0x49f228] */
            const float sec = (float)(parse_float_n(v, vl) * 62.0);
            current.husk_swap_at_sec = (sec == 0.0f) ? 1.0f : sec;
            parsed = 1;
        } else if (key_is(key, "husk_swap_at")) {
            /* Dual-unit: while +0x1A0 is still 0 the value is a PERCENT (atol*0.01),
               else seconds*62 — the witnessed parse-order dependence.
               [orig: @ 0x49f242-0x49f2c2] */
            if (current.husk_swap_at_sec == 0.0f)
                current.husk_swap_at = (float)(parse_int_n(v, vl) * 0.01);
            else
                current.husk_swap_at = (float)(parse_float_n(v, vl) * 62.0);
            parsed = 1;
        } else if (key_is(key, "scale")) {
            /* Signed Q16.16: atof's double times 65536, truncated toward zero
               by `fistp qword` under a temporary round-toward-zero control
               word, the low dword stored to def+0x1B8 (so a value past int32
               wraps). [orig: ItemDef_ParseProperty @0x49EB00 (the scale arm
               @0x49F6E0..0x49F73D: the _atof call @0x49F6F9, fmul by
               dbl_7C3CC0 = 65536.0 @0x49F6FE, RC=truncate @0x49F710, `fistp
               qword` @0x49F728, the store @0x49F736)] */
            current.scale_q16 = io::retail_fistp_truncate_low_dword(io::retail_atof(v) * 65536.0);
            parsed = 1;
        } else if (key_is(key, "debris_scale")) {
            current.debris_scale = (float)parse_float_n(v, vl); /* -> def+0x1BC */
            parsed = 1;
        } else if (key_is(key, "husk_sub_parts")) {
            current.husk_sub_parts = parse_int_n(v, vl); /* -> +0x100 count byte */
            parsed = 1;
        } else if (key_is(key, "husk_sub_part_types")) {
            /* Each value 'NN_NAME': split at the FIRST '_', which the arm cuts
               in the line buffer itself (a later short line's stale slot reads
               the cut), slot = atol(NN) - 1 (0..15), the remainder (internal
               underscores kept) resolved case-insensitively against the engine
               debris-type table names; at most 16 entries. The walk runs the
               slots from 1 to the first NULL one, past the line's count into
               the slots earlier lines left (io::ConfigTokens::slot).
               [orig: @ 0x49F31A..0x49F396 — the 16 cap @0x49F330, strstr(slot,
               "_") @0x49F343, the cut @0x49F351, atol @0x49F358, the unsigned
               `cmp edi, 0Fh; ja` @0x49F365, the NULL test @0x49F393;
               DeathPieceType_FindByName @ 0x57b310] */
            int processed = 0;
            for (int i = 1; i < io::kConfigMaxTokens; ++i) {
                const char *value = tokens.slot(i);
                if (value == nullptr || processed >= 16) break;
                const char *us = strchr(value, '_');
                if (us == nullptr) continue;
                tokens.terminate_at(us);
                const uint32_t slot = static_cast<uint32_t>(io::retail_atol(value)) - 1u;
                if (slot > 15u) continue;
                ++processed;
                const char *nm = us + 1;
                current.husk_sub_part_types[slot] =
                        (unsigned char)death_piece_type_index(nm, strlen(nm));
            }
            parsed = 1;
        } else if (key_is(key, "attrib:")) {
            /* Every token past the key -> ItemDefAttrib/Attrib2 bits, ORed into what earlier
               `attrib:` lines set. Unknown tokens (exp1, pilotonly, forceasset, neutral, ...)
               are not in the witnessed map and stay unmapped.
               [orig: ItemDef_ParseProperty @0x49eb00 — the key @0x4A0692, the count gate
               @0x4A06A8, the token loop to the line's count @0x4A0F42..0x4A0F52, the attrib
               ORs `or [..+54h]`, the attrib2 ORs `or [..+58h]` from @0x4A0D0A;
               docs/world/itemdef-re.md] */
            Token tok[kMaxValueTokens];
            const int ntok = value_tokens(tokens, tok, kMaxValueTokens);
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
                } else if (k == 4 && memcmp(lo, "good", 4) == 0) {
                    /* The mission editor's side words, which the game matches and stores nothing for
                       [orig: @0x4a06c3 / @0x4a06db -> @0x4a0cea; JOTACmed.exe ItemsDef_ParseToken
                       @0x431682 / @0x43169f]. */
                    current.attrib_good = 1;
                } else if (k == 4 && memcmp(lo, "evil", 4) == 0) {
                    current.attrib_evil = 1;
                } else {
                    int b2 = lookup_item_attrib2(lo, k);
                    if (b2) current.attrib2 |= (unsigned)b2;
                }
            }
            parsed = 1;
        /* Per-item particle-effect keys. All names are copied verbatim as
           strings [orig: ItemDef_ParseProperty @ 0x49eb00]. */
        } else if (key_is(key, "particlefx")) {
            parse_item_particle_slot(tokens, &current.particlefx, 0); /* +0x278/+0x298 [orig: @ 0x4a13ad] */
            parsed = 1;
        } else if (key_is(key, "particlefxs")) {
            parse_item_particle_slot(tokens, &current.particlefxs, 1); /* +0x2AE/+0x2EE, 2nd +0x2CE [orig: @ 0x4a140b] */
            parsed = 1;
        } else if (key_is(key, "particlefxw1")) {
            parse_item_particle_slot(tokens, &current.particlefxw1, 1); /* +0x304/+0x344, 2nd +0x324 [orig: @ 0x4a148b] */
            parsed = 1;
        } else if (key_is(key, "particlefxw2")) {
            parse_item_particle_slot(tokens, &current.particlefxw2, 1); /* +0x35A/+0x39A, 2nd +0x37A [orig: @ 0x4a150b] */
            parsed = 1;
        } else if (key_is(key, "particlefxw3")) {
            parse_item_particle_slot(tokens, &current.particlefxw3, 0); /* +0x3AE/+0x3CE, NO secondary [orig: @ 0x4a158b] */
            parsed = 1;
        } else if (key_is(key, "particlefxw4")) {
            parse_item_particle_slot(tokens, &current.particlefxw4, 0); /* +0x3E2/+0x402, NO secondary [orig: @ 0x4a15eb] */
            parsed = 1;
        } else if (key_is(key, "particledeath")) {
            copy_token(current.particledeath, sizeof(current.particledeath), tokens, 1); /* +0x416 [orig: @ 0x4a164b] */
            parsed = 1;
        } else if (key_is(key, "particleh2odeath")) {
            copy_token(current.particleh2odeath, sizeof(current.particleh2odeath), tokens, 1); /* +0x44A [orig: @ 0x4a168e] */
            parsed = 1;
        } else if (key_is(key, "particlefire")) {
            copy_token(current.particlefire, sizeof(current.particlefire), tokens, 1); /* +0x47E [orig: @ 0x4a16d0] */
            parsed = 1;
        } else if (key_is(key, "particleother")) {
            copy_token(current.particleother, sizeof(current.particleother), tokens, 1); /* +0x4B2 [orig: @ 0x4a1713] */
            parsed = 1;
        } else if (key_is(key, "particlefinale")) {
            copy_token(current.particlefinale, sizeof(current.particlefinale), tokens, 1); /* +0x4E4 [orig: @ 0x4a175b] */
            parsed = 1;
        } else if (key_is(key, "particlespawn")) {
            copy_token(current.particlespawn, sizeof(current.particlespawn), tokens, 1); /* +0x506 [orig: @ 0x4a179d] */
            parsed = 1;
        }

        if (parsed) {
            validate_property(DefRecordKind::Item, as_read.c_str(), as_read.size(), current.unmodeled_count, report, number, current.display_name);
            // What a writer keeps of the line: its place in the record's order, the file's indentation.
            def_note_line(DefRecordKind::Item, current.line_order, key, strlen(key));
            def_note_indent(out->layout, indent_noted, line, line_len);
            if (!row_line) noter.property(def_line_step(DefRecordKind::Item, key, strlen(key)));
        }
        if (!parsed) {
            authoring_issue(current.unmodeled_count, report, number, current.display_name, as_read.c_str(), as_read.size());
        }
    });
    noter.finish();

    /* A block the file never closes was allocated at its `begin` and stays an
       item [orig: the begin arm @0x49EBA8]; its `end` is missing, which the
       writer's form of it would add. */
    if (in_block) {
        authoring_issue(out->unmodeled_count, report, number, current.display_name, "end", 3, DefIssueCode::MalformedBlock);
        DA_PUSH(out->entries, out->count, entries_cap, current);
    }

    return 0;
}

int def_parse_items(const char *path, DefItemsFile *out, DefParseReport *report) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    DefTextNoter none(buf, file_len, nullptr);
    int rc = parse_items_buf(buf, file_len, out, report, none);
    free(buf);
    return rc;
}

int def_parse_items_memory(const uint8_t *data, size_t size, DefItemsFile *out, DefParseReport *report) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    DefTextNoter none((const char *)data, size, nullptr);
    return parse_items_buf((const char *)data, size, out, report, none);
}

int def_parse_items_memory(const uint8_t *data, size_t size, DefItemsFile *out, DefParseReport *report,
                           DefTextNotes &notes) {
    memset(out, 0, sizeof(*out));
    notes = DefTextNotes();
    if (!data) return -1;
    DefTextNoter noter((const char *)data, size, &notes);
    const int rc = parse_items_buf((const char *)data, size, out, report, noter);
    def_note_baseline(*out, notes);
    return rc;
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
