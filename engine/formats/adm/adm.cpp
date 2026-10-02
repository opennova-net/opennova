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
//
// `dropped` names each line whose input the table leaves out: a line the walk
// never hands the row parser (a comment, one whose first token starts with '/')
// [orig: File_ParseASCIIFile @0x53D810, called from AnimMap_LoadAdmFile
// @0x40CC40 at @0x40CEB4; the token count test @0x53D915, the '/' test
// @0x53D91E]; a row that registers nothing (a key naming no slot, no clip
// before the row ends) [orig: AnimMap_ParseConfigLine @0x40CB60, the slot test
// @0x40CBA4, the clip loop @0x40CBAE..0x40CC05]; and, on a row the table
// keeps, what follows a '/'-led token (@0x40CBD2) or a comment after its clips
// (the tokenizer's cut, io/ascii_config.h). The game registers a clip past the
// eighth too (its loop has no cap), but a parsed row holds 8: that line blocks.
// A blank line holds nothing and is not reported (the canonical form writes
// some).
int adm_parse_buffer(const char *bytes, size_t size, AdmFile *out, std::vector<AdmDroppedLine> *dropped) {
    if (!bytes || !out) return -1;
    memset(out, 0, sizeof(AdmFile));

    std::vector<AdmEntry> entries;
    size_t line = 0;
    io::for_each_config_line_span(bytes, size, [&](const io::ConfigTokens &row, const io::ConfigLineSpan &span) {
        ++line;
        const auto drop = [&](std::string what, bool blocks, size_t kept) {
            if (!dropped) return;
            AdmDroppedLine d;
            d.line = line;
            d.row = kept;
            d.key = row.token(0);
            d.what = std::move(what);
            d.blocks = blocks;
            dropped->push_back(std::move(d));
        };
        // Whether a comment cut the line (the tokenizer stopped at its "//" or ';').
        const bool comment = span.cut < span.end && (bytes[span.cut] == '/' || bytes[span.cut] == ';');
        if (row.count == 0) {
            if (comment) drop("A comment: the game skips the line.", false, SIZE_MAX);
            else if (span.cut > span.begin + row.skip) drop("The line holds no token: the game skips it.", false, SIZE_MAX);
            return;
        }
        if (row.tokens[0][0] == '/') {
            drop("The line starts with '/': the game skips it.", false, SIZE_MAX);
            return;
        }
        const char *key = row.token(0);
        AdmEntry entry;
        memset(&entry, 0, sizeof(entry));
        copy_trimmed(entry.key, sizeof(entry.key), key, key + strlen(key));
        if (adm_slot_name(entry.key).empty()) {
            drop("The key names no animation slot: the game registers nothing for the line.", false, SIZE_MAX);
            return;
        }
        int ended = 0;   // the '/'-led token that ends the row (0: none)
        size_t over = 0; // clips past the eighth
        for (int i = 1; i < row.count; ++i) {
            const char *clip = row.token(i);
            if (clip[0] == '/') {
                ended = i;
                break;
            }
            if (clip[0] == '\0') continue;
            if (entry.variant_count >= ADM_MAX_VARIANTS) {
                ++over;
                continue;
            }
            copy_trimmed(entry.variants[entry.variant_count],
                         sizeof(entry.variants[entry.variant_count]),
                         clip, clip + strlen(clip));
            if (entry.variants[entry.variant_count][0] != '\0')
                ++entry.variant_count;
        }
        if (entry.variant_count == 0) {
            drop(ended ? "The row ends at a token that starts with '/' before any clip: the game registers nothing for it."
                       : "The row names no clip: the game registers nothing for it.",
                 false, SIZE_MAX);
            return;
        }
        const size_t kept = entries.size();
        entries.push_back(entry);
        // The last token the row reads; a comment inside its value (a token the cut left
        // unterminated runs to the line's end) is the token's, not dropped.
        const int last = (ended ? ended : row.count) - 1;
        if (over)
            drop("The row names " + std::to_string(size_t(ADM_MAX_VARIANTS) + over) +
                         " clips: the game registers every one, but a table row holds at most 8.",
                 true, kept);
        else if (ended)
            drop(std::string("The row ends at '") + row.token(ended) + "': the game ignores the rest of the line.", false,
                 kept);
        else if (comment && span.token_end[last] <= span.cut)
            drop("The comment after the row's clips: the game ignores it.", false, kept);
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
