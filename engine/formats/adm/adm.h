// ADM animation-definition map: the parser and the canonical-form writer.
#pragma once

#include <stddef.h>

#include <string>

namespace opennova::adm {

inline constexpr int ADM_MAX_VARIANTS = 8;

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
// read back (a key without the anim_ prefix, no variants, a quote in a name).
int adm_write_buffer(const AdmFile *af, std::string &out);
int adm_write(const char *path, const AdmFile *af);

} // namespace opennova::adm
