// ADM animation-definition writer: the canonical stock row shape, from scratch
// (ADR 0003). Parity with retail's hand-edited tables is parse-equality over
// this form, not byte identity (the ADR 0021 writer-policy shape).
// [orig: AnimMap_ParseConfigLine @0x40cb60 -- a row is the key before the first
//  quote, then every quoted token as one variant of that slot; the parser
//  keeps only rows carrying "anim_"]

#include <formats/adm/adm.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace opennova::adm {

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
        if (e.key[0] == '\0' || std::strncmp(e.key, "anim_", 5) != 0) return -1;
        if (e.variant_count == 0 || e.variant_count > static_cast<size_t>(ADM_MAX_VARIANTS)) return -1;
        out += e.key;
        out += "\t\t\t\t";
        for (size_t v = 0; v < e.variant_count; ++v) {
            if (e.variants[v][0] == '\0' || std::strchr(e.variants[v], '"') != nullptr) return -1;
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
