// ADM animation definition file parser — pure C implementation.
// Parses key/value pairs from .adm text files.

#include "adm/adm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

// Trim leading/trailing whitespace in-place, returning pointer into buf.
static const char *trim(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start)) ++start;
    while (end > start && isspace((unsigned char)*(end - 1))) --end;
    return start;
    // Note: caller uses (end - start) for length after calling trim
}

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

int adm_parse(const char *path, AdmFile *out) {
    FILE *f;
    long file_len;
    char *data = NULL;
    size_t data_size;
    size_t cap = 0;
    size_t line_start, pos;

    if (!path || !out) return -1;
    memset(out, 0, sizeof(AdmFile));

    f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    file_len = ftell(f);
    if (file_len < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);

    data_size = (size_t)file_len;
    // Allocate +1 for trailing newline sentinel
    data = (char *)malloc(data_size + 1);
    if (!data) { fclose(f); return -1; }
    if (data_size > 0 && fread(data, 1, data_size, f) != data_size) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    // Replace embedded NULs with newlines
    {
        size_t i;
        for (i = 0; i < data_size; ++i) {
            if (data[i] == '\0') data[i] = '\n';
        }
    }
    data[data_size] = '\n';  // sentinel
    data_size += 1;

    // Parse line by line
    cap = 32;
    out->entries = (AdmEntry *)malloc(cap * sizeof(AdmEntry));
    if (!out->entries) { free(data); return -1; }
    out->count = 0;

    line_start = 0;
    for (pos = 0; pos < data_size; ++pos) {
        if (data[pos] == '\n' || data[pos] == '\r') {
            const char *line = data + line_start;
            size_t line_len = pos - line_start;
            const char *lend = line + line_len;
            const char *trimmed_start, *trimmed_end;
            const char *anim_ptr;
            const char *q1, *q2;

            // Trim
            trimmed_start = line;
            trimmed_end = lend;
            while (trimmed_start < trimmed_end &&
                   isspace((unsigned char)*trimmed_start))
                ++trimmed_start;
            while (trimmed_end > trimmed_start &&
                   isspace((unsigned char)*(trimmed_end - 1)))
                --trimmed_end;

            // Skip empty
            if (trimmed_start >= trimmed_end) {
                // Skip \r\n together
                if (pos + 1 < data_size && data[pos] == '\r' &&
                    data[pos + 1] == '\n')
                    ++pos;
                line_start = pos + 1;
                continue;
            }

            // Skip comment lines
            if (trimmed_end - trimmed_start >= 2 &&
                trimmed_start[0] == '/' && trimmed_start[1] == '/') {
                if (pos + 1 < data_size && data[pos] == '\r' &&
                    data[pos + 1] == '\n')
                    ++pos;
                line_start = pos + 1;
                continue;
            }

            // Must contain "anim_"
            anim_ptr = NULL;
            {
                const char *s;
                size_t tlen = (size_t)(trimmed_end - trimmed_start);
                if (tlen >= 5) {
                    for (s = trimmed_start; s + 5 <= trimmed_end; ++s) {
                        if (memcmp(s, "anim_", 5) == 0) {
                            anim_ptr = s;
                            break;
                        }
                    }
                }
            }
            if (!anim_ptr) {
                if (pos + 1 < data_size && data[pos] == '\r' &&
                    data[pos + 1] == '\n')
                    ++pos;
                line_start = pos + 1;
                continue;
            }

            // Find quotes
            q1 = (const char *)memchr(trimmed_start, '"',
                        (size_t)(trimmed_end - trimmed_start));
            if (!q1) {
                free(data); adm_free(out); return -1;
            }
            q2 = (const char *)memchr(q1 + 1, '"', (size_t)(trimmed_end - (q1 + 1)));
            if (!q2) {
                free(data); adm_free(out); return -1;
            }

            // Grow array if needed
            if (out->count >= cap) {
                size_t new_cap = cap * 2;
                AdmEntry *new_entries = (AdmEntry *)realloc(
                    out->entries, new_cap * sizeof(AdmEntry));
                if (!new_entries) { free(data); adm_free(out); return -1; }
                out->entries = new_entries;
                cap = new_cap;
            }

            // Key = everything before first quote, trimmed
            copy_trimmed(out->entries[out->count].key,
                         sizeof(out->entries[out->count].key),
                         trimmed_start, q1);

            // Value = between quotes, trimmed
            copy_trimmed(out->entries[out->count].value,
                         sizeof(out->entries[out->count].value),
                         q1 + 1, q2);

            // Skip entries with empty key
            if (out->entries[out->count].key[0] != '\0') {
                out->count++;
            }

            // Skip \r\n together
            if (pos + 1 < data_size && data[pos] == '\r' &&
                data[pos + 1] == '\n')
                ++pos;
            line_start = pos + 1;
            continue;
        }
    }

    free(data);

    // Shrink to fit
    if (out->count > 0 && out->count < cap) {
        AdmEntry *shrunk = (AdmEntry *)realloc(
            out->entries, out->count * sizeof(AdmEntry));
        if (shrunk) out->entries = shrunk;
    } else if (out->count == 0) {
        free(out->entries);
        out->entries = NULL;
    }

    return 0;
}

void adm_free(AdmFile *af) {
    if (!af) return;
    free(af->entries);
    af->entries = NULL;
    af->count = 0;
}
