// ADM animation definition file parser — pure C API.
// Parses key/value pairs from .adm text files.

#ifndef ADM_H
#define ADM_H

#include <stddef.h>

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define ADM_EXPORT __declspec(dllexport)
#  else
#    define ADM_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define ADM_EXPORT __attribute__((visibility("default")))
#  else
#    define ADM_EXPORT
#  endif
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

// Free all allocations inside an AdmFile.
ADM_EXPORT void adm_free(AdmFile *af);

#endif // ADM_H
