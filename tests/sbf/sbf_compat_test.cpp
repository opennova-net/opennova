/* SBF compat: parse + decode the first chunk of each committed SBF fixture.
   Fixtures only (no external assets); a missing/bad fixture is a failure. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sbf/sbf.h"

static int try_open(const char *path) {
    SbfArchive arc;
    int rc = sbf_open(&arc, path);
    if (rc != 0) {
        fprintf(stderr, "  FAIL %s (rc=%d)\n", path, rc);
        return 0;
    }
    if (arc.header.magic != SBF_MAGIC) { sbf_close(&arc); return 0; }
    if (arc.header.entry_count == 0)   { sbf_close(&arc); return 0; }

    uint8_t chunk[0x1008];
    int got = sbf_read_chunk(&arc, &arc.entries[0], 0, chunk, sizeof(chunk));
    if (got <= 0) { sbf_close(&arc); return 0; }
    int16_t out[SBF_CHUNK_AUDIO];
    int n = sbf_decode_chunk(chunk, got, out, SBF_CHUNK_AUDIO);
    if (n < 0) { sbf_close(&arc); return 0; }

    printf("  OK   %s (entries=%u, first=%.16s, first_chunk_samples=%d)\n",
           path, arc.header.entry_count, arc.entries[0].name, n);
    sbf_close(&arc);
    return 1;
}

int main(void) {
    int fail = 0;
    /* Committed fixtures only (a JO gamemus bank + a BHD menumus bank). */
    if (!try_open("fixtures/sbf/bhd_menumus.sbf")) ++fail;
    if (!try_open("fixtures/sbf/jo_gamemus.sbf"))  ++fail;
    return fail == 0 ? 0 : 1;
}
