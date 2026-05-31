/* Iterate every MUS .bin we know about. Hard-fails on the committed JO fixture;
   skips Desktop / external paths if absent or encrypted (the live game .bin
   files are SCR-XOR-encoded and our parser only consumes decrypted SCR0 data). */

#include <stdio.h>
#include <string.h>
#include "mus/mus.h"

static int try_open(const char *path, int hard_fail) {
    MusFile mf;
    int rc = mus_open(&mf, path);
    if (rc != 0) {
        if (hard_fail) {
            fprintf(stderr, "  HARD-FAIL %s rc=%d\n", path, rc);
            return 0;
        }
        if (rc == -30) {
            printf("  SKIP %s (file absent)\n", path);
        } else {
            printf("  SKIP %s (rc=%d; likely encrypted or non-MUS)\n", path, rc);
        }
        return 1;
    }
    if (mf.header.magic != MUS_MAGIC_SCR0) {
        mus_close(&mf);
        return 0;
    }
    printf("  OK   %s (%u scripts, first=%.16s, code=%u B, %u sections, %u intrinsics)\n",
           path, mf.header.chunk_count,
           mf.scripts[0].name, mf.scripts[0].code_size,
           mf.scripts[0].section_count, mf.intrinsic_count);
    mus_close(&mf);
    return 1;
}

int main(void) {
    int fail = 0;
    if (!try_open("fixtures/mus/jo_gamemus.bin", 1)) ++fail;

    /* Desktop probes: skip-if-absent, skip-if-encrypted. */
    try_open("C:/Users/taylor/Desktop/JO_ASSETS_t/gamemus.bin", 0);
    try_open("C:/Users/taylor/Desktop/JO_ASSETS_t/menumus.bin", 0);
    try_open("C:/Users/taylor/Desktop/AS_ASSETS/gamemus.bin",   0);
    try_open("C:/Users/taylor/Desktop/AS_ASSETS/menumus.bin",   0);
    try_open("C:/Users/taylor/Desktop/BHD_STock2/gamemus.bin",  0);
    try_open("C:/Users/taylor/Desktop/BHD_STock2/menumus.bin",  0);
    try_open("C:/Users/taylor/Desktop/DFX_Stock2/gamemus.bin",  0);
    try_open("C:/Users/taylor/Desktop/DFX_Stock2/menumus.bin",  0);

    return fail == 0 ? 0 : 1;
}
