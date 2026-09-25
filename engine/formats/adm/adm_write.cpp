// ADM animation-definition writer: the canonical stock row shape, from scratch
// (ADR 0003). Parity with retail's hand-edited tables is parse-equality over
// this form, not byte identity (the ADR 0021 writer-policy shape).
// [orig: AnimMap_ParseConfigLine @0x40cb60 -- token 0 is the key, and a row
//  registers only when the key past its first five characters names one of
//  the 252 slots (AnimMap_FindSlotByName @0x40cfa0, stricmp on key + 5); every
//  later token is a variant of that slot, and a token starting with '/' ends
//  the row (@0x40cbd2). The tokenizer (io/ascii_config.h) splits on space,
//  comma and tab outside quotes and cuts the line at an unquoted "//" or ';'.]
// So the writer quotes every variant and refuses what would not read back as
// written: a key that is not one plain token, a variant with a quote, a
// control character, a leading '/' or edge whitespace (the parser trims it).

#include <formats/adm/adm.h>

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
        if (std::memchr(e.key, '\0', sizeof(e.key)) == nullptr || e.key[0] == '\0' ||
            std::strncmp(e.key, "anim_", 5) != 0 || !plain_key(e.key))
            return -1;
        if (e.variant_count == 0 || e.variant_count > static_cast<size_t>(ADM_MAX_VARIANTS)) return -1;
        out += e.key;
        out += "\t\t\t\t";
        for (size_t v = 0; v < e.variant_count; ++v) {
            if (std::memchr(e.variants[v], '\0', sizeof(e.variants[v])) == nullptr ||
                !plain_variant(e.variants[v]))
                return -1;
            if (v > 0) out += ' ';
            out += '"';
            out += e.variants[v];
            out += '"';
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
    FILE *f = std::fopen(path, "wb");
    if (f == nullptr) return -1;
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return written == bytes.size() ? 0 : -1;
}

} // namespace opennova::adm
