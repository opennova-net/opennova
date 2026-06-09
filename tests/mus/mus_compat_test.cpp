/* Parse the committed MUS fixtures (gamemus + menumus). Both are the plaintext
   SCR0 form extracted from JO_CLIENT/localres.pff, so the parser must open them
   cleanly. No external/Desktop assets: everything is a committed fixture. */

#include <stdio.h>
#include <string.h>
#include "mus/mus.h"

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

static int check(const char *path) {
    MusFile mf;
    int rc = mus_open(&mf, path);
    if (rc != 0) {
        fprintf(stderr, "  FAIL %s rc=%d\n", path, rc);
        return 0;
    }
    if (mf.header.magic != MUS_MAGIC_SCR0) {
        fprintf(stderr, "  FAIL %s bad magic 0x%08x\n", path, mf.header.magic);
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
    if (!check(MUS_FIXTURE_DIR "/jo_gamemus.bin")) ++fail;
    if (!check(MUS_FIXTURE_DIR "/jo_menumus.bin")) ++fail;
    return fail == 0 ? 0 : 1;
}
