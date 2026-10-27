/* SBF compat: parse + decode the first chunk of every committed SBF fixture.
   Fixtures only (both shipped bank forms are swept by sbf_bank_sweep_test);
   a missing/bad fixture is a failure. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <formats/sbf/sbf.h>

#include "sbf/sbf_bank_file.h"

using namespace opennova::sbf;

static int try_open(const char *path) {
    SbfBankFile bank;
    int rc = bank.open(path);
    if (rc != 0) {
        fprintf(stderr, "  FAIL %s (rc=%d)\n", path, rc);
        return 0;
    }
    const SbfArchive &arc = bank.arc;
    if (arc.header.magic != SBF_MAGIC) return 0;
    if (arc.header.entry_count == 0)   return 0;

    const uint8_t *chunk = bank.chunk(&arc.entries[0], 0);
    if (chunk == NULL) return 0;
    int16_t out[SBF_CHUNK_AUDIO];
    int n = sbf_decode_chunk(chunk, arc.entries[0].block_size, out, SBF_CHUNK_AUDIO);
    if (n < 0) return 0;

    printf("  OK   %s (entries=%u, first=%.16s, first_chunk_samples=%d)\n",
           path, arc.header.entry_count, arc.entries[0].name, n);
    return 1;
}

int main(void) {
    int fail = 0;
    /* Committed fixtures only: the synthetic bank. */
    if (!try_open("fixtures/sbf/synth_gamemus.sbf")) ++fail;
    return fail == 0 ? 0 : 1;
}
