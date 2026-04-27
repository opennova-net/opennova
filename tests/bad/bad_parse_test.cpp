#include "bad/bad.h"

#include <cstdio>
#include <cstring>

#include "common/test_expect.h"
#include "common/test_paths.h"

int main() {
    char path[4096];
    std::snprintf(path, sizeof(path), "%s%cfixtures%cbad%cBINOC.bad",
                  test_paths_repo_root(__FILE__), TEST_PATHS_SEP,
                  TEST_PATHS_SEP, TEST_PATHS_SEP);

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

    BadFile invalid = {};
    TEST_EXPECT(bad_parse(nullptr, &invalid) == -1);

    return 0;
}
