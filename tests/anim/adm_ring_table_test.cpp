// The first-person weapon rings (IDA 2026-09-27): one ring-head table per
// loaded .adm shared by every weapon naming it, serving a row's LAST token
// first and walking backward; the weapon table's load sequence (each bake read
// in weapon.def order, one idle play at each def's END, one more per def after
// the file); and the FP channel's loop wrap, which serves the slot's ring and
// fades another entry in over eight advances.
// [orig: AnimMap_RegisterBoneNode @0x40C2D0 (@0x40C385); Anim_GetDurationTicks
//  @0x53EE10; AnimMap_PlayAnimBySlot @0x40BDA0; Anim_InitActions @0x541FA0
//  (@0x54225A); WeaponDefs_PlayIdleAnimAll @0x53FC10; AnimMap_AdvanceToNextAnim
//  @0x40BDF0; AnimChannel_AdvanceBlendedPlayback @0x40B1E0]
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

#include <base/resource_index/resource_index.h>
#include <formats/bad/bad_build.h>
#include <formats/def/def.h>
#include <runtime/anim/adm_fallback.h>
#include <runtime/anim/adm_ring_table.h>
#include <runtime/assets/asset_store.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/weapon_table_build.h>
#include <runtime/world/world.h>

namespace fs = std::filesystem;
using namespace opennova;
using opennova::anim::AdmRingTable;
using opennova::anim::AdmServed;

namespace {

// A one-bone clip at `fps` over `frames` intervals.
bool write_clip(const fs::path &path, uint32_t fps, uint32_t frames, bool loop) {
	bad::BadBuildClip clip;
	clip.name = path.stem().string();
	clip.fps = fps;
	clip.frame_count = frames;
	clip.flags = loop ? bad::BAD_FLAG_LOOP : 0u;
	bad::BadBuildBone bone;
	bone.name = "BN01";
	bone.keys.assign(frames + 1, bad::BadBuildQuat{});
	clip.bones.push_back(bone);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bad::bad_build_mint(clip, nullptr, bad::bad_retail_limits(), bytes, &error)) return false;
	return test_io::write_file(path.string(), bytes);
}

} // namespace

