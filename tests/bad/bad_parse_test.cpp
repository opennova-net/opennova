#include <formats/bad/bad.h>

#include <cstdio>
#include <cstring>

#include <string>

#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::bad;

// The null guard, then the layout pins on the minted clips
// (tests/fixtures/minimal_bad_gen.cpp): fixtures/anim/rig19_idle.bad carries
// the stock 19-bone idle's shape (the shipped BINOC.bad's: the pins below
// held on it), fixtures/anim/vm40_rst.bad the translated 40-bone reset's
// (357_RST.bad's).
int main() {
    BadFile invalid = {};
    TEST_EXPECT(bad_parse(nullptr, &invalid) == -1);

    const std::string fixture = test_paths_fixture("anim/rig19_idle.bad");
    BadFile file = {};
    TEST_EXPECT(bad_parse(fixture.c_str(), &file) == 0);

    TEST_EXPECT(file.version == 1);
    TEST_EXPECT(file.header_size == 80);
    TEST_EXPECT(file.fps == 30);
    TEST_EXPECT(file.frame_count == 3);
    TEST_EXPECT(file.flags == 1);
    TEST_EXPECT(file.bone_count == 19);

    TEST_EXPECT(file.num_bones == 19);
    TEST_EXPECT(file.bones != nullptr);
    TEST_EXPECT(std::strcmp(file.bones[0].name, "BN01 Hips") == 0);
    TEST_EXPECT(file.bones[0].parent_index == -1);

    TEST_EXPECT(file.num_channels == 19);
    TEST_EXPECT(file.channels != nullptr);
    TEST_EXPECT(file.channels[0].frame_count == 4);
    TEST_EXPECT(file.channels[0].frame_lengths != nullptr);
    TEST_EXPECT(file.channels[0].rotations != nullptr);

    TEST_EXPECT(file.num_events == 4);
    TEST_EXPECT(file.events != nullptr);
    TEST_EXPECT(file.num_translations == 0);
    TEST_EXPECT(file.translations == nullptr);

    bad_free(&file);
    TEST_EXPECT(file.bones == nullptr);
    TEST_EXPECT(file.channels == nullptr);
    TEST_EXPECT(file.events == nullptr);

    std::printf("rig19_idle.bad parsed with the stock idle's pins\n");

    // A translated clip carries a row per frame and the fence-post row
    // frame_count, which the runtime's lerp reaches on the last interval
    // [orig: AnimChannel_GetTranslationRowAtTime @0x4102d0 via BoneAnim_TransformBones @0x410360].
    // vm40_rst.bad: 40 bones, one frame, rows 0 and 1 (and the pad row past them).
    const std::string translated = test_paths_fixture("anim/vm40_rst.bad");
    BadFile rst = {};
    TEST_EXPECT(bad_parse(translated.c_str(), &rst) == 0);
    TEST_EXPECT(rst.frame_count == 1 && rst.bone_count == 40 && (rst.flags & 2u) != 0);
    TEST_EXPECT(rst.num_translations == 80);
    bad_free(&rst);
    std::printf("vm40_rst.bad carries rows 0..frame_count\n");
    return 0;
}
