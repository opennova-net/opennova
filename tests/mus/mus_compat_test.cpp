/* Open the MUS programs cleanly: the minted synth_gamemus.bin, synth_menumus.bin and synth_forms.bin
   (tests/fixtures/synth_mus_gen.cpp: the shipped gamemus and menumus shapes, and the shipped menu
   programs' form, each in MDEdit's layout); their parse is pinned by mus_parse. */
#include <stdio.h>
#include <string.h>
#include <formats/mus/mus.h>


using namespace opennova::mus;

#ifndef MUS_FIXTURE_DIR
#define MUS_FIXTURE_DIR "fixtures/mus"
#endif

static int check(const char *path, const char *script_name) {
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
    if (strncmp(mf.scripts[0].name, script_name, MUS_NAME_SIZE) != 0) {
        fprintf(stderr, "  FAIL %s first script %.16s, expected %s\n", path, mf.scripts[0].name, script_name);
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
    if (!check(MUS_FIXTURE_DIR "/synth_gamemus.bin", "gamescript")) ++fail;
    if (!check(MUS_FIXTURE_DIR "/synth_menumus.bin", "menuscript")) ++fail;
    if (!check(MUS_FIXTURE_DIR "/synth_forms.bin", "menuscript")) ++fail;
    return fail == 0 ? 0 : 1;
}
