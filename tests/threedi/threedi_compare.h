#ifndef THREEDI_COMPARE_H
#define THREEDI_COMPARE_H

#include <stddef.h>

// Compare explicitly selected raw chunks in two 3DI3 files.
// chunk_ids_csv is required and must contain one or more four-character chunk ids.
// Returns 0 for equal, 1 for mismatch, and a negative value for invalid input or read errors.
int threedi_3di3_compare_file_chunks(const char *expected_path,
                                                    const char *actual_path,
                                                    const char *chunk_ids_csv,
                                                    char *report,
                                                    size_t report_size);

#endif // THREEDI_COMPARE_H
