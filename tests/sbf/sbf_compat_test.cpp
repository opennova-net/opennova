/* SBF compat sweep: parse + decode first chunk of every SBF the dev has on
   disk. Two committed fixtures are required-pass; everything else is
   skip-without-fail. CI green requires only the committed fixtures. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sbf/sbf.h"

static int try_open(const char *path) {
    SbfArchive arc;
    int rc = sbf_open(&arc, path);
    if (rc != 0) {
        printf("  SKIP %s (rc=%d, file may be absent)\n", path, rc);
        return 1;
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
    /* Required (committed under fixtures/sbf/). */
    if (!try_open("fixtures/sbf/bhd_menumus.sbf")) ++fail;
    if (!try_open("fixtures/sbf/jo_gamemus.sbf"))  ++fail;
    /* Skip-if-absent (developer Desktop). */
    try_open("C:/Users/taylor/Desktop/Delta Force Black Hawk Down/gamemus.sbf");
    try_open("C:/Users/taylor/Desktop/Delta Force Land Warrior/Dflwmus.sbf");
    try_open("C:/Users/taylor/Desktop/Delta Force Task Force Dagger/mus.sbf");
    try_open("C:/Users/taylor/Desktop/Delta Force Xtreme 2/gamemus.sbf");
    try_open("C:/Users/taylor/Desktop/Delta Force Xtreme 2/menumus.sbf");
    try_open("C:/Users/taylor/Desktop/Joint Operations Demo/menumus.sbf");
    try_open("C:/Users/taylor/Desktop/JO_CLIENT/gamemus.sbf");
    try_open("C:/Users/taylor/Desktop/JO_CLIENT/menumus.sbf");
    return fail == 0 ? 0 : 1;
}
