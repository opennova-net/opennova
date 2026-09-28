// ADM animation-definition map: the parser and the canonical-form writer.
#pragma once

#include <stddef.h>

#include <string>
#include <string_view>

#include <base/io/strutil.h>

namespace opennova::adm {

inline constexpr int ADM_MAX_VARIANTS = 8;

// The slot a row's key names: the key past its first five characters,
// whatever those are (`anim_` in every retail table), which the runtime
// compares without case against its 252 slot names, "reset" being slot 0
// [orig: AnimMap_FindSlotByName @0x40cfa0, stricmp on key + 5, over
// g_AnimStateNameTable @0x8135F0]. A key of five characters or fewer names no
// slot here. That is our rule: retail's lookup reads on past such a key's end
// into the rest of its line, which the tokenizer cuts in place
// (Terrain_TokenizeConfigLine @0x53CB60), and no table carries one.
inline std::string_view adm_slot_name(std::string_view key) {
    return key.size() > 5 ? key.substr(5) : std::string_view();
}

// Whether `key` names the slot `slot` (a slot name: "reset", "wpn_idle").
inline bool adm_key_names_slot(std::string_view key, std::string_view slot) {
    const std::string_view name = adm_slot_name(key);
    return !name.empty() && strutil::iequals(name, slot);
}

// The key the engine looks the slot up by, lower case: "anim_" and the slot
// name, the spelling every lookup uses (world::infantry_anim_key,
// world::body_anim_adm_key); empty only for a key of five characters or
// fewer (adm_slot_name). Whether the name is one of the 252 slots is
// anim::adm_slot_index's question, which answers -1 for a key that names
// none. Two rows whose keys differ only before their sixth character name
// one slot.
inline std::string adm_slot_key(std::string_view key) {
    const std::string_view name = adm_slot_name(key);
    return name.empty() ? std::string() : "anim_" + strutil::to_lower(name);
}

typedef struct AdmEntry {
    char key[64];
    // Every quoted clip on a row is one variant in the slot's circular ring.
    size_t variant_count;
    char variants[ADM_MAX_VARIANTS][64];
} AdmEntry;

typedef struct AdmFile {
    AdmEntry *entries;
    size_t count;
} AdmFile;

int adm_parse(const char *path, AdmFile *out);
int adm_parse_buffer(const char *bytes, size_t size, AdmFile *out);
void adm_free(AdmFile *af);

// The canonical-form writer (adm_write.cpp): one leading blank line, rows of
// `key<4 tabs>"variant" "variant"`, CRLF line ends and the CRLF x3 + NUL
// trailer the stock tables end with. Parity with retail's hand-edited files is
// parse-equality over this form. Returns -1 for a row the parser could not
// read back as written: a key of five characters or fewer (it names no slot)
// or that is not one plain token (a space, tab, comma, quote, ';' or "//"
// splits or cuts it), no variants, or a variant holding a quote or a control
// character, starting with '/' (which ends a row) or with edge whitespace
// (which the parser trims).
int adm_write_buffer(const AdmFile *af, std::string &out);
int adm_write(const char *path, const AdmFile *af);

} // namespace opennova::adm
