/* Open the MUS programs cleanly: the minted synth_gamemus.bin + synth_menumus.bin
   (tests/fixtures/synth_mus_gen.cpp) unconditionally; the shipped gamemus +
   menumus (the plaintext SCR0 form extracted from JO_CLIENT/localres.pff) from
   the reference fixture set behind OPENNOVA_JO_ASSETS. */

#include <stdio.h>
#include <string.h>
#include <formats/mus/mus.h>

#include <string>

#include "common/retail_paths.h"

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

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    int fail = 0;
    if (!check(MUS_FIXTURE_DIR "/synth_gamemus.bin", "gamescript")) ++fail;
    if (!check(MUS_FIXTURE_DIR "/synth_menumus.bin", "menuscript")) ++fail;

    const std::string game = retail::reference_fixture("mus/jo_gamemus.bin");
    const std::string menu = retail::reference_fixture("mus/jo_menumus.bin");
    if (game.empty() || menu.empty()) {
        retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/mus/jo_{gamemus,menumus}.bin (the shipped programs)");
    } else {
        if (!check(game.c_str(), "gamescript")) ++fail;
        if (!check(menu.c_str(), "menuscript")) ++fail;
    }
    return fail == 0 ? 0 : 1;
}
