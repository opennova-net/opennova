// ADM animation-definition parser. Runtime reads only.
#ifndef ADM_H
#define ADM_H

#include <stddef.h>

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

} // namespace opennova::adm

#endif // ADM_H