int main() {
	const fs::path dir = fs::path(test_paths_temp_dir()) / test_paths_unique("opennova_adm_ring_table");
	std::error_code ignored;
	fs::remove_all(dir, ignored);
	fs::create_directories(dir);
	// i1 steps a quarter a tick (a wrap every 4), i2 an eighth (every 8).
	TEST_EXPECT(write_clip(dir / "rst.bad", 30, 1, false));
	TEST_EXPECT(write_clip(dir / "i1.bad", 31, 2, true));
	TEST_EXPECT(write_clip(dir / "i2.bad", 31, 4, true));
	TEST_EXPECT(write_clip(dir / "f.bad", 31, 2, false));
	TEST_EXPECT(write_clip(dir / "solo.bad", 31, 2, true));
	{
		std::ofstream(dir / "g.adm", std::ios::binary)
				<< "\r\nanim_reset\t\"rst\"\r\nanim_wpn_idle\t\"i1\" \"i2\"\r\nanim_wpn_fire\t\"f\"\r\n"
				   "anim_notaslot\t\"f\"\r\nanim_wpn_reload\t\"solo\"\r\n";
		std::ofstream(dir / "noreset.adm", std::ios::binary) << "\r\nanim_wpn_idle\t\"i1\"\r\n";
		std::ofstream(dir / "default.adm", std::ios::binary)
				<< "\r\nanim_reset\t\"rst\"\r\nanim_wpn_idle\t\"i1\"\r\n";
	}
	ResourceIndex index;
	TEST_EXPECT(index.scan(dir.string()));
	assets::AssetStore assets{&index};

	// --- the name the load opens, one rule for every entity and weapon: the
	// authored name cut at its LAST '.' with ".adm" appended, default.adm where
	// the roots lack that file, no map for no name [orig: AnimMap_LoadAdmFile
	// @0x40CCA1, the swap @0x40CCB9..0x40CCF8, the miss @0x40CD00..0x40CD25] ---
	{
		const auto exists = [&index](const std::string &file) { return index.has_file(file); };
		TEST_EXPECT(anim::adm_file_name("").empty());
		TEST_EXPECT(anim::adm_load_name("", exists).empty());
		TEST_EXPECT(anim::adm_file_name("ak47_1st") == "ak47_1st.adm");
		TEST_EXPECT(anim::adm_load_name("g", exists) == "g.adm");
		TEST_EXPECT(anim::adm_load_name("G.ADM", exists) == "G.adm");
		TEST_EXPECT(anim::adm_load_name("g.txt", exists) == "g.adm"); // any extension is swapped
		TEST_EXPECT(anim::adm_load_name("g.adm.bak", exists) == "default.adm"); // opens g.adm.adm
		TEST_EXPECT(anim::adm_load_name("absent", exists) == "default.adm");
		TEST_EXPECT(anim::adm_file_name(".") == ".adm");
	}

	// --- the table: last first, shared, the reset slot and the backfill ---
	{
		AdmRingTable rings;
		TEST_EXPECT(rings.load(&assets, "g"));
		TEST_EXPECT(rings.loaded("G.ADM"));
		TEST_EXPECT(!rings.load(&assets, "noreset")); // no reset clip, no table
		TEST_EXPECT(rings.resolves("g", "anim_wpn_recoil")); // a slot, authored or not
		TEST_EXPECT(!rings.resolves("g", "anim_notaslot"));
		TEST_EXPECT(!rings.resolves("other", "anim_wpn_idle"));
		TEST_EXPECT(rings.peek("g", "anim_wpn_idle").variant == 1);
		TEST_EXPECT(rings.serve("g", "anim_wpn_idle").variant == 1); // "i1" "i2" serves i2 first
		TEST_EXPECT(rings.serve("g", "ANIM_WPN_IDLE").variant == 0);
		TEST_EXPECT(rings.serve("g", "xxxx_wpn_idle").variant == 1);
		// A slot the table does not author holds the first reset token; slot 0
		// the last. Neither moves.
		const AdmServed backfill = rings.serve("g", "anim_wpn_recoil");
		TEST_EXPECT(backfill.key == "anim_reset" && backfill.variant == 0 && backfill.clip != nullptr);
		TEST_EXPECT(rings.serve("g", "anim_reset").key == "anim_reset");
		TEST_EXPECT(!rings.serve("g", "anim_notaslot").valid());
		// A later weapon naming the table gets the same heads back.
		TEST_EXPECT(rings.load(&assets, "g.adm"));
		TEST_EXPECT(rings.serve("g", "anim_wpn_idle").variant == 0);
		// A definition object's own lengths stand in for a table no store holds.
		std::unordered_map<std::string, std::vector<anim::AdmClipFacts>> own;
		for (float s : {0.5f, 1.0f, 0.25f}) {
			anim::AdmClipFacts facts;
			facts.seconds = s;
			own["anim_wpn_reload"].push_back(facts);
		}
		rings.adopt("dict", own);
		TEST_EXPECT(rings.serve("dict", "anim_wpn_reload").variant == 2);
		TEST_EXPECT(rings.serve("dict", "anim_wpn_reload").clip->seconds == 1.0f);
		own["anim_wpn_reload"].clear();
		rings.adopt("g", own); // a loaded table keeps its own rings
		TEST_EXPECT(rings.serve("g", "anim_wpn_idle").variant == 1);
	}

	// --- the weapon table's load sequence: both weapons read the SAME ring.
	// A: its idle DELAYEND auto reads i2 (9 ticks), its END plays i1.
	// B: DELAYSTART auto reads i2 (9), DELAYEND auto reads i1 (5 < 9: kept),
	//    its END plays i2. After the file each plays once more: i1, then i2.
	{
		static const char kDefs[] =
				"weapon \"WPN_A\"\r\n\tanimadm g\r\n"
				"\taction \"idle\"\r\n\t\tdelaystart 0\r\n\t\tdelayend auto\r\n\t\tanim anim_wpn_idle\r\n\tend\r\n"
				"end\r\n"
				"weapon \"WPN_B\"\r\n\tanimadm G.ADM\r\n"
				"\taction \"idle\"\r\n\t\tdelaystart auto\r\n\t\tdelayend auto\r\n\t\tanim anim_wpn_idle\r\n\tend\r\n"
				"end\r\n"
				"weapon \"WPN_C\"\r\n\tanimadm absent.adm\r\n"
				"\taction \"idle\"\r\n\t\tdelaystart 0\r\n\t\tdelayend auto\r\n\t\tanim anim_wpn_idle\r\n\tend\r\n"
				"end\r\n";
		def::DefWeaponsFile parsed{};
		TEST_EXPECT(def::def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(kDefs), sizeof(kDefs) - 1,
				&parsed) == 0);
		world::WeaponTable table = world::build_weapon_table(parsed, &assets);
		const world::WeaponTableEntry *a = table.by_index(1);
		const world::WeaponTableEntry *b = table.by_index(2);
		TEST_EXPECT(a != nullptr && b != nullptr);
		if (a != nullptr && b != nullptr) {
			const auto &ai = a->action_fsm.actions[world::weapon_action::kIdle];
			const auto &bi = b->action_fsm.actions[world::weapon_action::kIdle];
			std::printf("[rings] A idle %d/%d  B idle %d/%d\n", ai.delay_start, ai.delay_end,
					bi.delay_start, bi.delay_end);
			TEST_EXPECT(ai.delay_start == 0 && ai.delay_end == 9);
			TEST_EXPECT(bi.delay_start == 9 && bi.delay_end == 5);
			// Each entry keeps the name its load opened and keyed the table on.
			TEST_EXPECT(a->animadm == "g.adm" && b->animadm == "G.adm");
		}
		// Seven serves so far (A's read and END, B's two reads and END, the two
		// after the file): the first play in the match serves i1.
		TEST_EXPECT(table.rings.peek("g", "anim_wpn_idle").variant == 0);
		// An ANIMADM the roots lack loads default.adm, keyed on that name.
		// [orig: AnimMap_LoadAdmFile @0x40CD00..0x40CD25]
		const world::WeaponTableEntry *c = table.by_index(3);
		TEST_EXPECT(c != nullptr && c->animadm == "default.adm");
		if (c != nullptr) TEST_EXPECT(c->action_fsm.actions[world::weapon_action::kIdle].delay_end == 5);

		// A mount by name runs what the load baked and reads no ring; a
		// same-weapon re-bake reads a copy of the match's heads; the mount of
		// a weapon whose file is missing binds default.adm.
		world::World w;
		w.registry.configure_pool(0, 8);
		world::Entity seed;
		seed.kind = world::EntityKind::Organic;
		seed.alive = true;
		seed.health = 100;
		w.cached.local_player = w.registry.spawn(0, seed);
		w.tables.weapons = std::move(table);
		world::LocalPlayerWeapon lw;
		world::PlayerViewState view;
		world::WeaponInstallData data;
		data.name = "WPN_B";
		data.animadm = "G.ADM";
		data.table_baked = true;
		const int32_t head = w.tables.weapons.rings.peek("g", "anim_wpn_idle").variant;
		world::local_weapon_install(w, lw, data, false, false, nullptr, view);
		TEST_EXPECT(lw.def.actions[world::weapon_action::kIdle].delay_start == 9);
		TEST_EXPECT(w.tables.weapons.rings.peek("g", "anim_wpn_idle").variant == head);
		world::WeaponFsmActionRow idle;
		std::snprintf(idle.name, sizeof(idle.name), "idle");
		std::snprintf(idle.anim, sizeof(idle.anim), "anim_wpn_idle");
		idle.delaystart = 0;
		idle.delayend = -1; // one read: a served head would move
		data.rows = {idle};
		data.table_baked = false;
		world::local_weapon_install(w, lw, data, true, true, nullptr, view);
		TEST_EXPECT(lw.def.actions[world::weapon_action::kIdle].delay_end > 0);
		TEST_EXPECT(w.tables.weapons.rings.peek("g", "anim_wpn_idle").variant == head);
		world::WeaponInstallData missing;
		missing.name = "WPN_C";
		missing.animadm = "absent.adm";
		missing.table_baked = true;
		world::local_weapon_install(w, lw, missing, false, false, nullptr, view);
		TEST_EXPECT(lw.anim_map == "default.adm");
		def::def_free_weapons(&parsed);
	}

	// --- the FP channel: a play serves, a loop wrap fades the next entry in ---
	{
		AdmRingTable rings;
		TEST_EXPECT(rings.load(&assets, "g"));
		world::LocalPlayerWeapon w;
		w.anim_map = "g.adm";
		TEST_EXPECT(!world::fp_channel_play(rings, w, "anim_reset")); // slot 0: no play
		TEST_EXPECT(world::fp_channel_play(rings, w, "anim_wpn_idle"));
		TEST_EXPECT(w.anim_key == "anim_wpn_idle" && w.anim_variant == 1 && w.anim_advance_ticks == 0);
		for (int t = 1; t <= 7; ++t) world::fp_channel_advance(rings, w);
		TEST_EXPECT(!w.anim_blending && w.anim_advance_ticks == 7);
		world::fp_channel_advance(rings, w); // i2 wraps: the ring serves i1
		TEST_EXPECT(w.anim_blending && w.anim_blend_variant == 0 && w.anim_blend_ticks == 0);
		TEST_EXPECT(w.anim_blend_weight == 0.0f && w.anim_fade_countdown == 8);
		TEST_EXPECT(w.anim_latched_variant == 0 && w.anim_variant == 1);
		for (int k = 1; k <= 7; ++k) {
			world::fp_channel_advance(rings, w);
			TEST_EXPECT(w.anim_blending && w.anim_blend_weight == 0.125f * static_cast<float>(k));
			TEST_EXPECT(w.anim_advance_ticks == static_cast<uint32_t>(8 + k)); // the outgoing runs on
		}
		world::fp_channel_advance(rings, w); // the eighth: the incoming replaces it
		TEST_EXPECT(!w.anim_blending && w.anim_variant == 0 && w.anim_advance_ticks == 8);
		// i1 at tick 8 sits at t = 0; its next wrap (tick 12) serves i2 again.
		for (int t = 0; t < 4; ++t) world::fp_channel_advance(rings, w);
		TEST_EXPECT(w.anim_blending && w.anim_blend_variant == 1);
		// A play restarts only the primary half: the fade runs on and still
		// promotes its clip over the played one.
		TEST_EXPECT(world::fp_channel_play(rings, w, "anim_wpn_fire"));
		TEST_EXPECT(w.anim_key == "anim_wpn_fire" && w.anim_blending);
		TEST_EXPECT(w.anim_slot_key == "anim_wpn_fire");
		for (int k = 0; k < 8; ++k) world::fp_channel_advance(rings, w);
		TEST_EXPECT(!w.anim_blending && w.anim_key == "anim_wpn_idle" && w.anim_variant == 1);
		// A single-entry loop wraps onto itself: nothing fades.
		TEST_EXPECT(world::fp_channel_play(rings, w, "anim_wpn_reload"));
		for (int t = 0; t < 9; ++t) world::fp_channel_advance(rings, w);
		TEST_EXPECT(!w.anim_blending && w.anim_key == "anim_wpn_reload" && w.anim_advance_ticks == 9);
		// A slot the table does not author plays its reset clip.
		TEST_EXPECT(world::fp_channel_play(rings, w, "anim_wpn_recoil"));
		TEST_EXPECT(w.anim_key == "anim_reset" && w.anim_variant == 0);
		TEST_EXPECT(w.anim_slot_key == "anim_wpn_recoil");
	}

	fs::remove_all(dir, ignored);
	return 0;
}
