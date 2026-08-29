#include <formats/bad/bad.h>

#include <cstdio>
#include <cstring>

#include <string>

#include "common/retail_paths.h"
#include "common/test_expect.h"

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
    return 0;
}
