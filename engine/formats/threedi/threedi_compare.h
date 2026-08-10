#ifndef THREEDI_COMPARE_H
#define THREEDI_COMPARE_H

#include <stddef.h>

#ifndef THREEDI_EXPORT
#include <io/export.h>
#define THREEDI_EXPORT OPENNOVA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Compare explicitly selected raw chunks in two 3DI3 files.
// chunk_ids_csv is required and must contain one or more four-character chunk ids.
// Returns 0 for equal, 1 for mismatch, and a negative value for invalid input or read errors.
THREEDI_EXPORT int threedi_3di3_compare_file_chunks(const char *expected_path,
                                                    const char *actual_path,
                                                    const char *chunk_ids_csv,
                                                    char *report,
                                                    size_t report_size);

#ifdef __cplusplus
}
#endif

#endif // THREEDI_COMPARE_H
