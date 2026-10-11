// The anti-cheat challenge sources computed from definitions
// (runtime/inmatch/integrity_sources.h): retail's in-memory ammo.def and
// weapon.def tables rebuilt from synthetic corpora field by field (the
// tokenizer's quirks, the allocation and inherit rules, the effects tables,
// the tracer item resolve, the action bind's .adm duration rings and the
// whole-table sanitizer), and, with --retail, from the pinned install's jox01
// corpus against the live witness (integrity_oracles.h).
// [orig: AmmoDef_LoadAll @0x40B0B0; AmmoDef_ParseProperty @0x40A2D0;
//  WeaponDefs_ParseLineCallback @0x543680; Anim_InitActions @0x541FA0;
//  WeaponSlotDef_ComputeSanitizedChecksum @0x53F8F0;
//  NetPacket_WriteEntityCRCChecksum @0x42B020]
#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_paths.h"
#include "integrity_oracles.h"

#include <base/io/crc32_mpeg2.h>
#include <base/io/le.h>
#include <base/resource_index/boot_policy.h>
#include <base/resource_index/resource_index.h>
#include <formats/bad/bad_build.h>
#include <formats/lwf/lwf.h>
#include <runtime/assets/asset_store.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/inmatch/integrity_ammo_image.h>
#include <runtime/inmatch/integrity_sources.h>
#include <runtime/inmatch/integrity_weapon_image.h>
#include <runtime/mission/runtime_boot.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace opennova;
using inmatch::AmmoRecordTable;
using inmatch::WeaponRecordTable;

