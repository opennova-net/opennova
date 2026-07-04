// ADM animation definition file parser — pure C API.
// Parses key/value pairs from .adm text files.

#ifndef ADM_H
#define ADM_H

#include <stddef.h>

#include <io/export.h>
#define ADM_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AdmEntry {
    char key[64];
    char value[256];
} AdmEntry;

typedef struct AdmFile {
    AdmEntry *entries;
    size_t count;
} AdmFile;

// Parse a .adm file. Returns 0 on success, -1 on error.
// On success, caller must eventually call adm_free().
ADM_EXPORT int adm_parse(const char *path, AdmFile *out);

// Parse a .adm from an in-memory buffer (does not take ownership of `bytes`).
// Returns 0 on success, -1 on error. On success, caller must call adm_free().
// Used by hosts that read assets from a VFS (PFF archive) rather than disk.
ADM_EXPORT int adm_parse_buffer(const char *bytes, size_t size, AdmFile *out);

// Free all allocations inside an AdmFile.
ADM_EXPORT void adm_free(AdmFile *af);

// Write entries to a .adm file. Each entry is emitted as
//   <key>\t\t\t\t"<value>"
// one per line (CRLF), preceded by a blank line and followed by a trailing
// CRLF*3 + NUL, matching stock NovaLogic .adm files. Callers order entries
// (reset first, key "anim_reset"). Returns 0 on success, -1 on error.
ADM_EXPORT int adm_write(const char *path, const AdmEntry *entries, size_t count);

#ifdef __cplusplus
}
#endif

#endif // ADM_H
