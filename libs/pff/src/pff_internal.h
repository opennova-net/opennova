#ifndef PFF_INTERNAL_H
#define PFF_INTERNAL_H

/* libs/pff-internal helpers shared between the reader (pff.cpp) and writer (pff_writer.cpp).
   Not part of the public API (pff/pff.h). Both translation units compile as C++, so this needs
   no extern "C". */

#include <stddef.h>

/* Normalize a PFF name into an uppercase, trailing-space-trimmed C string (the engine's
   strupr + 0x20-trim used for sort/lookup; PFF_SortEntries @ 0x768280 / PFF_FindEntry
   @ 0x7685d0). Reads up to raw_cap bytes or until a NUL; result capped to out_sz - 1 chars.
   The writer uses this for its directory sort + duplicate-name detection so on-disk ordering
   agrees with what the reader expects. */
void pff_norm_name(const char *raw, size_t raw_cap, char *out, size_t out_sz);

#endif /* PFF_INTERNAL_H */
