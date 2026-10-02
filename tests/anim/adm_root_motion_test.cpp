/* engine/runtime/anim AdmRootMotion — the engine-side IRootMotionSource
   (moved from the adapter's InfantryRootMotion), pinned headless over the
   committed fixtures: soldier.adm -> idle.bad/walk.bad through a mounted
   ResourceIndex. The .bad sampling semantics themselves are grilled in
   tests/anim/root_motion_test.cpp; this pins the registry/resolution/advance
   plumbing of the ported source. */

#include <chrono>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
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
#include <runtime/anim/adm_root_motion.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/world/infantry.h>

int main() {
    using opennova::anim::AdmRootMotion;
    using opennova::world::RootMotionFrame;

    const char *root = test_paths_repo_root(__FILE__);
    opennova::ResourceIndex index;
    opennova::assets::AssetStore index_assets{&index};
    TEST_EXPECT(index.scan(std::string(root) + "/fixtures/anim"));

    AdmRootMotion source;
    TEST_EXPECT(source.empty());

    // First successful registration is the default set (id 0); re-registering
    // the same name (any case) returns the cached id.
    const int soldier = source.register_adm(&index_assets, "soldier.adm");
    TEST_EXPECT(soldier == 0);
    TEST_EXPECT(source.register_adm(&index_assets, "SOLDIER.ADM") == 0);
    TEST_EXPECT(!source.empty());
    TEST_EXPECT(source.adm_name(0) == "soldier.adm");

    const int us01 = source.register_adm(&index_assets, "US01.adm");
    TEST_EXPECT(us01 == 1);
    TEST_EXPECT(source.register_adm(&index_assets, "missing.adm") == -1);
    TEST_EXPECT(source.set_count() == 2);

    std::printf("[adm] soldier clips=%d us01 clips=%d\n",
            source.clip_count(0), source.clip_count(1));
    TEST_EXPECT(source.clip_count(0) > 0);

    // State 0 (RESET) is authored in both fixture maps; an unauthored state
    // falls back to RESET's channel (the retail AnimMap registration rule).
    TEST_EXPECT(source.has_clip(0, opennova::world::anim_state::kReset));
    const int32_t reset_len =
            source.clip_length_ticks(0, opennova::world::anim_state::kReset, 0);
    std::printf("[adm] reset clip_length_ticks=%d\n", reset_len);
    TEST_EXPECT(reset_len > 0);
    // Fallback: a state id far past the authored table still resolves (RESET).
    TEST_EXPECT(source.clip_length_ticks(0, 199, 0) == reset_len);
    // Availability is AUTHORSHIP, not resolvability [orig: animMap[id] !=
    // animMap[0]; the NULL-slot fill at AnimMap_RegisterEntity @0x40bb60].
    // soldier.adm authors anim_stop (with RESET's own .bad -- still its own
    // node per AnimMap_RegisterBoneNode @0x40c2d0) but not anim_idle_3;
    // US01.adm authors neither. The unauthored states still PLAY (RESET).
    TEST_EXPECT(source.has_clip(0, opennova::world::anim_state::kStop));
    TEST_EXPECT(!source.has_clip(0, opennova::world::anim_state::kIdle3));
    TEST_EXPECT(!source.has_clip(0, 199));
    TEST_EXPECT(!source.has_clip(1, opennova::world::anim_state::kStop));
    TEST_EXPECT(source.has_clip(1, opennova::world::anim_state::kEmplaced));
    TEST_EXPECT(!source.has_clip(7, opennova::world::anim_state::kReset));
    TEST_EXPECT(source.clip_length_ticks(0, opennova::world::anim_state::kIdle3, 0) == reset_len);
    // An unregistered set has nothing.
    TEST_EXPECT(source.clip_length_ticks(7, 0, 0) == -1);

    // advance(): the simulation-tick playhead convention — phase increments by one
    // per call, frames carry capsule extents (top gets the +0x2000 bias).
    int32_t phase = -1;
    RootMotionFrame frame{};
    TEST_EXPECT(source.advance(0, opennova::world::anim_state::kReset, phase, frame));
    TEST_EXPECT(phase == 0);
    const int32_t bottom0 = frame.capsule_bottom;
    const int32_t top0 = frame.capsule_top;
    std::printf("[adm] reset f0: dx=%d dy=%d dz=%d bottom=%d top=%d events=%u\n",
            frame.dx, frame.dy, frame.dz, frame.capsule_bottom,
            frame.capsule_top, frame.events);
    TEST_EXPECT(top0 > bottom0);

    // Determinism: a fresh playhead over the same clip reproduces frame 0.
    int32_t phase2 = -1;
    RootMotionFrame frame2{};
    TEST_EXPECT(source.advance(0, opennova::world::anim_state::kReset, phase2, frame2));
    TEST_EXPECT(frame2.dx == frame.dx && frame2.capsule_bottom == bottom0 &&
            frame2.capsule_top == top0);

    // Blended advance with weight 0 equals the primary-only sample.
    int32_t pa = -1, pb = -1;
    RootMotionFrame blended{};
    TEST_EXPECT(source.advance_blended(0,
            opennova::world::anim_state::kReset, 0, pa,
            opennova::world::anim_state::kReset, 0, pb, 0.0f, blended));
    TEST_EXPECT(blended.dx == frame.dx);
    TEST_EXPECT(blended.capsule_bottom == bottom0);
    TEST_EXPECT(blended.capsule_top == top0);

    // ---- the variant ring: every quoted token on a row is its own clip ----
    // A two-clip idle row over the two committed .bads: idle.bad (no root travel)
    // and walk.bad (forward travel). Variant 0 must sample idle, variant 1 walk,
    // variant 2 wraps back to idle, and a single-clip row reports a ring of 1.
    // [orig: AnimMap_ParseConfigLine @0x40cb60 registers every token on the slot;
    //  AnimMap_PlayAnimBySlot @0x40bda0 serves the head and advances it]
    {
        const std::string dir = std::string(test_paths_temp_dir()) + "/" + test_paths_unique("opennova_rm_ring");
#ifdef _WIN32
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0777);
#endif
        auto copy_file = [&](const char *name) {
            std::vector<uint8_t> bytes;
            if (!index.read_file(name, bytes)) return false;
            std::ofstream f(dir + "/" + name, std::ios::binary);
            f.write(reinterpret_cast<const char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(f);
        };
        TEST_EXPECT(copy_file("idle.bad"));
        TEST_EXPECT(copy_file("walk.bad"));
        {
            std::ofstream f(dir + "/ring.adm", std::ios::binary);
            f << "\r\nanim_reset\t\t\t\t\"idle\"\r\n"
                 "anim_idle\t\t\t\t\"idle\" \"walk\"\r\n"
                 "anim_walk_forward\t\t\t\"walk\"\r\n\r\n\r\n";
            TEST_EXPECT(static_cast<bool>(f));
        }
        opennova::ResourceIndex ring_index;
    opennova::assets::AssetStore ring_index_assets{&ring_index};
        TEST_EXPECT(ring_index.scan(dir));
        AdmRootMotion rings;
        const int rid = rings.register_adm(&ring_index_assets, "ring.adm");
        TEST_EXPECT(rid == 0);
        using opennova::world::anim_state::kIdle;
        using opennova::world::anim_state::kWalkForward;
        TEST_EXPECT(rings.variant_count(rid, kIdle) == 2);
        TEST_EXPECT(rings.variant_count(rid, kWalkForward) == 1);
        // A missing state binds RESET's ring (size 1).
        TEST_EXPECT(rings.variant_count(rid, 199) == 1);

        // Sample several ticks of each variant and compare forward travel.
        auto travel = [&](int state, int variant) {
            int32_t ph = -1;
            int32_t sum = 0;
            for (int i = 0; i < 8; ++i) {
                RootMotionFrame fr{};
                if (!rings.advance_variant(rid, state, variant, ph, fr)) return INT32_MIN;
                sum += fr.dx;
            }
            return sum;
        };
        const int32_t idle_v0 = travel(kIdle, 0);
        const int32_t idle_v1 = travel(kIdle, 1);
        const int32_t idle_v2 = travel(kIdle, 2); // wraps -> variant 0
        const int32_t walk_v0 = travel(kWalkForward, 0);
        std::printf("[adm] ring travel idle[0]=%d idle[1]=%d idle[2]=%d walk=%d\n",
                idle_v0, idle_v1, idle_v2, walk_v0);
        TEST_EXPECT(idle_v0 != INT32_MIN && idle_v1 != INT32_MIN);
        TEST_EXPECT(idle_v1 == walk_v0);   // variant 1 IS the walk clip
        TEST_EXPECT(idle_v1 != idle_v0);   // and it differs from variant 0
        TEST_EXPECT(idle_v2 == idle_v0);   // wrap modulo the ring size
        // The variant-less advance is variant 0.
        int32_t ph = -1;
        int32_t sum = 0;
        for (int i = 0; i < 8; ++i) {
            RootMotionFrame fr{};
            TEST_EXPECT(rings.advance(rid, kIdle, ph, fr));
            sum += fr.dx;
        }
        TEST_EXPECT(sum == idle_v0);

        // Per-variant queries follow the SERVED ring entry, not entry 0 — the
        // state-entry rotate re-inits the channel from the served entry, so its
        // own frame count is the promotion clock and its own bottom is the dip
        // [orig: ring rotate @0x40b740-0x40b749; frame_count read @0x40b25d].
        const int32_t idle_len_v0 = rings.clip_length_ticks(rid, kIdle, 0);
        const int32_t idle_len_v1 = rings.clip_length_ticks(rid, kIdle, 1);
        const int32_t walk_len = rings.clip_length_ticks(rid, kWalkForward, 0);
        std::printf("[adm] ring length idle[0]=%d idle[1]=%d walk=%d\n",
                idle_len_v0, idle_len_v1, walk_len);
        TEST_EXPECT(idle_len_v1 == walk_len); // variant 1 IS the walk clip
        TEST_EXPECT(rings.clip_length_ticks(rid, kIdle, 2) == idle_len_v0); // wrap
        TEST_EXPECT(rings.capsule_bottom_at(rid, kIdle, 4, 1) ==
                    rings.capsule_bottom_at(rid, kWalkForward, 4, 0));
        uint32_t ring_words_v1[8] = {};
        uint32_t walk_words[8] = {};
        const int n_v1 = rings.scan_triggers(rid, kIdle, -1, 7, ring_words_v1, 8, 1);
        const int n_walk = rings.scan_triggers(rid, kWalkForward, -1, 7, walk_words, 8, 0);
        TEST_EXPECT(n_v1 == n_walk);
        for (int i = 0; i < n_v1; ++i) TEST_EXPECT(ring_words_v1[i] == walk_words[i]);

        // A row names its slot by its key past the first five characters,
        // whatever they are, and a later row naming the same slot adds to that
        // slot's ring (retail FSldr02 repeats its emplaced rows).
        // [orig: AnimMap_ParseConfigLine @0x40cb60 -> AnimMap_FindSlotByName
        //  @0x40cfa0, stricmp on key + 5; AnimMap_RegisterBoneNode @0x40c2d0]
        {
            std::ofstream f(dir + "/slots.adm", std::ios::binary);
            f << "\r\nANIM_RESET\t\t\t\t\"idle\"\r\n"
                 "xxxx_walk_forward\t\t\t\"walk\"\r\n"
                 "anim_idle\t\t\t\t\"idle\"\r\n"
                 "ANIM_IDLE\t\t\t\t\"walk\"\r\n\r\n\r\n";
            TEST_EXPECT(static_cast<bool>(f));
        }
        opennova::ResourceIndex slot_index;
        opennova::assets::AssetStore slot_assets{&slot_index};
        TEST_EXPECT(slot_index.scan(dir));
        AdmRootMotion slots;
        const int sid = slots.register_adm(&slot_assets, "slots.adm");
        TEST_EXPECT(sid == 0);
        TEST_EXPECT(slots.has_clip(sid, opennova::world::anim_state::kReset));
        TEST_EXPECT(slots.has_clip(sid, kWalkForward));
        TEST_EXPECT(slots.variant_count(sid, kIdle) == 2);
        TEST_EXPECT(slots.clip_length_ticks(sid, kIdle, 1) == walk_len);
    }

    // A token whose .bad does not load registers failsafe.bad in its place when the
    // mount has one, in each of the three .adm readers (the ring keeps its places),
    // and nothing without one; a lost reset clip binds the failsafe; a clip named
    // outside a table takes none.
    // [orig: AnimMap_ParseConfigLine @0x40cb60 -> AnimMap_FindOrLoadBoneFile @0x40c030,
    //  the failsafe entry @0x40c25b..0x40c2a1, none @0x40c260; AnimMap_Init @0x40be40,
    //  the load @0x40be96..0x40bead]
    {
        namespace fs = std::filesystem;
        using opennova::world::anim_state::kIdle;
        using opennova::world::anim_state::kWalkForward;
        const fs::path dir = fs::path(test_paths_temp_dir()) /
                ("opennova_rm_failsafe_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct TempRoot {
            fs::path path;
            ~TempRoot() {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } const cleanup{dir};
        std::error_code ec;
        fs::create_directories(dir, ec);
        auto put = [&](const char *from, const char *to) {
            std::vector<uint8_t> bytes;
            if (!index.read_file(from, bytes)) return false;
            std::ofstream f((dir / to).string(), std::ios::binary);
            f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(f);
        };
        TEST_EXPECT(put("idle.bad", "idle.bad") && put("walk.bad", "walk.bad") && put("walk.bad", "failsafe.bad"));
        std::ofstream((dir / "gap.adm").string(), std::ios::binary)
                << "anim_reset \"idle\"\r\nanim_idle \"idle\" \"absent\" \"idle\"\r\nanim_walk_forward \"walk\"\r\n";
        std::ofstream((dir / "lost_reset.adm").string(), std::ios::binary)
                << "anim_reset \"absent\"\r\nanim_idle \"idle\"\r\n";

        opennova::ResourceIndex fs_index;
        opennova::assets::AssetStore fs_assets{&fs_index};
        TEST_EXPECT(fs_index.scan(dir.string()));
        AdmRootMotion motion;
        const int gap = motion.register_adm(&fs_assets, "gap.adm");
        TEST_EXPECT(gap >= 0 && motion.variant_count(gap, kIdle) == 3);
        TEST_EXPECT(motion.clip_length_ticks(gap, kIdle, 1) == motion.clip_length_ticks(gap, kWalkForward, 0));
        // The failsafe here is walk's bytes: its variant travels, idle's do not.
        auto travel = [&](int state, int variant) {
            int32_t phase = -1, sum = 0;
            for (int i = 0; i < 8; ++i) {
                RootMotionFrame fr{};
                if (!motion.advance_variant(gap, state, variant, phase, fr)) return INT32_MIN;
                sum += fr.dx;
            }
            return sum;
        };
        TEST_EXPECT(travel(kIdle, 1) == travel(kWalkForward, 0) && travel(kIdle, 1) != travel(kIdle, 0));
        uint32_t served_words[8] = {}, walk_words[8] = {};
        const int n_served = motion.scan_triggers(gap, kIdle, -1, 7, served_words, 8, 1);
        TEST_EXPECT(n_served == motion.scan_triggers(gap, kWalkForward, -1, 7, walk_words, 8, 0));
        for (int i = 0; i < n_served; ++i) TEST_EXPECT(served_words[i] == walk_words[i]);
        opennova::anim::AdmClipIndex facts;
        TEST_EXPECT(facts.load(&fs_assets, "gap.adm") > 0 && facts.clips_for("anim_idle") &&
                    facts.clips_for("anim_idle")->size() == 3);
        opennova::anim::SkeletalClips rig;
        TEST_EXPECT(rig.load_from_adm(&fs_assets, "gap.adm", {}, {}));
        const opennova::anim::SkeletalClips::ClipSource *served = rig.find_clip_source("anim_idle", 1);
        TEST_EXPECT(served && served->file == "failsafe.bad" && served->entry == 1 && served->token == 1);
        std::string key;
        TEST_EXPECT(rig.variant_of(1, 2, key) == 2 && key == "anim_idle");
        TEST_EXPECT(rig.load_from_adm(&fs_assets, "lost_reset.adm", {}, {}));
        TEST_EXPECT(rig.find_clip("anim_reset") && rig.find_clip("anim_reset")->source.file == "failsafe.bad");
        TEST_EXPECT(rig.load_from_files(&fs_assets, "idle.bad", {{"anim_idle", "idle.bad"}, {"anim_run", "absent.bad"}}));
        TEST_EXPECT(rig.find_clip("anim_run") == nullptr);

        // Without failsafe.bad the token registers nothing and the ring closes up.
        TEST_EXPECT(fs::remove(dir / "failsafe.bad", ec));
        opennova::ResourceIndex bare_index;
        opennova::assets::AssetStore bare_assets{&bare_index};
        TEST_EXPECT(bare_index.scan(dir.string()));
        AdmRootMotion bare;
        const int bare_gap = bare.register_adm(&bare_assets, "gap.adm");
        TEST_EXPECT(bare_gap >= 0 && bare.variant_count(bare_gap, kIdle) == 2);
        TEST_EXPECT(facts.load(&bare_assets, "gap.adm") > 0 && facts.clips_for("anim_idle")->size() == 2);
        TEST_EXPECT(rig.load_from_adm(&bare_assets, "gap.adm", {}, {}));
        TEST_EXPECT(rig.variant_of(1, 1, key) == -1 && rig.variant_of(1, 2, key) == 1);
        TEST_EXPECT(!rig.load_from_adm(&bare_assets, "lost_reset.adm", {}, {}));
    }

    // THE CROSSED-FRAME TRIGGER SCAN — one entry per authored frame entered,
    // in order, never coalesced [orig: the per-frame consume org2
    // @0x4b76e6-0x4b78a8]. Walked against the same clip `advance` uses.
    {
        uint32_t words[8] = {0};
        // A fresh clip start (from_phase = -1) fires frame 0.
        const int first = source.scan_triggers(soldier, opennova::world::anim_state::kReset, -1, 0,
                                               words, 8, 0);
        TEST_EXPECT(first == 1);
        // Re-scanning the SAME span from the same start does not double-fire
        // (the scan is a pure function of the span, so the caller advances
        // from_phase; this pins that a zero-width span yields nothing).
        TEST_EXPECT(source.scan_triggers(soldier, opennova::world::anim_state::kReset, 0, 0, words, 8, 0) == 0);
        // At 30 fps, the third 62 Hz tick crosses the first frame boundary.
        const int one = source.scan_triggers(soldier, opennova::world::anim_state::kReset, 0, 3, words, 8, 0);
        TEST_EXPECT(one == 1);
        // A wide span reports every frame it crossed, bounded by max_out.
        const int many = source.scan_triggers(soldier, opennova::world::anim_state::kReset, 0, 64, words, 8, 0);
        TEST_EXPECT(many > 1);
        TEST_EXPECT(many <= 8);
        uint32_t two_only[2] = {0};
        TEST_EXPECT(source.scan_triggers(soldier, opennova::world::anim_state::kReset, 0, 64, two_only, 2, 0) == 2);
        // An unauthored state falls back to RESET's channel, the same
        // AnimMap registration rule the length/advance paths follow — so a
        // scan of state 9999 reports RESET's words, not nothing.
        TEST_EXPECT(source.scan_triggers(soldier, 9999, -1, 8, words, 8, 0) ==
                    source.scan_triggers(soldier, opennova::world::anim_state::kReset,
                                         -1, 8, words, 8, 0));
        // An UNREGISTERED SET has no track at all.
        TEST_EXPECT(source.scan_triggers(7, 0, -1, 8, words, 8, 0) == 0);
        // A null destination scans nothing rather than faulting.
        TEST_EXPECT(source.scan_triggers(soldier, opennova::world::anim_state::kReset,
                                         -1, 8, nullptr, 8, 0) == 0);
        // The capsule-bottom dip is readable at a position without advancing;
        // it matches what advance() reports for the same frame.
        TEST_EXPECT(source.capsule_bottom_at(
                            soldier, opennova::world::anim_state::kReset, 0, 0) == bottom0);
        TEST_EXPECT(source.capsule_bottom_at(7, 0, 0, 0) == 0);
    }

    // clear() empties the registry.
    source.clear();
    TEST_EXPECT(source.empty());
    TEST_EXPECT(source.clip_count(0) == 0);

    // A null index registers nothing, never crashes.
    AdmRootMotion no_index;
    TEST_EXPECT(no_index.register_adm(nullptr, "soldier.adm") == -1);

    std::printf("native anim adm root motion: OK\n");
    return 0;
}
