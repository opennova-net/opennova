#include <formats/bad/bad.h>

#include <cstdio>
#include <cstring>

#include <string>

#include "common/retail_paths.h"
#include "common/test_expect.h"

using namespace opennova::bad;

// The null guard runs unconditionally; the shipped BINOC.bad (the reference
// fixture set, OPENNOVA_JO_ASSETS) is the SKIP-LEG retail leg that pins the
// rig's layout.
int main() {
    BadFile invalid = {};
    TEST_EXPECT(bad_parse(nullptr, &invalid) == -1);

    const std::string fixture = retail::reference_fixture("bad/BINOC.bad");
    if (fixture.empty())
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/bad/BINOC.bad (the shipped 19-bone rig)");
    const char *path = fixture.c_str();

    BadFile file = {};
    TEST_EXPECT(bad_parse(path, &file) == 0);

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

    std::printf("retail leg: BINOC.bad parsed with the shipped pins\n");

    // A translated clip carries a row per frame and the fence-post row
    // frame_count, which the runtime's lerp reaches on the last interval
    // [orig: sub_4102D0 @0x4102d0 via BoneAnim_TransformBones @0x410360].
    // 357_RST.bad: 40 bones, one frame, rows 0 and 1 (and retail's pad row).
    const std::string translated = retail::asset_file("357_RST.bad");
    if (translated.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS/357_RST.bad (a translated clip)");
    BadFile rst = {};
    TEST_EXPECT(bad_parse(translated.c_str(), &rst) == 0);
    TEST_EXPECT(rst.frame_count == 1 && rst.bone_count == 40 && (rst.flags & 2u) != 0);
    TEST_EXPECT(rst.num_translations == 80);
    bad_free(&rst);
    std::printf("retail leg: 357_RST.bad carries rows 0..frame_count\n");
    return 0;
}
