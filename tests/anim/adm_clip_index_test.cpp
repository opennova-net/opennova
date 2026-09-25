/* engine/runtime/anim AdmClipIndex — the native clip-length source the
   weapon FSM bake rings from (S6b). Pinned over the authored fixtures/anim
   (resolvable .bads) and a rig map staged in a temp dir whose .bads are
   absent (the continue-on-failure edge). */

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <runtime/assets/asset_store.h>
#include <runtime/anim/adm_clip_index.h>

int main() {
    using opennova::anim::AdmClipIndex;

    const char *root = test_paths_repo_root(__FILE__);

    {
        opennova::ResourceIndex index;
        opennova::assets::AssetStore index_assets{&index};
        TEST_EXPECT(index.scan(std::string(root) + "/fixtures/anim"));
        AdmClipIndex clips;
        // ".adm" appends when missing; keys land lowercased.
        const int keys = clips.load(&index_assets, "soldier");
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
        TEST_EXPECT(clips.load(&index_assets, "US01.adm") > 0);
        TEST_EXPECT(clips.adm_name() == "US01.adm");
        TEST_EXPECT(clips.lengths_for("anim_walk_forward") == nullptr);
    }

    {
        // A weapon rig map whose .bads are absent (the shipped MP5 map's shape,
        // staged alone in a temp dir): every variant skips (continue-on-failure),
        // the load degrades to zero keys, and the FSM's 'auto' delays collapse
        // exactly as the model-never-loads path did.
        const std::string dir = std::string(test_paths_temp_dir()) + "/opennova_clipindex_test";
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0777);
#endif
        {
            std::ofstream f(dir + "/mp5_1st.adm", std::ios::binary);
            TEST_EXPECT(static_cast<bool>(f));
            f << "\r\nanim_reset\t\t\t\t\"mp5_RST\"\r\n"
                 "anim_wpn_fire\t\t\t\t\"mp5_1f\"\r\n"
                 "anim_wpn_reload\t\t\t\t\"mp5_1r\"\r\n";
        }
        opennova::ResourceIndex index;
        opennova::assets::AssetStore index_assets{&index};
        TEST_EXPECT(index.scan(dir));
        AdmClipIndex clips;
        TEST_EXPECT(clips.load(&index_assets, "mp5_1st.adm") == 0);
        TEST_EXPECT(!clips.loaded());
        TEST_EXPECT(clips.lengths_for("anim_wpn_fire") == nullptr);
    }

    {
        // A row names its slot by its key past the first five characters,
        // whatever they are: `ANIM_RESET` is anim_reset and `xxxx_idle` is
        // anim_idle. [orig: AnimMap_FindSlotByName @ 0x40cfa0, stricmp on
        // key + 5]
        const std::string dir = std::string(test_paths_temp_dir()) + "/opennova_clipindex_slots";
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0777);
#endif
        opennova::ResourceIndex fixtures;
        TEST_EXPECT(fixtures.scan(std::string(root) + "/fixtures/anim"));
        std::vector<uint8_t> idle;
        TEST_EXPECT(fixtures.read_file("idle.bad", idle));
        {
            std::ofstream bad(dir + "/idle.bad", std::ios::binary);
            bad.write(reinterpret_cast<const char *>(idle.data()), static_cast<std::streamsize>(idle.size()));
            std::ofstream f(dir + "/slots.adm", std::ios::binary);
            f << "\r\nANIM_RESET\t\t\t\t\"idle\"\r\nxxxx_idle\t\t\t\t\"idle\"\r\n";
            TEST_EXPECT(static_cast<bool>(bad) && static_cast<bool>(f));
        }
        opennova::ResourceIndex index;
        opennova::assets::AssetStore index_assets{&index};
        TEST_EXPECT(index.scan(dir));
        AdmClipIndex clips;
        TEST_EXPECT(clips.load(&index_assets, "slots.adm") == 2);
        TEST_EXPECT(clips.lengths_for("anim_reset") != nullptr);
        TEST_EXPECT(clips.lengths_for("anim_idle") != nullptr);
    }

    {
        // No index: nothing loads, nothing crashes.
        AdmClipIndex clips;
        TEST_EXPECT(clips.load(nullptr, "soldier.adm") == 0);
    }

    std::printf("native anim adm clip index: OK\n");
    return 0;
}
