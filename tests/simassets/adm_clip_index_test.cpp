/* engine/runtime/simassets AdmClipIndex — the native clip-length source the
   weapon FSM bake rings from (S6b). Pinned over the committed fixtures:
   fixtures/anim (resolvable .bads) and fixtures/adm/mp5_1st.adm (a real rig
   map whose .bads are absent — the continue-on-failure edge). */

#include <cstdio>
#include <string>

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <resource_index/resource_index.h>
#include <simassets/adm_clip_index.h>

int main() {
    using opennova::simassets::AdmClipIndex;

    const char *root = test_paths_repo_root(__FILE__);

    {
        opennova::ResourceIndex index;
        TEST_EXPECT(index.scan(std::string(root) + "/fixtures/anim"));
        AdmClipIndex clips;
        // ".adm" appends when missing; keys land lowercased.
        const int keys = clips.load(&index, "soldier");
        std::printf("[clipindex] soldier keys=%d\n", keys);
        TEST_EXPECT(keys == 7);
        TEST_EXPECT(clips.loaded());
        TEST_EXPECT(clips.adm_name() == "soldier.adm");

        const std::vector<float> *idle = clips.lengths_for("ANIM_IDLE");
        TEST_EXPECT(idle != nullptr && idle->size() == 1);
        TEST_EXPECT((*idle)[0] > 0.0f);
        const std::vector<float> *walk = clips.lengths_for("anim_walk_forward");
        TEST_EXPECT(walk != nullptr && walk->size() == 1);
        std::printf("[clipindex] idle=%f walk=%f\n", (*idle)[0], (*walk)[0]);
        // Distinct .bads produce their own lengths; shared .bads agree.
        const std::vector<float> *jog = clips.lengths_for("anim_jog_forward");
        TEST_EXPECT(jog != nullptr && (*jog)[0] == (*walk)[0]);
        TEST_EXPECT(clips.lengths_for("anim_wpn_fire") == nullptr);

        // Reload replaces wholesale.
        TEST_EXPECT(clips.load(&index, "US01.adm") > 0);
        TEST_EXPECT(clips.adm_name() == "US01.adm");
        TEST_EXPECT(clips.lengths_for("anim_walk_forward") == nullptr);
    }

    {
        // A real weapon rig map whose .bads are absent: every variant skips
        // (continue-on-failure), the load degrades to zero keys, and the FSM's
        // 'auto' delays collapse exactly as the model-never-loads path did.
        opennova::ResourceIndex index;
        TEST_EXPECT(index.scan(std::string(root) + "/fixtures/adm"));
        AdmClipIndex clips;
        TEST_EXPECT(clips.load(&index, "mp5_1st.adm") == 0);
        TEST_EXPECT(!clips.loaded());
        TEST_EXPECT(clips.lengths_for("anim_wpn_fire") == nullptr);
    }

    {
        // No index: nothing loads, nothing crashes.
        AdmClipIndex clips;
        TEST_EXPECT(clips.load(nullptr, "soldier.adm") == 0);
    }

    std::printf("simassets adm clip index: OK\n");
    return 0;
}
