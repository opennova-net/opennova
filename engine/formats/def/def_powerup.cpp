#include <formats/def/def.h>

// POWERUP.DEF: the pickup rows the Powerup items bind (world/powerup.h).
//
// The retail loader runs the shared ASCII walk twice: a counting pass over the
// `powerup` keyword lines sizes the 576-byte row buffer, then the property pass
// fills a zeroed parse block and copies it into the next row at each `end`.
// A row is the block's name, the scalar keys, the per-class ammo rows and its
// two ActionDef rows (pickup/respawn), which the action-line parser fills line
// by line while an `action` block is open.
// [orig: PowerUpDef_LoadFromFile @0x443350 (the count pass
//  PowerUpDef_CountCallback @0x4425B0, the row pass PowerUpDef_ParseProperty
//  @0x442EE0, the row copy PowerUpDef_RegisterNewEntry @0x442C00);
//  Game_StartMission loads it @0x5256CD and logs "Unable to load powerup.def"
//  and continues when it is missing]

#include "def_notes.h"
#include "def_scan.h"

#include <base/io/ascii_config.h>
#include <base/io/strutil.h>

#include <stdlib.h>
#include <string.h>
#include <string_view>

using namespace opennova::defscan;

namespace opennova::def {

namespace {

bool key_is(const char *key, const char *name) { return strutil::iequals(key, name); }

// `auto` -> -1, else atol [orig: ActionDef_ParseScriptLine @0x40279A / @0x402B2C]
int parse_delay(const char *v) {
    return strutil::iequals(std::string_view(v), "auto") ? -1 : (int)strtol(v, nullptr, 10);
}

// One in-ACTION line. The keys the two powerup handlers read land in the row;
// the others the shared parser accepts (dupsound, ctrlreg, ctrlreginc) are
// dropped, and an unknown key is ignored like the original's: false for a line
// whose key lands nothing in the row (an authoring report's ignored input).
// [orig: ActionDef_ParseScriptLine @0x4023C0 -- function @0x40296E, anim
//  @0x402873, delaystart/delay/delayend @0x40279A/@0x402B2C, soundset/
//  soundsetend/particle/particleuserpoint/texttoken/action_value stores]
bool parse_action_line(DefPowerupAction &action, const io::ConfigTokens &tokens) {
    const char *key = tokens.tokens[0];
    const char *v = tokens.token(1);
    const size_t vl = strlen(v);
    if (key_is(key, "function")) {
        safe_copy(action.function, sizeof(action.function), v, vl);
    } else if (key_is(key, "anim")) {
        safe_copy(action.anim, sizeof(action.anim), v, vl);
    } else if (key_is(key, "delaystart")) {
        action.delaystart = parse_delay(v);
    } else if (key_is(key, "delayend") || key_is(key, "delay")) {
        action.delayend = parse_delay(v);
    } else if (key_is(key, "soundset")) {
        safe_copy(action.soundset, sizeof(action.soundset), v, vl);
    } else if (key_is(key, "soundsetend")) {
        safe_copy(action.soundsetend, sizeof(action.soundsetend), v, vl);
    } else if (key_is(key, "particle")) {
        safe_copy(action.particle, sizeof(action.particle), v, vl);
    } else if (key_is(key, "particleuserpoint")) {
        safe_copy(action.particleuserpoint, sizeof(action.particleuserpoint), v, vl);
    } else if (key_is(key, "texttoken")) {
        safe_copy(action.texttoken, sizeof(action.texttoken), v, vl);
    } else if (key_is(key, "action_value")) {
        action.action_value = (int)strtol(v, nullptr, 10);
    } else {
        return false;
    }
    return true;
}

// What the loader reads past without storing anything, reported to an authoring tool (the
// other families' UnknownProperty): saving drops it, which the game reads the same.
void ignored(DefPowerupFile *out, DefParseReport *report, size_t line, const char *record, const char *key) {
    authoring_issue(out->unmodeled_count, report, line + 1, record, key, strlen(key));
}

int parse_powerup_buffer(const char *buf, size_t file_len, DefPowerupFile *out, DefParseReport *report,
                         DefTextNotes *notes) {
    DefTextNoter noter(buf, file_len, notes);
    // A line of the open row (`key`'s step) or of its open action block.
    const auto noted = [&noter](DefRecordKind kind, const char *key) { noter.property(def_line_step(kind, key, strlen(key))); };
    size_t entries_cap = 0;
    DefPowerupDef current;
    memset(&current, 0, sizeof(current));
    size_t ammo_cap = 0;
    // The three parse-state words: a block is open (dword_A89588), an action
    // block is open inside it (dword_A8958C), and which action row it fills.
    bool in_block = false;
    bool in_action = false;
    DefPowerupAction *action = nullptr;

    // The shared ASCII walk cuts at CR LF only and drops the last byte of an
    // unterminated tail line [orig: File_ParseASCIIFile @0x53D8C7..0x53D8F5,
    //  the tail @0x53D8E9 / @0x53D8EC]; a lone LF is no line break there.
    size_t line_index = (size_t)-1;
    io::for_each_config_line_span(buf, file_len, [&](const io::ConfigTokens &tokens,
                                                     const io::ConfigLineSpan &span) {
        ++line_index;
        noter.line(buf + span.begin);
        if (tokens.count == 0 || tokens.tokens[0][0] == '/') return;
        const char *key = tokens.tokens[0];
        const char *v = tokens.token(1);
        const size_t vl = strlen(v);

        // `powerup <name>` opens a block; a second one while a block is open
        // logs "definition missing end" and is dropped, the open block stays
        // current [orig: @0x442F02..0x442F49].
        if (key_is(key, "powerup")) {
            if (in_block) ignored(out, report, line_index, current.name, key);
            if (!in_block) {
                // The block was zeroed at the previous `end` (or is fresh);
                // only the 16-byte name lands here [orig: strncpy @0x442F3F].
                const size_t n = vl < 16 ? vl : 16;
                memcpy(current.name, v, n);
                current.name[n] = '\0';
                current.open_line = line_index;
                current.note = noter.open(DefRecordKind::Powerup);
                in_block = true;
            }
            return;
        }

        // `end`: closes the open action, else registers the open block, else
        // logs "definition missing start" [orig: @0x442F5D..0x443017].
        if (key_is(key, "end")) {
            if (in_action) {
                if (in_block && action != nullptr) {
                    noter.close();
                    in_action = false;
                    action = nullptr;
                }
            } else if (in_block) {
                noter.close();
                current.end_line = line_index;
                DA_PUSH(out->entries, out->count, entries_cap, current);
                memset(&current, 0, sizeof(current));
                ammo_cap = 0;
                in_block = false;
            } else {
                ignored(out, report, line_index, "", key);
            }
            return;
        }

        // Lines outside a block are ignored [orig: the dword_A89588 test @0x44302C].
        if (!in_block) {
            ignored(out, report, line_index, "", key);
            return;
        }

        // In-action lines go to the action-line parser [orig: @0x443039..0x44304F].
        if (in_action) {
            if (action != nullptr && !parse_action_line(*action, tokens)) ignored(out, report, line_index, current.name, key);
            else if (action != nullptr) noted(DefRecordKind::PowerupAction, key);
            return;
        }

        // `action pickup|respawn` opens an action row; any other name logs
        // "Invalid for powerup" and opens nothing [orig: @0x443056..0x4430F4].
        if (key_is(key, "action")) {
            if (key_is(v, "pickup"))
                action = &current.pickup;
            else if (key_is(v, "respawn"))
                action = &current.respawn;
            else {
                ignored(out, report, line_index, current.name, key);
                return;
            }
            // A block of the row's name read again replaces it: the earlier block is read for nothing.
            if (action->present) noter.drop_nested(action->note);
            memset(action, 0, sizeof(*action));
            action->present = 1;
            action->note = noter.open_nested(DefRecordKind::PowerupAction, DEF_LINE_ORDER_BLOCKS);
            in_action = true;
            return;
        }
        noted(DefRecordKind::Powerup, key); // a line of the row's (a key it reads none of: read for nothing)
        if (key_is(key, "respawn_time")) {
            current.respawn_time = (int)strtol(v, nullptr, 10); // @0x443103
        } else if (key_is(key, "max_respawns")) {
            current.max_respawns = (int)strtol(v, nullptr, 10); // @0x44312C
        } else if (key_is(key, "hp")) {
            current.hp = (int)strtol(v, nullptr, 10); // @0x443155
        } else if (key_is(key, "mana")) {
            current.mana = (int)strtol(v, nullptr, 10); // @0x44317E
        } else if (key_is(key, "weapon")) {
            // The name resolves against the weapon table at runtime; `all`
            // is the -1 sentinel; a name the table lacks logs "Weapon not
            // found" and leaves the row's 0 [orig: @0x4431A7..0x443216].
            // Both fill the one word row+0x34, the later line's kept: `all`
            // leaves no name behind it.
            current.weapon_all = key_is(v, "all") ? 1 : 0;
            if (current.weapon_all) memset(current.weapon, 0, sizeof(current.weapon));
            else safe_copy(current.weapon, sizeof(current.weapon), v, vl);
        } else if (key_is(key, "allammo")) {
            current.allammo = 1; // @0x443220
        } else if (key_is(key, "ammo")) {
            // `ammo <class> <count>` [orig: @0x443240..0x443277]
            DefPowerupAmmo row;
            memset(&row, 0, sizeof(row));
            safe_copy(row.class_name, sizeof(row.class_name), v, vl);
            row.count = (int)strtol(tokens.token(2), nullptr, 10);
            // A row of the row's, one line (its notes its own).
            const int row_step = def_line_step(DefRecordKind::PowerupAmmo, key, strlen(key));
            row.note = noter.open_nested(DefRecordKind::PowerupAmmo, DEF_LINE_ORDER_ROWS, false,
                                         uint8_t(row_step < 0 ? 0 : row_step));
            DA_PUSH(current.ammo, current.ammo_count, ammo_cap, row);
        } else {
            ignored(out, report, line_index, current.name, key); // "unrecognized token" [orig: @0x44328C]
        }
    });
    noter.finish();
    // An unterminated tail block is never registered (retail registers only at `end`): a
    // file the typed model cannot carry as written, which an authoring tool refuses to
    // rewrite until it is closed or removed.
    if (in_block)
        authoring_issue(out->unmodeled_count, report, current.open_line + 1, current.name, "powerup", 7,
                        DefIssueCode::MalformedBlock);
    if (current.ammo != nullptr) free(current.ammo);
    return 0;
}

} // namespace

int def_parse_powerup(const char *path, DefPowerupFile *out, DefParseReport *report) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    const int rc = parse_powerup_buffer(buf, file_len, out, report, nullptr);
    free(buf);
    return rc;
}

namespace {

int parse_powerup_copy(const uint8_t *data, size_t size, DefPowerupFile *out, DefParseReport *report,
                       DefTextNotes *notes) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    char *buf = (char *)malloc(size + 1);
    if (!buf) return -1;
    memcpy(buf, data, size);
    buf[size] = '\0';
    const int rc = parse_powerup_buffer(buf, size, out, report, notes);
    free(buf);
    return rc;
}

} // namespace

int def_parse_powerup_memory(const uint8_t *data, size_t size, DefPowerupFile *out, DefParseReport *report) {
    return parse_powerup_copy(data, size, out, report, nullptr);
}

int def_parse_powerup_memory(const uint8_t *data, size_t size, DefPowerupFile *out, DefParseReport *report,
                             DefTextNotes &notes) {
    notes = DefTextNotes();
    const int rc = parse_powerup_copy(data, size, out, report, &notes);
    def_note_baseline(*out, notes);
    return rc;
}

void def_free_powerup(DefPowerupFile *f) {
    if (!f) return;
    for (size_t i = 0; i < f->count; ++i) free(f->entries[i].ammo);
    free(f->entries);
    f->entries = nullptr;
    f->count = 0;
}

} // namespace opennova::def
