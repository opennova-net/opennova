// ADM animation definition file parser: anim slot rows of .bad clip variants.

#include <formats/adm/adm.h>
#include <base/io/ascii_config.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

namespace opennova::adm {

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

// Copy a trimmed substring into dst, null-terminated, capped at max_len chars.
static void copy_trimmed(char *dst, size_t dst_size,
                         const char *start, const char *end) {
    size_t len;
    // Trim leading
    while (start < end && isspace((unsigned char)*start)) ++start;
    // Trim trailing
    while (end > start && isspace((unsigned char)*(end - 1))) --end;

    len = (size_t)(end - start);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, start, len);
    dst[len] = '\0';
}

// --------------------------------------------------------------------------
// API
// --------------------------------------------------------------------------

// Parse a .adm from an in-memory buffer (does not take ownership of `bytes`).
// Used by embedders that read assets from a VFS (PFF archive) rather than disk.
// The rows arrive through the shared retail config reader (io/ascii_config.h:
// CRLF lines, space/comma/tab tokens, '"' quoting, "//" and ';' comments).
// Per row: token 0 names the anim slot, and every later token is a clip
// variant on that one slot's ring until a token starting with '/' ends the
// row; an empty token is skipped [orig: AnimMap_ParseConfigLine @0x40CB60 —
// the slot lookup @0x40CB97, the '/' break @0x40CBD0..0x40CBD2, the empty
// skip @0x40CBD4..0x40CBD6]. The slot is the key past its first five
// characters, whatever they are (adm_slot_name): `ANIM_RESET` and `xxxx_reset`
// name slot 0 as `anim_reset` does. Which of the 252 slot names a key's tail
// matches is the runtime's lookup (its table lives in runtime/world), and a
// row that names none registers nothing there; this parser keeps every key
// longer than five characters. A row with no clip registers nothing and never
// fails the file.
int adm_parse_buffer(const char *bytes, size_t size, AdmFile *out) {
    if (!bytes || !out) return -1;
    memset(out, 0, sizeof(AdmFile));

    std::vector<AdmEntry> entries;
    io::for_each_config_line(bytes, size, [&](const io::ConfigTokens &row) {
        const char *key = row.token(0);
        AdmEntry entry;
        memset(&entry, 0, sizeof(entry));
        copy_trimmed(entry.key, sizeof(entry.key), key, key + strlen(key));
        if (adm_slot_name(entry.key).empty()) return;
        for (int i = 1; i < row.count; ++i) {
            const char *clip = row.token(i);
            if (clip[0] == '/') break;
            if (clip[0] == '\0' || entry.variant_count >= ADM_MAX_VARIANTS) continue;
            copy_trimmed(entry.variants[entry.variant_count],
                         sizeof(entry.variants[entry.variant_count]),
                         clip, clip + strlen(clip));
            if (entry.variants[entry.variant_count][0] != '\0')
                ++entry.variant_count;
        }
        if (entry.variant_count != 0) entries.push_back(entry);
    });

    if (!entries.empty()) {
        out->entries = (AdmEntry *)malloc(entries.size() * sizeof(AdmEntry));
        if (!out->entries) return -1;
        memcpy(out->entries, entries.data(), entries.size() * sizeof(AdmEntry));
        out->count = entries.size();
    }
    return 0;
}

int adm_parse(const char *path, AdmFile *out) {
    FILE *f;
    long file_len;
    char *data;
    size_t data_size;
    int rc;

    if (!path || !out) return -1;

    f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    file_len = ftell(f);
    if (file_len < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    data_size = (size_t)file_len;

    data = (char *)malloc(data_size ? data_size : 1);
    if (!data) { fclose(f); return -1; }
    if (data_size > 0 && fread(data, 1, data_size, f) != data_size) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    rc = adm_parse_buffer(data, data_size, out);
    free(data);
    return rc;
}

void adm_free(AdmFile *af) {
    if (!af) return;
    free(af->entries);
    af->entries = NULL;
    af->count = 0;
}

} // namespace opennova::adm
