// opennova::audio::SoundProfileTable — the SndProf.def parser + lookup semantics
// [orig: SoundProfile_LoadAll @ 0x527490, sound_profile_xml_callback @ 0x526fc0,
// SoundProfile_FindSlotByName @ 0x526e30]. The inline fixture mirrors the retail
// file's shapes (quoted begin names + trailing comment text, tab/space runs,
// float param columns, the med/crs percent rows). The retail-corpus spot check
// (JO_ASSETS' sndprof.def, 49 profiles) runs only when the env var points at a
// local install — SKIP-AS-PASS otherwise (docs/asset-gated-tests.md).
#include "audio/sound_profile.h"
#include "common/test_expect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using opennova::audio::SoundProfile;
using opennova::audio::SoundProfileTable;
namespace slot = opennova::audio;

static const char kFixture[] =
    "\n"
    "begin \"default\"\t\t default\n"
    "\tmedloopfadeinstart   \t20 \t\n"
    "\tmedloopfadeinend    \t30 \t\n"
    "\tcrslooppitchendp     \t100\n"
    "end \n"
    "\n"
    "begin \"SP_Test1\"\t\t A soldier profile, trailing comment ignored\n"
    "     sounddeath     BM1_DEATH\n"
    "     SSNightDead    BM1_DEATH_K\n"
    "     SSFallDead     FALLDEAD2\n"
    "     SSFallAlive    JUMPLAND_DIRT\n"
    "     SSLFootGND     FSP_DIRT_L\n"
    "     SSRFootGND     FSP_DIRT_R\n"
    "     SSFootWater    FS_WATER\n"
    "     ssaudio1\t\tFSP_PRONE\n"
    "     soundloop_2\t\tV_APACHE_ILP\t\t.8 1.2 \n"
    "     rotor_impact\tIMP_ROTOR_FLESH .1\n"
    "     notakeyword   IGNORED\n"
    "end \n"
    "orphan_line_outside_begin  ALSO_IGNORED\n";

int main() {
    // Inline fixture: shapes and stores.
    {
        SoundProfileTable t;
        TEST_EXPECT(t.parse(kFixture, sizeof(kFixture) - 1) == 2);
        TEST_EXPECT(t.entries().size() == 2);

        const SoundProfile &def = t.entries()[0];
        TEST_EXPECT(def.name == "default");
        // Percent rows store x655 [orig: 655 * ftol @ 0x5270dd].
        TEST_EXPECT(def.loop_params[0] == 20 * 655);
        TEST_EXPECT(def.loop_params[1] == 30 * 655);
        TEST_EXPECT(def.loop_params[11] == 100 * 655);

        const SoundProfile &p = t.entries()[1];
        TEST_EXPECT(p.name == "SP_Test1");
        TEST_EXPECT(p.set_names[slot::kSlotDeath] == "BM1_DEATH");
        TEST_EXPECT(p.set_names[slot::kSlotNightDeath] == "BM1_DEATH_K");
        TEST_EXPECT(p.set_names[slot::kSlotFallDead] == "FALLDEAD2");
        TEST_EXPECT(p.set_names[slot::kSlotFallAlive] == "JUMPLAND_DIRT");
        TEST_EXPECT(p.set_names[slot::kSlotFootLGround] == "FSP_DIRT_L");
        TEST_EXPECT(p.set_names[slot::kSlotFootRGround] == "FSP_DIRT_R");
        TEST_EXPECT(p.set_names[slot::kSlotFootWater] == "FS_WATER");
        // Keyword match is case-insensitive ("ssaudio1" vs the table's SSAudio1).
        TEST_EXPECT(p.set_names[slot::kSlotAudio1] == "FSP_PRONE");
        // Unauthored slots stay empty (the resolved-id-0 no-op).
        TEST_EXPECT(p.set_names[slot::kSlotFootLSnow].empty());
        TEST_EXPECT(p.set_names[slot::kSlotChuteOpen].empty());
        // Float param columns store x65536 [orig: fmul dbl_7C3CC0 @ 0x527127].
        TEST_EXPECT(p.set_names[1] == "V_APACHE_ILP"); // Soundloop_2 = slot 1
        TEST_EXPECT(p.param2_q16[1] == static_cast<int32_t>(0.8 * 65536.0));
        TEST_EXPECT(p.param3_q16[1] == static_cast<int32_t>(1.2 * 65536.0));
        TEST_EXPECT(p.param2_q16[slot::kSlotRotorImpact] ==
                    static_cast<int32_t>(0.1 * 65536.0));

        // Lookup: case-insensitive first match; a miss falls back to the FIRST
        // profile [orig: @ 0x526e30 returns the array base on miss].
        TEST_EXPECT(t.find("sp_test1") == &p);
        TEST_EXPECT(t.find("no_such_profile") == &def);
        TEST_EXPECT(t.index_of("SP_TEST1") == 1);
        TEST_EXPECT(t.index_of("no_such_profile") == 0);
        TEST_EXPECT(t.find("default") == &def);
    }

    // Empty table: null / -1.
    {
        SoundProfileTable t;
        TEST_EXPECT(t.find("anything") == nullptr);
        TEST_EXPECT(t.index_of("anything") == -1);
    }

    // The slot keyword table is the engine's, index == slot.
    {
        TEST_EXPECT(std::strcmp(slot::sound_profile_slot_keyword(17), "SSLFootGND") == 0);
        TEST_EXPECT(std::strcmp(slot::sound_profile_slot_keyword(23), "SSFootWater") == 0);
        TEST_EXPECT(std::strcmp(slot::sound_profile_slot_keyword(44), "FreeFall") == 0);
        TEST_EXPECT(std::strcmp(slot::sound_profile_slot_keyword(50), "tumble_skid") == 0);
        TEST_EXPECT(slot::sound_profile_slot_keyword(51) == nullptr);
    }

    // Retail corpus (asset-gated): JO's sndprof.def parses to 49 profiles and the
    // SP player profile carries the witnessed footstep sets.
    if (const char *assets = std::getenv("JO_ASSETS")) {
        std::string path = std::string(assets) + "/sndprof.def";
        if (FILE *f = std::fopen(path.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<char> buf(static_cast<size_t>(n));
            const bool read_ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
            std::fclose(f);
            TEST_EXPECT(read_ok);
            SoundProfileTable t;
            TEST_EXPECT(t.parse(buf.data(), buf.size()) == 49);
            const SoundProfile *player = t.find("SP_JO_SP_PlayerM1");
            TEST_EXPECT(player != nullptr && player->name == "SP_JO_SP_PlayerM1");
            TEST_EXPECT(player->set_names[slot::kSlotFootLGround] == "FSP_DIRT_L");
            TEST_EXPECT(player->set_names[slot::kSlotFootWater] == "FS_WATER");
            TEST_EXPECT(player->set_names[slot::kSlotNightDeath] == "BM1_DEATH_K");
            TEST_EXPECT(player->set_names[slot::kSlotFreeFall] == "FREEFALL");
        } else {
            std::printf("SKIP: %s not present\n", path.c_str());
        }
    } else {
        std::printf("SKIP: JO_ASSETS not set (retail sndprof.def spot check)\n");
    }

    return 0;
}
