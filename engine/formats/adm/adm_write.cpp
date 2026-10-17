// ADM animation-definition writer: the canonical stock row shape, from scratch
// (ADR 0003). Parity with retail's hand-edited tables is parse-equality over
// this form, not byte identity (the ADR 0021 writer-policy shape).
// [orig: AnimMap_ParseConfigLine @0x40cb60 -- token 0 is the key, and a row
//  registers only when the key past its first five characters names one of
//  the 252 slots (AnimMap_FindSlotByName @0x40cfa0, stricmp on key + 5); every
//  later token is a variant of that slot, and a token starting with '/' ends
//  the row (@0x40cbd2). The tokenizer (io/ascii_config.h) splits on space,
//  comma and tab outside quotes and cuts the line at an unquoted "//" or ';'.]
// So the writer quotes every variant but a 29th (the line's 30th token, which
// the tokenizer never closes, written bare) and refuses what would not read
// back as written: more than 29 variants, a line past 1000 characters, a key
// of five characters or fewer (adm_slot_name: it names no slot, and the parser
// drops its row) or that is not one plain token, a variant with a quote, a
// control character, a leading '/' or edge whitespace (the parser trims it),
// or a 29th that is not one plain token.

#include <formats/adm/adm.h>

#include <base/io/ascii_config.h>
#include <base/io/os_path.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace opennova::adm {

namespace {

bool has_control(const char *s) {
    for (; *s != '\0'; ++s)
        if (static_cast<unsigned char>(*s) < 0x20 || *s == 0x7f) return true;
    return false;
}

// The key is written bare: one token, nothing that splits, quotes or cuts it.
bool plain_key(const char *key) {
    if (has_control(key) || std::strstr(key, "//") != nullptr) return false;
    return std::strpbrk(key, " \t,\";") == nullptr;
}

// A variant is written quoted: no quote inside, no control character, no
// leading '/' (it would end the row) and no edge whitespace (trimmed on read).
bool plain_variant(const char *v) {
    const size_t len = std::strlen(v);
    if (len == 0 || has_control(v) || std::strchr(v, '"') != nullptr || v[0] == '/') return false;
    return v[0] != ' ' && v[0] != '\t' && v[len - 1] != ' ' && v[len - 1] != '\t';
}

} // namespace

// The line a row is written as, its length: the key, four tabs, each clip quoted and a space between them, the
// line's last clip bare when it is the line's 30th token (adm_write_buffer).
static size_t written_row_length(const AdmEntry &e) {
    size_t length = std::strlen(e.key) + 4;
    for (size_t v = 0; v < e.variants.size(); ++v)
        length += (v > 0 ? 1 : 0) + std::strlen(e.variants[v].text) + (v + 1 < size_t(ADM_MAX_VARIANTS) ? 2 : 0);
    return length;
}

const char *adm_row_problem(const AdmEntry &e) {
    if (std::memchr(e.key, '\0', sizeof(e.key)) == nullptr || adm_slot_name(e.key).empty())
        return "A key names its slot past its first five characters (anim_...).";
    if (!plain_key(e.key)) return "A key is one plain token: no space, tab, comma, quote, ';' or \"//\".";
    if (e.variants.empty()) return "A row names at least one clip.";
    if (e.variants.size() > static_cast<size_t>(ADM_MAX_VARIANTS))
        return "A row names at most 29 clips: the game's line reader takes 30 tokens, its key and 29 clips.";
    for (size_t v = 0; v < e.variants.size(); ++v)
        if (std::memchr(e.variants[v], '\0', sizeof(e.variants[v])) == nullptr || !plain_variant(e.variants[v]))
            return "A clip name holds no quote or control character, does not start with '/' (it ends the row) "
                   "and has no space at either end.";
    // The line reader stops at its 30th token's start, so that token is never cut at a closing quote: a 29th clip
    // is written bare [orig: Config_TokenizeLine @0x53CB60, the 30 cap @0x53CC8C..0x53CC93].
    if (e.variants.size() == static_cast<size_t>(ADM_MAX_VARIANTS) && !plain_key(e.variants.back().text))
        return "A row's 29th clip is written bare, the line's 30th token, which the game's line reader never closes: "
               "no space, tab, comma, quote, ';' or \"//\" in it.";
    // The line reader keeps 1000 characters of a line [orig: Config_TokenizeLine @0x53CB60, the clamp
    // @0x53CBBB..0x53CBC3].
    if (written_row_length(e) > io::kConfigMaxLineChars)
        return "A row's line holds 1000 characters at most: the game's line reader cuts it there.";
    return nullptr;
}

int adm_write_buffer(const AdmFile *af, std::string &out) {
    out.clear();
    if (af == nullptr) return -1;
    if (af->count > 0 && af->entries == nullptr) return -1;
    // One leading blank line, `key<4 tabs>"variant" "variant"` rows joined by CRLF,
    // then the CRLF x3 + NUL trailer the stock tables end with (the parser maps
    // embedded NULs to newlines).
    out += "\r\n";
    for (size_t i = 0; i < af->count; ++i) {
        const AdmEntry &e = af->entries[i];
        if (adm_row_problem(e) != nullptr) return -1;
        out += e.key;
        out += "\t\t\t\t";
        for (size_t v = 0; v < e.variants.size(); ++v) {
            if (v > 0) out += ' ';
            const bool bare = v + 1 == static_cast<size_t>(ADM_MAX_VARIANTS); // the line's 30th token
            if (!bare) out += '"';
            out += e.variants[v];
            if (!bare) out += '"';
        }
        if (i + 1 < af->count) out += "\r\n";
    }
    out.append("\r\n\r\n\r\n", 6);
    out.push_back('\0');
    return 0;
}

int adm_write(const char *path, const AdmFile *af) {
    if (path == nullptr) return -1;
    std::string bytes;
    if (adm_write_buffer(af, bytes) != 0) return -1;
    FILE *f = opennova::io::fopen_utf8(path, "wb");
    if (f == nullptr) return -1;
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return written == bytes.size() ? 0 : -1;
}

} // namespace opennova::adm