namespace {

int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		if (!(expr)) {                                                                             \
			std::printf("%s:%d: FAIL: %s\n", __FILE__, __LINE__, #expr);                           \
			++g_failures;                                                                          \
		}                                                                                          \
	} while (0)

std::vector<uint8_t> bytes_of(const std::string &text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

int32_t i32(const std::vector<uint8_t> &bytes, size_t row_size, int row, size_t off) {
	return static_cast<int32_t>(
			io::read_u32_le(bytes.data() + static_cast<size_t>(row) * row_size + off));
}
int32_t u16(const std::vector<uint8_t> &bytes, size_t row_size, int row, size_t off) {
	return io::read_u16_le(bytes.data() + static_cast<size_t>(row) * row_size + off);
}
int32_t u8(const std::vector<uint8_t> &bytes, size_t row_size, int row, size_t off) {
	return bytes[static_cast<size_t>(row) * row_size + off];
}
std::string str(
		const std::vector<uint8_t> &bytes, size_t row_size, int row, size_t off, size_t max) {
	const char *p = reinterpret_cast<const char *>(
			bytes.data() + static_cast<size_t>(row) * row_size + off);
	return std::string(p, strnlen(p, max));
}

int32_t ammo32(const AmmoRecordTable &t, int row, size_t off) {
	return i32(t.bytes, inmatch::kAmmoRecordSize, row, off);
}
int32_t ammo16(const AmmoRecordTable &t, int row, size_t off) {
	return u16(t.bytes, inmatch::kAmmoRecordSize, row, off);
}
int32_t ammo8(const AmmoRecordTable &t, int row, size_t off) {
	return u8(t.bytes, inmatch::kAmmoRecordSize, row, off);
}
int32_t weapon32(const WeaponRecordTable &t, int row, size_t off) {
	return i32(t.bytes, inmatch::kWeaponRecordSize, row, off);
}
int32_t weapon8(const WeaponRecordTable &t, int row, size_t off) {
	return u8(t.bytes, inmatch::kWeaponRecordSize, row, off);
}
std::string weapon_str(const WeaponRecordTable &t, int row, size_t off, size_t max) {
	return str(t.bytes, inmatch::kWeaponRecordSize, row, off, max);
}

AmmoRecordTable ammo_table(const std::string &text,
		const std::vector<inmatch::IntegrityItemRow> &items = {},
		const audio::SoundSetIndex *sounds = nullptr) {
	io::ConfigTokens tokens;
	const std::vector<uint8_t> bytes = bytes_of(text);
	return inmatch::build_ammo_record_table(bytes.data(), bytes.size(), items, sounds, tokens);
}

// A one-bone clip at `fps` over `frames` intervals.
bool write_clip(const fs::path &path, uint32_t fps, uint32_t frames) {
	bad::BadBuildClip clip;
	clip.name = path.stem().string();
	clip.fps = fps;
	clip.frame_count = frames;
	bad::BadBuildBone bone;
	bone.name = "BN01";
	bone.keys.assign(frames + 1, bad::BadBuildQuat{});
	clip.bones.push_back(bone);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bad::bad_build_mint(clip, nullptr, bad::bad_retail_limits(), bytes, &error))
		return false;
	return test_io::write_file(path.string(), bytes);
}

bool write_text(const fs::path &path, const std::string &text) {
	return test_io::write_file(path.string(), bytes_of(text));
}

// --- the item list ---------------------------------------------------------

void test_item_rows() {
	const std::vector<uint8_t> items =
			bytes_of("begin ITEM_ZERO\r\nid 100000\r\nend\r\n"
					 "id 777\r\n" // outside a block: no row takes it
					 "begin ITEM_ONE\r\nid 104000\r\nend\r\n"
					 "begin ITEM_TWO\r\nid 105310\r\n"
					 "begin Ammo_One\r\nend\r\n" // a nested begin opens the next row
					 "begin A_NAME_OF_FORTY_NINE_CHARACTERS_RUNNING_PAST_IT\r\nend\r\n");
	const std::vector<inmatch::IntegrityItemRow> rows =
			inmatch::read_integrity_item_rows(items.data(), items.size());
	CHECK(rows.size() == 5);
	if (rows.size() != 5)
		return;
	CHECK(rows[0].name == "ITEM_ZERO" && rows[0].id == 0);
	CHECK(rows[1].name == "ITEM_ONE" && rows[1].id == 4000);
	CHECK(rows[2].name == "ITEM_TWO" && rows[2].id == 5310);
	CHECK(rows[3].name == "Ammo_One" && rows[3].id == 0); // no id line: the memset 0
	CHECK(rows[4].name == "A_NAME_OF_FORTY_NINE_CHARACTERS_RUNNING_PAST_I" &&
			rows[4].name.size() == 46);
}

std::vector<inmatch::IntegrityItemRow> sample_items() {
	return { { "ITEM_ZERO", 0 }, { "ITEM_ONE", 4000 }, { "ITEM_TWO", 5310 }, { "Ammo_One", 0 } };
}

// --- the ammo row ----------------------------------------------------------

void test_ammo_fields() {
	const AmmoRecordTable t = ammo_table(
			"ammo AT_NULL\r\nvelocity 900\r\nmax_age 2.5\r\ndrag 0.5\r\nend\r\n"
			"ammo AMMO_ONE\r\n"
			"flag silenced\r\nflag internal\r\nflag Priority\r\nflag bogus\r\n"
			"arm_age 0.25\r\nfrndlyTrcrID 5310\r\nfoeTrcrID 999\r\n"
			"error 1.5\r\nbullet_radius 0.75\r\nMF_Light 7\r\n"
			"kztype rounds_kz_C4\r\nkz_damage 70000\r\nspread_count 9\r\n"
			"kz_minradius 3\r\nkz_maxradius 12.5\r\nkz_pieslice 45\r\n"
			"turnrate_maxpit 1.25\r\nturnrate_maxyaw 0.5\r\nboresight_maxang 10\r\n"
			"climb_angle 30\r\nclimb_time 1.5\r\nheat_det_range 4.5\r\nradar_det_range 7.5\r\n"
			"scorch_id 12\r\nscar_type 3\r\nlight_move 3 255 128 64\r\nlight_impact 1.5 1 2 3 0\r\n"
			"min_stable_velocity 300\r\ntumble_error 0.04\r\nweight_in_grains 55\r\n"
			"min_damage 10\r\nmax_damage 90\r\npenetration_impact 4\r\npenetration_kz 5\r\n"
			"armor_density 1 2 3\r\nsecondary_anim 300\r\nkz_physics 2\r\ntracerRate 3\r\n"
			"recoil 10 20 30\r\ntracer_type rapidred\r\ndopplerdiv 257\r\nnotarmmedammo AT_NULL\r\n"
			"ai_launch some_set\r\nkz_sound some_set\r\nai_launcheffect fx\r\nsecondary_effect "
			"fx\r\n"
			"unknown_key 5\r\n"
			"end\r\n"
			"ammo AMMO_TWO\r\ntracer_type stdred 77\r\ndrag 65535.99999\r\nfrndlyTrcrID 0\r\n"
			"foeTrcrID 4000\r\nkztype rounds_kz_null\r\nend\r\n",
			sample_items());
	CHECK(t.capacity == 3 && t.count == 3);
	// The template row keeps its own values and is never inherited into.
	CHECK(ammo32(t, 0, 4) == 900 && ammo32(t, 0, 8) == 155 && ammo32(t, 0, 28) == 0x8000);
	CHECK(str(t.bytes, inmatch::kAmmoRecordSize, 0, 144, 32) == "AT_NULL");
	// Row 1, every encoding.
	CHECK(ammo32(t, 1, 0) == 0x840008); // silenced | the first `internal` | Priority
	CHECK(ammo32(t, 1, 4) == 900); // velocity -1, inherited
	CHECK(ammo32(t, 1, 8) == 155); // max_age -1, inherited
	CHECK(ammo32(t, 1, 12) == 16); // arm_age 0.25 s in ticks
	CHECK(ammo32(t, 1, 16) == 2); // the item index of type id 5310
	CHECK(ammo32(t, 1, 20) == 3); // a miss: the item named as the ammo
	CHECK(ammo32(t, 1, 24) == 98304); // error 1.5
	CHECK(ammo32(t, 1, 28) == 0x10000); // the drag default
	CHECK(ammo32(t, 1, 32) == 49152); // bullet_radius 0.75
	CHECK(ammo32(t, 1, 36) == 1 && ammo32(t, 1, 40) == 7);
	CHECK(ammo16(t, 1, 44) == 5 && ammo16(t, 1, 46) == (70000 & 0xFFFF));
	CHECK(ammo32(t, 1, 48) == 9);
	CHECK(ammo32(t, 1, 52) == 196608 && ammo32(t, 1, 56) == 819200);
	CHECK(ammo32(t, 1, 60) == 11930464 * 22); // 45 / 2 in C division
	CHECK(ammo32(t, 1, 64) == 0 && ammo32(t, 1, 68) == 0 && ammo32(t, 1, 72) == 0 &&
			ammo32(t, 1, 76) == 0);
	CHECK(ammo32(t, 1, 80) == 240533 && ammo32(t, 1, 84) == 96213);
	CHECK(ammo32(t, 1, 88) == 119304640 && ammo32(t, 1, 92) == 357913920);
	CHECK(ammo32(t, 1, 96) == 93);
	CHECK(ammo16(t, 1, 100) == 4 && ammo16(t, 1, 102) == 7);
	CHECK(ammo16(t, 1, 108) == 0); // no effects table
	CHECK(ammo16(t, 1, 116) == 12 && ammo16(t, 1, 118) == 3);
	CHECK(ammo32(t, 1, 120) == 196608 && ammo32(t, 1, 124) == 0xFF8040);
	CHECK(ammo32(t, 1, 128) == 0x010203 && ammo32(t, 1, 132) == 98304);
	CHECK(ammo32(t, 1, 136) == 10); // 0 ticks reads 10
	CHECK(ammo32(t, 1, 140) == 0);
	CHECK(str(t.bytes, inmatch::kAmmoRecordSize, 1, 144, 32) == "AMMO_ONE");
	CHECK(ammo32(t, 1, 176) == 300 && ammo32(t, 1, 180) == 2621 && ammo32(t, 1, 184) == 55);
	CHECK(ammo32(t, 1, 188) == 10 && ammo32(t, 1, 192) == 90);
	CHECK(ammo32(t, 1, 196) == 4 && ammo32(t, 1, 200) == 5);
	CHECK(ammo32(t, 1, 204) == 1 && ammo32(t, 1, 208) == 2 && ammo32(t, 1, 212) == 3);
	CHECK(ammo8(t, 1, 224) == 44 && ammo8(t, 1, 225) == 2 && ammo8(t, 1, 226) == 3);
	CHECK(ammo8(t, 1, 227) == 10 && ammo8(t, 1, 228) == 20 && ammo8(t, 1, 229) == 30);
	CHECK(ammo32(t, 1, 232) == 6 && ammo32(t, 1, 236) == 6); // one name: copied
	CHECK(ammo8(t, 1, 240) == 1);
	CHECK(str(t.bytes, inmatch::kAmmoRecordSize, 1, 241, 35) == "AT_NULL");
	// Row 2: two tracer names, a drag that wraps to -1 and inherits, the id
	// that matches row 0 falls back to the name (none: no store), the null
	// kill-zone name is refused.
	CHECK(ammo32(t, 2, 232) == 1 && ammo32(t, 2, 236) == 77);
	CHECK(ammo32(t, 2, 28) == 0x8000);
	CHECK(ammo32(t, 2, 16) == 0 && ammo32(t, 2, 20) == 1);
	CHECK(ammo16(t, 2, 44) == 0);
	CHECK(ammo8(t, 2, 227) == 24 && ammo32(t, 2, 60) == 0x7FFFFFFF && ammo32(t, 2, 80) == -1);
	// The CRC zeroes the six pointer dwords and covers the rest.
	uint32_t crc = 0;
	CHECK(t.row_crc(1, crc));
	uint8_t image[inmatch::kAmmoRecordSize];
	std::memcpy(image, t.bytes.data() + inmatch::kAmmoRecordSize, sizeof(image));
	CHECK(crc == io::crc32_mpeg2(image, sizeof(image)));
	CHECK(t.lookup_by_name("ammo_two") == 2 && t.lookup_by_name("AT_NULL") == 0 &&
			t.lookup_by_name("x") == 0);
}

void test_ammo_tokenizer_quirks() {
	// A short line reads a slot an earlier, longer line left: `recoil 30 30`
	// right after `light_move 3 255 128 64` reads that line's "128" for its
	// third value. A bare LF splits no line. A file that does not end in CR LF
	// loses its last byte, so its final `end` reads `en` and closes nothing.
	const AmmoRecordTable t =
			ammo_table("ammo AT_NULL\r\nvelocity 900\r\nend\r\n"
					   "ammo STALE\r\nlight_move 3 255 128 64\r\nrecoil 30 30\r\n"
					   "velocity 100\nvelocity 200\r\nmax_damage 50 // trailing comment\r\nend\r\n"
					   "ammo OPEN_AT_EOF\r\nend");
	CHECK(t.capacity == 3);
	CHECK(ammo8(t, 1, 227) == 30 && ammo8(t, 1, 228) == 30 && ammo8(t, 1, 229) == 128);
	CHECK(ammo32(t, 1, 4) == 100);
	CHECK(ammo32(t, 1, 192) == 50);
	CHECK(ammo32(t, 2, 4) == -1); // never closed: never inherited
	const AmmoRecordTable closed =
			ammo_table("ammo AT_NULL\r\nvelocity 900\r\nend\r\nammo CLOSED\r\nend\r\n");
	CHECK(ammo32(closed, 1, 4) == 900);
}

void test_ammo_capacity_and_abort() {
	// An `ammo` line inside an open def ends the whole parse; the count pass
	// saw every `ammo` line, so the rows past the abort stay all zero and
	// challengeable, and one past the capacity is out of range.
	const AmmoRecordTable t = ammo_table(
			"ammo A0\r\nend\r\nammo A1\r\nvelocity 5\r\nammo A2\r\nend\r\nAmmo A3\r\nend\r\n");
	CHECK(t.capacity == 4 && t.count == 2);
	CHECK(ammo32(t, 1, 4) == 5 && ammo32(t, 1, 8) == -1);
	const std::vector<uint8_t> zeros(inmatch::kAmmoRecordSize, 0);
	uint32_t crc = 0;
	CHECK(t.row_crc(3, crc) && crc == io::crc32_mpeg2(zeros.data(), zeros.size()));
	CHECK(!t.row_crc(4, crc) && !t.row_crc(-1, crc));
	// A name of 32 characters or more keeps 31.
	const AmmoRecordTable named =
			ammo_table("ammo ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\r\nend\r\n");
	CHECK(str(named.bytes, inmatch::kAmmoRecordSize, 0, 144, 32) ==
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ01234");
}

void test_ammo_effects_tables() {
	lwf::File first;
	first.multis.resize(2);
	first.multis[0].name = "movesnd";
	first.multis[0].target_id = 30;
	first.multis[1].name = "zipsnd";
	first.multis[1].target_id = 60;
	lwf::File second;
	second.multis.resize(1);
	second.multis[0].name = "MOVESND"; // shadowed by the first bank's
	second.multis[0].target_id = 99;
	audio::SoundSetIndex sounds;
	sounds.add_bank(0, first);
	sounds.add_bank(1, second);
	const AmmoRecordTable t = ammo_table(
			"ammo FX0\r\neffects_table\r\nend\r\nend\r\n"
			"ammo FX1\r\nend\r\n"
			"ammo FX2\r\neffects_table\r\nmove none movesnd 1\r\nzip none zipsnd 1\r\n"
			"zip none movesnd 1\r\nbogus none none 1\r\ndirt none\r\nend\r\n"
			"effects_table\r\nwater none none 1\r\nend\r\nend\r\n"
			"ammo FX3\r\neffects_table\r\nmove none MoveSnd 1\r\nend\r\nend\r\n"
			"ammo FX4\r\neffects_table\r\nzip none missing 1\r\nmove none none 1\r\nend\r\nend\r\n",
			{}, &sounds);
	CHECK(ammo16(t, 0, 108) == 1 && ammo32(t, 0, 140) == 0); // installed, empty
	CHECK(ammo16(t, 1, 108) == 0); // none
	CHECK(ammo16(t, 2, 108) == 3); // row 0 + move + zip; the second table is not installed
	CHECK(ammo32(t, 2, 140) == 0x320000); // zip after move, capped
	CHECK(ammo16(t, 3, 108) == 2 && ammo32(t, 3, 140) == (30 << 16));
	CHECK(ammo16(t, 4, 108) == 3 && ammo32(t, 4, 140) == 0); // no sound set found
	// The table pointer is never part of the CRC.
	uint32_t with_pointer = 0;
	CHECK(t.row_crc(2, with_pointer));
	std::vector<uint8_t> image(t.bytes.begin() + 2 * inmatch::kAmmoRecordSize,
			t.bytes.begin() + 3 * inmatch::kAmmoRecordSize);
	io::write_u32_le(image.data() + 104, 0);
	CHECK(with_pointer == io::crc32_mpeg2(image.data(), image.size()));
}

void test_ammo_notarmmed_overflow() {
	// notarmmedammo is an unbounded copy at +241: 75 characters run 40 bytes
	// into the next row, whose allocation re-stamps its first nine dwords and
	// leaves +36..+39 holding characters 71..74.
	const std::string name =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz!@#$%^&*(WXYZ";
	CHECK(name.size() == 75);
	const AmmoRecordTable t =
			ammo_table("ammo N0\r\nnotarmmedammo " + name + "\r\nend\r\nammo N1\r\nend\r\n");
	CHECK(std::memcmp(t.bytes.data() + 241, name.data(), 35) == 0);
	CHECK(std::memcmp(t.bytes.data() + inmatch::kAmmoRecordSize + 36, name.data() + 71, 4) == 0);
	CHECK(ammo8(t, 1, 40) == 0);
}

// --- the weapon.def table --------------------------------------------------

struct WeaponCorpus {
	fs::path dir;
	ResourceIndex index;
	std::unique_ptr<assets::AssetStore> assets;
	mission::BootFileSource files;
};

bool make_weapon_corpus(WeaponCorpus &c) {
	c.dir = fs::path(test_paths_temp_dir()) / test_paths_unique("opennova_integrity_sources");
	std::error_code ignored;
	fs::remove_all(c.dir, ignored);
	fs::create_directories(c.dir);
	// The weapon's .adm: anim_wpn_recoil's three variants serve the last first.
	// Ticks: f 12 frames -> 26, r1 6 -> 14, r2 9 -> 20, r3 3 -> 7 (at 30 fps).
	bool ok = write_clip(c.dir / "rst.bad", 30, 1) && write_clip(c.dir / "f.bad", 30, 12) &&
			write_clip(c.dir / "r1.bad", 30, 6) && write_clip(c.dir / "r2.bad", 30, 9) &&
			write_clip(c.dir / "r3.bad", 30, 3) && write_clip(c.dir / "i.bad", 30, 2);
	ok = ok &&
			write_text(c.dir / "wpn.adm",
					"\r\nanim_reset\t\"rst\"\r\nanim_wpn_fire\t\"f\"\r\nanim_wpn_recoil\t\"r1\" "
					"\"r2\" \"r3\"\r\n"
					"anim_wpn_idle\t\"i\"\r\n");
	for (const char *image :
			{ "clip.tga", "rnd.tga", "sight.tga", "icon.tga", "icon_long_name.tga" })
		ok = ok && write_text(c.dir / image, "x");
	ok = ok && c.index.scan(c.dir.string());
	c.assets = std::make_unique<assets::AssetStore>(&c.index);
	c.files = mission::boot_files_from_index(c.index);
	return ok;
}

WeaponRecordTable weapon_table(
		const WeaponCorpus &c, const AmmoRecordTable &ammo, const std::string &text) {
	io::ConfigTokens tokens;
	const std::vector<uint8_t> bytes = bytes_of(text);
	return inmatch::build_weapon_record_table(
			bytes.data(), bytes.size(), ammo, c.files, c.assets.get(), tokens);
}

void test_weapon_fields(const WeaponCorpus &c) {
	const AmmoRecordTable ammo = ammo_table("ammo AT_NULL\r\nend\r\nammo AMMO_556\r\nend\r\n");
	const WeaponRecordTable t = weapon_table(c, ammo,
			"ammoclass_max_carry CLASS_556 300\r\n"
			"weapon WPN_A\r\n"
			"category 13\r\nrank 5\r\nflags Scoped\r\nflags NoSelect\r\nflags Armor\r\n"
			"statid 7\r\nclipsize 0\r\nstartrounds 90\r\nclassrounds sniper 4\r\nclassrounds "
			"nobody 9\r\n"
			"error 1.5 0.5 0.25 3 0.75 2.5\r\nerror_hipTheta 0.5\r\nerror_upTheta 0.25\r\n"
			"vmacrotoken VOICE\r\nround_type AMMO_556\r\nround_type AT_NULL\r\n"
			"special_hold 2\r\nattack_anim 3\r\nrun_anim 4\r\n"
			"heat_effect smoke 1.5\r\nheat_values 50 3\r\nheat_sound some_set\r\n"
			"charfilter medic\r\ncharfilter engineer\r\nteamfilter blue\r\nammobucket -12\r\n"
			"sameas WPN_B\r\nmaxclips 6\r\nclipweight 0.5\r\nweaponweight 4.5\r\nstability 0.5 "
			"0.25 3\r\n"
			"emplacedstance 1\r\nammoclass CLASS_556 2\r\n"
			"hudclipgfx 10 20 clip.tga\r\nhudclipgfx 30 40 missing.tga\r\n"
			"hudrndgfx 1 2 3 4 5 rnd.tga\r\n"
			"targetpitchmax 10\r\ntargetpitchmin 5\r\ntargetyawrange 90\r\n"
			"launchuserpoint MFLASH\r\nsoundfireloop loop1\r\nswitchcategory 3\r\n"
			"renderfov 65\r\nsplash 9\r\ndesignation_time 2\r\n"
			"sights sight.tga 1 2 3 4 add\r\nsights missing.tga 1 2 3 4 add\r\n"
			"loadout_menu_icon icon_long_name.tga\r\nloadout_menu_icon icon.tga\r\n"
			"weapon_class grenade\r\nfarpinfo 5 6\r\nloadout_selectable 1\r\nloadout_subclasses "
			"2\r\n"
			"scope_paralax_distance 0.5\r\nscope_max_zero 1 2 3 4\r\nscope_max_mag 8 "
			"4\r\nscope_min_mag 3\r\n"
			"pos 1 2 3 4 5 6\r\ngfx1 none\r\ncrosshair none\r\nunknown_key 1\r\n"
			"animadm wpn\r\n"
			"action fire\r\nanim anim_wpn_fire\r\ndelaystart 2\r\ndelayend auto\r\nend\r\n"
			"action recoil\r\nanim anim_wpn_recoil\r\ndelaystart auto\r\ndelay auto\r\nend\r\n"
			"end\r\n"
			"weapon WPN_B\r\nanimadm wpn.txt\r\nammoclass CLASS_NEW 0\r\n"
			"action recoil\r\nanim anim_wpn_recoil\r\ndelaystart auto\r\nend\r\nend\r\n"
			"weapon null\r\nend\r\n"
			"weapon WPN_C\r\nclipsize 5\r\nstatid 3\r\nend\r\n"
			"weapon wpn_c\r\nclipsize 7\r\nend\r\n");
	CHECK(t.computable);
	CHECK(t.live_count() == 5);
	// The engine's "null" row 0, its defaults.
	CHECK(weapon_str(t, 0, 20, 32) == "null" && weapon32(t, 0, 4) == -1 && weapon32(t, 0, 88) == 1);
	CHECK(weapon32(t, 0, 92) == -1 && weapon32(t, 0, 224) == 1 && weapon32(t, 0, 152) == 2);
	CHECK(weapon32(t, 0, 344) == 0x10000 && weapon32(t, 0, 1104) == 0);
	// WPN_A, every non-sanitized encoding.
	CHECK(weapon_str(t, 1, 20, 32) == "WPN_A");
	CHECK(weapon32(t, 1, 0) == 0 && weapon32(t, 1, 16) == 5);
	CHECK(weapon32(t, 1, 8) == (0x1 | 0x1000) && weapon32(t, 1, 12) == 0x1);
	CHECK(weapon32(t, 1, 84) == 7 && weapon32(t, 1, 88) == 0 && weapon32(t, 1, 92) == 90);
	CHECK(weapon32(t, 1, 104) == 4 && weapon32(t, 1, 96) == 0);
	CHECK(weapon32(t, 1, 176) == 98304 && weapon32(t, 1, 180) == 32768 &&
			weapon32(t, 1, 184) == 16384);
	CHECK(weapon32(t, 1, 188) == 196608 && weapon32(t, 1, 192) == 49152 &&
			weapon32(t, 1, 196) == 163840);
	CHECK(weapon32(t, 1, 204) == 32768 && weapon32(t, 1, 208) == 16384);
	CHECK(weapon_str(t, 1, 728, 16) == "VOICE");
	CHECK(weapon32(t, 1, 4) == 1); // AMMO_556's row; row 0 is "unknown round"
	CHECK(weapon32(t, 1, 164) == 2 && weapon32(t, 1, 168) == 3 && weapon32(t, 1, 172) == 4);
	CHECK(weapon_str(t, 1, 856, 16) == "smoke" && weapon32(t, 1, 884) == 98304);
	CHECK(weapon32(t, 1, 876) == (50 << 16) / 100 && weapon32(t, 1, 880) == (3 << 16) / 6200);
	CHECK(weapon32(t, 1, 124) == 0x11 && weapon32(t, 1, 128) == 2 && weapon32(t, 1, 220) == 12);
	CHECK(weapon_str(t, 1, 52, 32) == "WPN_B");
	CHECK(weapon32(t, 1, 332) == 6 && weapon32(t, 1, 336) == 32768 &&
			weapon32(t, 1, 340) == 294912);
	CHECK(weapon32(t, 1, 344) == 32768 && weapon32(t, 1, 348) == 16384 &&
			weapon32(t, 1, 352) == 196608);
	CHECK(weapon32(t, 1, 228) == 1);
	CHECK(weapon8(t, 1, 216) == 11 && weapon32(t, 1, 224) == 2); // the carry row's class id
	CHECK(weapon32(t, 1, 620) == 10 && weapon32(t, 1, 624) == 20);
	CHECK(weapon32(t, 1, 644) == 1 && weapon32(t, 1, 656) == 4 && weapon8(t, 1, 727) == 5);
	CHECK(weapon32(t, 1, 316) == 10 * 11930464 && weapon32(t, 1, 320) == 5 * -11930464);
	CHECK(weapon32(t, 1, 324) == 90 * 11930464);
	CHECK(weapon_str(t, 1, 744, 16) == "MFLASH" && weapon_str(t, 1, 760, 24) == "loop1");
	CHECK(weapon32(t, 1, 356) == 3 && weapon32(t, 1, 360) == 1);
	float fov = 0.0f;
	std::memcpy(&fov, t.bytes.data() + inmatch::kWeaponRecordSize + 328, sizeof(fov));
	CHECK(fov == 65.0f);
	CHECK(weapon32(t, 1, 1108) == 9 && weapon32(t, 1, 1112) == 124);
	CHECK(weapon32(t, 1, 600) == 1); // only the mounted image counts
	CHECK(weapon_str(t, 1, 888, 32) == "icon.tgag_name.tga"); // strncpy of strlen: no terminator
	CHECK(weapon32(t, 1, 932) == 3);
	CHECK(weapon32(t, 1, 232) == 5 && weapon32(t, 1, 236) == 6);
	CHECK(weapon32(t, 1, 936) == 1 && weapon32(t, 1, 940) == 2);
	CHECK(weapon32(t, 1, 140) == 32767);
	CHECK(weapon32(t, 1, 132) == 1 && weapon32(t, 1, 156) == 2 && weapon32(t, 1, 160) == 3);
	CHECK(weapon32(t, 1, 136) == 4);
	CHECK(weapon32(t, 1, 144) == 8 && weapon32(t, 1, 148) == 4 && weapon32(t, 1, 152) == 3);
	// The fire cycle: FIRE's `auto` delayEnd 26 - 2 = 24, RECOIL's two `auto`
	// reads off the ring's last two variants, r3 (7) then r2 (20 - 7 = 13).
	CHECK(weapon32(t, 1, 1104) == 24 + 7 + 13);
	// WPN_B names the same .adm (any extension): the shared ring serves r1 next.
	// Its new class takes the next id, and a zero count reads 1 at its end.
	CHECK(weapon32(t, 2, 1104) == 14);
	CHECK(weapon8(t, 2, 216) == 12 && weapon32(t, 2, 224) == 1);
	// `weapon null` takes a new row; a redefinition reuses its row from the
	// defaults, keeping the last block's fields.
	CHECK(weapon_str(t, 3, 20, 32) == "null");
	CHECK(weapon_str(t, 4, 20, 32) == "wpn_c" && weapon32(t, 4, 88) == 7 &&
			weapon32(t, 4, 84) == 0);
	CHECK(weapon_str(t, 5, 20, 32).empty());
}

// The sanitized whole-table CRC over an image the test builds itself: the
// defaults, the name, and every overwrite (renderfov reduced to its rounded
// half: 80 -> 40, 65 -> 33).
void test_weapon_sanitizer(const WeaponCorpus &c) {
	const AmmoRecordTable ammo = ammo_table("ammo AT_NULL\r\nend\r\n");
	const WeaponRecordTable t = weapon_table(c, ammo, "weapon WPN_Z\r\nrenderfov 65\r\nend\r\n");
	CHECK(t.live_count() == 2);
	std::vector<uint8_t> expected(2 * inmatch::kWeaponRecordSize, 0);
	for (int row = 0; row < 2; ++row) {
		uint8_t *r = expected.data() + static_cast<size_t>(row) * inmatch::kWeaponRecordSize;
		const char *name = row == 0 ? "null" : "WPN_Z";
		std::memcpy(r + 20, name, std::strlen(name));
		io::write_u32_le(r + 4, 0xFFFFFFFFu);
		io::write_u32_le(r + 88, 1);
		io::write_u32_le(r + 92, 0xFFFFFFFFu);
		io::write_u32_le(r + 152, 2);
		io::write_u32_le(r + 212, 77);
		io::write_u32_le(r + 224, 1);
		io::write_u32_le(r + 240, 22);
		for (size_t off = 244; off < 316; off += 4)
			io::write_u32_le(r + off, 0x0B0B0B0Bu);
		io::write_u32_le(r + 328, row == 0 ? 40 : 33);
		for (size_t off = 344; off < 356; off += 4)
			io::write_u32_le(r + off, 0x10000);
		for (size_t off = 376; off < 456; off += 4)
			io::write_u32_le(r + off, 0x42424242u);
		std::memset(r + 456, 99, 0x90);
		for (size_t off = 604; off < 620; off += 4)
			io::write_u32_le(r + off, 0x42424242u);
		for (size_t off = 628; off < 644; off += 4)
			io::write_u32_le(r + off, 0x42424242u);
		for (size_t off = 676; off < 724; off += 4)
			io::write_u32_le(r + off, 44);
		r[724] = r[725] = r[726] = 111;
		io::write_u32_le(r + 920, 88);
		std::memset(r + 944, 222, 0xA0);
	}
	CHECK(t.sanitized_crc() == io::crc32_mpeg2(expected.data(), expected.size()));
	// No live row: 0.
	WeaponRecordTable empty;
	CHECK(empty.sanitized_crc() == 0);
}

void test_weapon_uncomputable(const WeaponCorpus &c) {
	const AmmoRecordTable ammo = ammo_table("ammo AT_NULL\r\nend\r\n");
	// A fifth mounted `sights` row writes a texture pointer over the row count.
	const WeaponRecordTable sights = weapon_table(c, ammo,
			"weapon WPN_S\r\nsights sight.tga 1 2 3 4 a\r\nsights sight.tga 1 2 3 4 a\r\n"
			"sights sight.tga 1 2 3 4 a\r\nsights sight.tga 1 2 3 4 a\r\nend\r\n");
	CHECK(sights.computable && weapon32(sights, 1, 600) == 4);
	const WeaponRecordTable five = weapon_table(c, ammo,
			"weapon WPN_S\r\nsights sight.tga 1 2 3 4 a\r\nsights sight.tga 1 2 3 4 a\r\n"
			"sights sight.tga 1 2 3 4 a\r\nsights sight.tga 1 2 3 4 a\r\nsights sight.tga 1 2 3 4 "
			"a\r\nend\r\n");
	CHECK(!five.computable);
	// A `weapon` line inside an open block aborts the parse with the open
	// weapon's actions never bound: the post-parse sum reads through null.
	const WeaponRecordTable open =
			weapon_table(c, ammo, "weapon WPN_O\r\nclipsize 3\r\nweapon WPN_P\r\nend\r\n");
	CHECK(!open.computable && open.live_count() == 2);
}

// --- the sources -----------------------------------------------------------

void test_sources(const WeaponCorpus &c) {
	CHECK(write_text(c.dir / "items.def", "begin ITEM_ZERO\r\nid 100000\r\nend\r\n"));
	CHECK(write_text(
			c.dir / "ammo.def", "ammo AT_NULL\r\nend\r\nammo AMMO_556\r\nvelocity 3\r\nend\r\n"));
	CHECK(write_text(c.dir / "weapon.def",
			"weapon WPN_A\r\nround_type AMMO_556\r\nanimadm wpn\r\n"
			"action fire\r\nanim anim_wpn_fire\r\ndelayend auto\r\nend\r\nend\r\n"));
	ResourceIndex index;
	CHECK(index.scan(c.dir.string()));
	assets::AssetStore assets{ &index };
	const mission::BootFileSource files = mission::boot_files_from_index(index);
	const inmatch::IntegritySources s = inmatch::compute_integrity_sources(files, &assets);
	CHECK(s.ammo_known && s.ammo_row_crcs.size() == 2);
	CHECK(s.weapon_table_known && s.weapon_named_rows == 2);
	// The same table through the image builders.
	const std::vector<uint8_t> ammo_text =
			bytes_of("ammo AT_NULL\r\nend\r\nammo AMMO_556\r\nvelocity 3\r\nend\r\n");
	const std::vector<uint8_t> weapon_text =
			bytes_of("weapon WPN_A\r\nround_type AMMO_556\r\nanimadm wpn\r\n"
					 "action fire\r\nanim anim_wpn_fire\r\ndelayend auto\r\nend\r\nend\r\n");
	io::ConfigTokens tokens;
	const AmmoRecordTable ammo = inmatch::build_ammo_record_table(
			ammo_text.data(), ammo_text.size(), { { "ITEM_ZERO", 0 } }, nullptr, tokens);
	const WeaponRecordTable weapons = inmatch::build_weapon_record_table(
			weapon_text.data(), weapon_text.size(), ammo, files, &assets, tokens);
	uint32_t row1 = 0;
	CHECK(ammo.row_crc(1, row1) && s.ammo_row_crcs[1] == row1);
	CHECK(s.weapon_table_crc == weapons.sanitized_crc());
	CHECK(weapon32(weapons, 1, 1104) == 26 && weapon32(weapons, 1, 4) == 1);
	// The ammo leg reads its index as a signed char; the one-row weapon CRC is
	// 0 only past the named rows.
	uint32_t crc = 0;
	CHECK(s.ammo_row(1, crc) == inmatch::IntegritySources::AmmoRow::Crc && crc == row1);
	CHECK(s.ammo_row(2, crc) == inmatch::IntegritySources::AmmoRow::OutOfRange);
	CHECK(s.ammo_row(0xFF, crc) == inmatch::IntegritySources::AmmoRow::OutOfRange);
	CHECK(!s.weapon_row_crc(1, crc) && s.weapon_row_crc(2, crc) && crc == 0 &&
			!s.weapon_row_crc(0xFF, crc));
	// No asset store: the weapon leg cannot read its rings and stays silent.
	const inmatch::IntegritySources no_assets = inmatch::compute_integrity_sources(files, nullptr);
	CHECK(no_assets.ammo_known && !no_assets.weapon_table_known);
	CHECK(no_assets.ammo_row(1, crc) == inmatch::IntegritySources::AmmoRow::Crc);
	// No items.def (or no ammo.def): nothing is computed and every leg is silent.
	fs::remove(c.dir / "items.def");
	ResourceIndex bare;
	CHECK(bare.scan(c.dir.string()));
	const inmatch::IntegritySources none =
			inmatch::compute_integrity_sources(mission::boot_files_from_index(bare), &assets);
	CHECK(!none.ammo_known && !none.weapon_table_known);
	CHECK(none.ammo_row(0, crc) == inmatch::IntegritySources::AmmoRow::Silent);
	CHECK(!inmatch::compute_integrity_sources(mission::BootFileSource{}, &assets).ammo_known);
}

// --- the retail corpus -----------------------------------------------------

inmatch::IntegritySources compute_from_install(
		const std::string &root, const std::string &expansion) {
	LaunchFlags flags;
	flags.expansion = expansion;
	ResourceIndex index;
	if (index.scan_install(root, flags) != ResourceIndex::InstallScan::Mounted)
		return {};
	assets::AssetStore assets{ &index };
	return inmatch::compute_integrity_sources(mission::boot_files_from_index(index), &assets);
}

void test_retail_jox01() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (the pinned install with its jox01 expansion)");
		return;
	}
	// The pinned reference with its jox01 expansion, as a launch with
	// `/exp jox01` mounts it; computed twice to show the sources carry no state
	// from one mission load to the next.
	const inmatch::IntegritySources jox01 = compute_from_install(install, "jox01");
	const inmatch::IntegritySources again = compute_from_install(install, "jox01");
	CHECK(jox01.ammo_known &&
			jox01.ammo_row_crcs.size() == std::size(integrity_oracle::kJox01AmmoRowCrcs));
	if (jox01.ammo_row_crcs.size() == std::size(integrity_oracle::kJox01AmmoRowCrcs)) {
		for (size_t row = 0; row < jox01.ammo_row_crcs.size(); ++row) {
			if (jox01.ammo_row_crcs[row] != integrity_oracle::kJox01AmmoRowCrcs[row]) {
				std::printf("FAIL: jox01 ammo row 0x%02zX computed 0x%08X, witnessed 0x%08X\n", row,
						jox01.ammo_row_crcs[row], integrity_oracle::kJox01AmmoRowCrcs[row]);
				++g_failures;
			}
		}
	}
	CHECK(integrity_oracle::row_digest(jox01.ammo_row_crcs.data(), jox01.ammo_row_crcs.size()) ==
			integrity_oracle::kJox01RowDigest);
	CHECK(jox01.weapon_table_known &&
			jox01.weapon_table_crc == integrity_oracle::kJox01WeaponTableCrc);
	CHECK(again.ammo_row_crcs == jox01.ammo_row_crcs &&
			again.weapon_table_crc == jox01.weapon_table_crc);
	std::printf("jox01: %zu ammo rows, weapon table 0x%08X over %d named rows\n",
			jox01.ammo_row_crcs.size(), jox01.weapon_table_crc, jox01.weapon_named_rows);
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	test_item_rows();
	test_ammo_fields();
	test_ammo_tokenizer_quirks();
	test_ammo_capacity_and_abort();
	test_ammo_effects_tables();
	test_ammo_notarmmed_overflow();
	WeaponCorpus corpus;
	CHECK(make_weapon_corpus(corpus));
	test_weapon_fields(corpus);
	test_weapon_sanitizer(corpus);
	test_weapon_uncomputable(corpus);
	test_sources(corpus);
	std::error_code ignored;
	fs::remove_all(corpus.dir, ignored);
	if (retail::selected())
		test_retail_jox01();
	if (g_failures != 0)
		std::printf("%d failure(s)\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
