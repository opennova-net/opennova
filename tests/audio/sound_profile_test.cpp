// opennova::audio::SoundProfileTable — the SndProf.def parser + lookup semantics
// [orig: SoundProfile_LoadAll @ 0x527490, sound_profile_xml_callback @ 0x526fc0,
// SoundProfile_FindSlotByName @ 0x526e30]. The inline fixture mirrors the retail
// file's shapes (quoted begin names + trailing comment text, tab/space runs,
// float param columns, the med/crs percent rows). The retail-corpus spot check
// (<OPENNOVA_JO_ASSETS>/sndprof.def, 49 profiles) runs only when the extracted
// asset tree carries it — SKIP-LEG otherwise (docs/asset-gated-tests.md).
#include <runtime/audio/sound_profile.h>
#include <base/resource_index/resource_index.h>
#include "common/test_expect.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/retail_paths.h"

using opennova::audio::SoundProfile;
using opennova::audio::SoundProfileTable;
namespace slot = opennova::audio;

static const char kFixture[] =
    "\r\n"
    "begin \"default\"\t\t default\r\n"
    "\tmedloopfadeinstart   \t20 \t\r\n"
    "\tmedloopfadeinend    \t30 \t\r\n"
    "\tcrslooppitchendp     \t100\r\n"
    "end \r\n"
    "\r\n"
    "begin \"SP_Test1\"\t\t A soldier profile, trailing comment ignored\r\n"
    "     sounddeath     BM1_DEATH\r\n"
    "     SSNightDead    BM1_DEATH_K\r\n"
    "     SSFallDead     FALLDEAD2\r\n"
    "     SSFallAlive    JUMPLAND_DIRT\r\n"
    "     SSLFootGND     FSP_DIRT_L\r\n"
    "     SSRFootGND     FSP_DIRT_R\r\n"
    "     SSFootWater    FS_WATER\r\n"
    "     ssaudio1\t\tFSP_PRONE\r\n"
    "     soundloop_2\t\tV_APACHE_ILP\t\t.8 1.2 \r\n"
    "     rotor_impact\tIMP_ROTOR_FLESH .1\r\n"
    "     notakeyword   IGNORED\r\n"
    "end \r\n"
    "orphan_line_outside_begin  ALSO_IGNORED\r\n";

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
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

    // The shared walk and the game's atof [orig: SoundProfile_LoadAll @0x527490
    // -> File_ParseASCIIFile @0x5274DD; SoundProfile_ParseLineCallback]: a comma
    // separates columns and `;` ends the line; an exponent may be marked d and a
    // hex spelling reads 0; a begin name keeps its first 64 characters (the
    // terminator at [64], @0x527045).
    {
        static const char kWalk[] =
            "begin AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAXYZ\r\n"
            "soundloop_1,V_A,.5,1.5,3\r\n"
            "rotor_impact IMP ;.5\r\n"
            "SSFallDead X 0x10\r\n"
            "medloopfadeinstart 2d1\r\n"
            "end\r\n";
        SoundProfileTable t;
        TEST_EXPECT(t.parse(kWalk, sizeof(kWalk) - 1) == 1);
        const SoundProfile &p = t.entries()[0];
        TEST_EXPECT(p.name.size() == 64);
        TEST_EXPECT(p.set_names[0] == "V_A");
        TEST_EXPECT(p.param2_q16[0] == 0x8000 && p.param3_q16[0] == 0x18000 && p.param4[0] == 3);
        TEST_EXPECT(p.set_names[slot::kSlotRotorImpact] == "IMP");
        TEST_EXPECT(p.param2_q16[slot::kSlotRotorImpact] == 0);
        TEST_EXPECT(p.set_names[slot::kSlotFallDead] == "X");
        TEST_EXPECT(p.param2_q16[slot::kSlotFallDead] == 0);
        TEST_EXPECT(p.loop_params[0] == 20 * 655);
    }

    // A short line reads the slots a longer one left: tokens 0..2 reset each line,
    // 3..29 keep their pointers into the reused line buffer [orig:
    // Terrain_TokenizeConfigLine @0x53CB71..0x53CB81], and the callback reads
    // tokens[4] and tokens[5] with no count check (@0x52714b, @0x52718b). So a
    // bare `soundloop_3 V_B` after `soundloop_2 V_A .8 1.2 3` keeps param2 0
    // (token 2, reset) but takes 1.2 and 3, which outlive the shorter line.
    {
        static const char kStale[] =
            "begin Stale\r\n"
            "soundloop_2 V_A .8 1.2 3\r\n"
            "soundloop_3 V_B\r\n"
            "end\r\n";
        SoundProfileTable t;
        TEST_EXPECT(t.parse(kStale, sizeof(kStale) - 1) == 1);
        const SoundProfile &p = t.entries()[0];
        TEST_EXPECT(p.set_names[2] == "V_B");
        TEST_EXPECT(p.param2_q16[2] == 0);
        TEST_EXPECT(p.param3_q16[2] == static_cast<int32_t>(1.2 * 65536.0));
        TEST_EXPECT(p.param4[2] == 3);
    }

    // Empty table: null / -1.
    {
        SoundProfileTable t;
        TEST_EXPECT(t.find("anything") == nullptr);
        TEST_EXPECT(t.index_of("anything") == -1);
    }

    // `begin` cuts a name of 64 characters or more at 64 in the line buffer
    // itself, so a later short line's stale slot 4 over those bytes stops at
    // the cut: "4242" rather than the name's "4242424242", which atol would
    // saturate [orig: SoundProfile_ParseLineCallback, the strlen >= 0x40 test
    // @0x52703F and `mov [edi+40h], dl` @0x527045; the column reads
    // @0x5270f0..0x52718b].
    {
        const std::string text =
                "begin P1\r\n"
                "sounddeath " + std::string(50, 'A') + " 1 1 99999999\r\n"
                "end\r\n"
                "begin " + std::string(60, 'B') + "4242424242\r\n"
                "sounddeath Z\r\n"
                "end\r\n";
        SoundProfileTable t;
        TEST_EXPECT(t.parse(text.data(), text.size()) == 2);
        const SoundProfile &p2 = t.entries()[1];
        TEST_EXPECT(p2.name == std::string(60, 'B') + "4242");
        TEST_EXPECT(p2.param4[slot::kSlotDeath] == 4242);
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
    const std::string path = retail::asset_file("sndprof.def");
    if (!path.empty()) {
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
            std::fprintf(stderr, "FAIL: cannot open %s\n", path.c_str());
            return 1;
        }
    } else {
        retail::skip_leg("OPENNOVA_JO_ASSETS/sndprof.def (retail sound profile spot check)");
    }

    // The install's base SndProf.def through its mount: a short slot line
    // reads the slots an earlier, longer line left, so the Chinook's bare
    // soundloop_3 takes a param3 of 1.2 (78643) and the Mil26's soundloop_2
    // the gear count 2 from slot 4 [orig: SoundProfile_ParseLineCallback
    // @0x526fc0, the column reads @0x5270f0..0x52718b; Terrain_TokenizeConfigLine
    // @0x53CB71..0x53CB81].
    const std::string install = retail::install();
    if (!install.empty()) {
        opennova::ResourceIndex index;
        std::vector<uint8_t> bytes;
        TEST_EXPECT(index.scan(install) && index.read_file("sndprof.def", bytes));
        SoundProfileTable t;
        TEST_EXPECT(t.parse(reinterpret_cast<const char *>(bytes.data()), bytes.size()) > 0);
        const SoundProfile *chinook = t.find("SP_Chinook");
        const SoundProfile *mil26 = t.find("SP_Mil26");
        TEST_EXPECT(chinook != nullptr && chinook->name == "SP_Chinook");
        TEST_EXPECT(mil26 != nullptr && mil26->name == "SP_Mil26");
        TEST_EXPECT(chinook->param3_q16[slot::kSlotSoundLoop1 + 2] == 78643);
        TEST_EXPECT(chinook->param2_q16[slot::kSlotSoundLoop1 + 2] == 0);
        TEST_EXPECT(mil26->param4[slot::kSlotSoundLoop1 + 1] == 2);
    } else {
        retail::skip_leg("OPENNOVA_JO_DIR (the mounted SndProf.def's stale-slot rows)");
    }

    return 0;
}
