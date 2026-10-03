// ADM animation-definition map: the parser and the canonical-form writer.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

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

// A line whose input the parser leaves out of the table (adm_parse_buffer's
// `dropped`): what the game ignores there, or what the parsed model cannot
// hold (`blocks`). Blank lines hold nothing and are not reported.
struct AdmDroppedLine {
    size_t line = 0;       // 1-based, as the retail walk numbers the lines
    size_t row = SIZE_MAX; // the table row the line keeps (an index into entries), else SIZE_MAX
    std::string key;       // the line's first token ("" for none)
    std::string what;      // a sentence: what is left out and why
    bool blocks = false;   // input the table would lose that the game reads (a ninth clip)
};

int adm_parse(const char *path, AdmFile *out);
// `dropped`, when given, receives every line whose input the table leaves out.
int adm_parse_buffer(const char *bytes, size_t size, AdmFile *out,
                     std::vector<AdmDroppedLine> *dropped = nullptr);
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
// Why the writer refuses a row (the reasons above, as a sentence), or null when
// it writes it.
const char *adm_row_problem(const AdmEntry &entry);

} // namespace opennova::adm
