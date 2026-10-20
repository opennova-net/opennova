#include <editor/preview/preview_held_weapon.h>

#include <cmath>
#include <cstdio>
#include <vector>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <editor/documents/animation_slots.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/preview_clip_sounds.h>
#include <formats/def/def.h>
#include <runtime/anim/adm_clip_index.h>
#include <runtime/anim/anim_slot_names.h>
#include <runtime/inmatch/client_replica_present.h>
#include <runtime/mission/placement_traits.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity_pose.h>
#include <runtime/world/infantry.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/weapon_table_build.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;
namespace as = world::anim_state;

// The catalog the mission load reads the weapons from.
constexpr const char *kWeaponCatalog = "weapon.def";

// The body stands in the preview's frame unturned: the placement the identity, which is mission yaw
// 180 (mission::bms_to_presentation_basis(0, 180, 0)).
constexpr double kPreviewBodyYawDeg = 180.0;

// A row's key and slot in words: "row 50 (anim_knife)".
std::string row_words(int slot) {
	if (slot < 0 || slot >= anim::kAnimSlotCount)
		return "no row";
	return "row " + std::to_string(slot) + " (anim_" + anim::kAnimSlotNames[slot] + ")";
}

// The weapon channel's own rows: the hold ladder's poses, the attacks, the binoculars, the reloads.
// The body's primary channel never plays them; every other body row is one the default hold
// mirrors.
bool weapon_channel_row(int slot) {
	return slot >= as::kHoldKnife && slot <= as::kReload2;
}

// The death rows: the death selector's states (world::compute_death_anim_state), a dead body's.
bool death_row(int slot) {
	return slot >= as::kDeathFire && slot < kAnimBodySlotCount;
}

// How `entry` reaches row `slot`, and the weapon channel's hold state with it there (`state`),
// through the game's own selectors: the hold ladder [orig: Entity_UpdateInfantryPlayerBody
// @0x4b5dad..0x4b5e6f], the fire stamp [orig: WeaponAction_Fire @0x542bbc..0x542bea], the reload
// window [orig: WeaponSlot_ReloadAmmo @0x54173c] and the binoculars' override.
HeldRoute held_route(const HeldWeaponChoice &weapon, int slot, int &state) {
	const int hold = weapon.hold;
	if (weapon_channel_row(slot)) {
		// A weapon-channel row: the arms show it whatever reaches it.
		state = slot;
		for (const bool scoped : { false, true })
			if (world::infantry_weapon_hold_state(hold, as::kIdle, scoped, false, false) == slot)
				return HeldRoute::Hold;
		world::InfantryState stamped;
		stamped.wpn_state = -1;
		world::infantry_weapon_attack_stamp(stamped, weapon.attack);
		if (stamped.wpn_state == slot)
			return HeldRoute::Attack;
		if (weapon.clip &&
				world::infantry_weapon_hold_state(hold, as::kIdle, false, false, true) == slot)
			return HeldRoute::Reload;
		if (world::infantry_weapon_hold_state(hold, as::kIdle, false, true, false) == slot)
			return HeldRoute::Binoculars;
		return HeldRoute::None;
	}
	// A body row: the ladder over it as the body's primary state. A hold the default branch takes
	// mirrors it (the upper body plays the row itself); any other plays its own hold row on the
	// upper body.
	state = world::infantry_weapon_hold_state(hold, slot, false, false, false);
	return state == world::infantry_weapon_hold_state(0, slot, false, false, false)
			? HeldRoute::Mirror
			: HeldRoute::Carried;
}

// Whether Auto may draw `route` for the row: one that reaches a weapon-channel row, or a hold
// mirroring a body row.
bool auto_pairs(HeldRoute route) {
	return route != HeldRoute::None && route != HeldRoute::Carried;
}

// How a weapon reaches the row, in words.
std::string route_words(const HeldWeaponChoice &weapon, int slot) {
	switch (weapon.route) {
		case HeldRoute::Hold:
			return "its special_hold " + std::to_string(weapon.hold) + " holds it in " +
					row_words(slot) +
					(world::infantry_weapon_hold_state(
							 weapon.hold, as::kIdle, false, false, false) == slot
									? std::string()
									: std::string(", the scope raised"));
		case HeldRoute::Attack:
			return "its attack_anim " + std::to_string(weapon.attack) + " plays " +
					row_words(slot) + " as it fires";
		case HeldRoute::Reload:
			return "its clip refills in " + row_words(slot) + ", the reload window";
		case HeldRoute::Binoculars:
			return row_words(slot) + " plays over it while the binoculars are raised";
		case HeldRoute::Mirror:
			return "its special_hold " + std::to_string(weapon.hold) +
					" mirrors the body, so the upper body plays " + row_words(slot) + " itself";
		case HeldRoute::Carried:
			return "it is carried through " + row_words(slot) + " while its special_hold " +
					std::to_string(weapon.hold) + " plays " + row_words(weapon.state) +
					" on the upper body";
		case HeldRoute::None:
			break;
	}
	return "the game never plays " + row_words(slot) + " with it";
}

// A preview point turned by `joint`'s deform (a direction: no translation).
PreviewVec3 turn(const PreviewJoint &joint, const PreviewVec3 &v) {
	return preview_joint_carry(joint, v, true);
}

} // namespace

// --- the options -------------------------------------------------------------------------------

io::JsonValue held_weapon_options_to_json(const HeldWeaponOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("weapon", json_string(options.weapon));
	return out;
}

bool read_held_weapon_options(
		const io::JsonValue &json, HeldWeaponOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options.held is an object {weapon}.";
		return false;
	}
	HeldWeaponOptions options = held;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "weapon") {
			if (!member.value.is_string()) {
				error = "options.held.weapon is a weapon's name (\"\" the first of weapon.def that "
						"plays the row, \"none\" none).";
				return false;
			}
			options.weapon = strutil::iequals(member.value.string, kHeldWeaponNone)
					? std::string(kHeldWeaponNone)
					: member.value.string;
		} else {
			error = "Unknown held-weapon option \"" + member.key + "\" (it takes weapon).";
			return false;
		}
	}
	held = options;
	return true;
}

// --- the sources -------------------------------------------------------------------------------

void HeldWeapon::clear() {
	const uint64_t serial = serial_ + (model_ ? 1 : 0);
	const uint64_t reads = reads_count_;
	*this = HeldWeapon();
	serial_ = serial;
	reads_count_ = reads;
}

void HeldWeapon::read_table_(const std::shared_ptr<const FileSource> &files) {
	++reads_count_;
	table_known_ = true;
	table_.clear();
	// The project's files as the game's mission load reads them (mission::read_weapon_defs):
	// weapon.def through the game's parser, then the table the load builds
	// (world::build_weapon_table), each gfx3 loaded through the game's model store over the same
	// files; every file read noted with its stamp, weapon.def's even where it is missing.
	auto stamped = std::make_shared<StampedFiles>(files);
	stamped->stamp(kWeaponCatalog);
	ResourceIndex index;
	index.mount_source(stamped);
	assets::AssetStore store(&index);
	mission::BootFileSource boot;
	boot.has_file = [&index](const std::string &name) { return index.has_file(name); };
	boot.read_file = [&stamped](const std::string &name, std::vector<uint8_t> &out) {
		return stamped->read(name, out);
	};
	def::DefWeaponsFile weapons{};
	if (mission::read_weapon_defs(boot, kWeaponCatalog, weapons) == mission::DefTableRead::Read) {
		const world::WeaponTable table = world::build_weapon_table(weapons, &store);
		// Index 0 is the engine's reserved "null" def; the file's weapons follow from 1
		// [orig: AnimDef_InitAll @0x5435c0].
		for (size_t i = 1; i < table.entries.size() && i < world::kAdmSlotNone; ++i) {
			const world::WeaponTableEntry &row = table.entries[i];
			if (!row.valid)
				continue;
			Entry entry;
			entry.choice.name = row.name;
			entry.choice.adm = int(i);
			entry.choice.hold = row.special_hold;
			entry.choice.attack = row.attack_anim;
			entry.choice.clip = row.clipsize > 0;
			entry.choice.gfx3 = row.third_person_model;
			entry.model = row.third_person_model_asset;
			table_.push_back(std::move(entry));
		}
	}
	def::def_free_weapons(&weapons);
	table_read_ = stamped->stamps();
}

bool HeldWeapon::refresh(const std::shared_ptr<const FileSource> &files, const AssetScan *scan,
		const AssetGraph *graph, const ClipSoundItem &item, const std::string &clip_key,
		size_t rig_bones, bool first_person, const HeldWeaponOptions &options) {
	const int slot = clip_key.empty() ? -1 : anim::adm_slot_index(clip_key);
	// A body's row of a person's map: a weapon's map (first person), a lone clip, a first-person
	// slot and a rig of no bone 16 (no person's: the game hangs the weapon there) hold none.
	if (!files || first_person || slot < 0 || slot >= kAnimBodySlotCount ||
			rig_bones <= size_t(world::kHeldWeaponBoneIndex)) {
		const bool was = active_ || model_;
		clear();
		return was;
	}
	// The state said before, to tell what moved.
	const std::string said = words_ + '\n' + note_ + '\n' + model_path_ + '\n' + record_.locator;
	const assets::Model was_model = model_;
	const bool was_hand = hand_frame_;
	const float was_attach[3] = { attach_deg_[0], attach_deg_[1], attach_deg_[2] };
	const size_t was_drawn = drawn_;
	const bool was_active = active_;
	const int was_row = row_;
	bool moved = !table_known_ || table_read_.moved(*files);
	if (moved)
		read_table_(files);
	active_ = true;
	row_ = slot;
	// Only the player class sets a body's held weapon [orig: PlayerClass_InitEntity @0x4b1060]; an
	// org0/org1 body never writes +0x2B0 on foot (world-wac-ai-re.md 13.5).
	player_ = item.found && strutil::iequals(item.ai_function, "plyr");
	choices_.clear();
	for (const Entry &entry : table_) {
		HeldWeaponChoice choice = entry.choice;
		choice.route = held_route(choice, slot, choice.state);
		choices_.push_back(std::move(choice));
	}
	// The weapon drawn: the one the options name (the game's lookup by name [orig:
	// AdmDef_FindIndexByName @0x53FD80]), else the first that plays the row on a player's body.
	drawn_ = SIZE_MAX;
	named_ = false;
	note_.clear();
	words_.clear();
	const bool none = options.weapon == kHeldWeaponNone;
	if (!none && !options.weapon.empty()) {
		for (size_t i = 0; i < choices_.size() && drawn_ == SIZE_MAX; ++i)
			if (strutil::iequals(choices_[i].name, options.weapon))
				drawn_ = i;
		named_ = drawn_ != SIZE_MAX;
		if (!named_)
			note_ = options.weapon + " is not in weapon.def: ";
	}
	const std::string row = row_words(slot);
	if (none) {
		words_ = "No weapon is held: the options ask for none.";
	} else if (!named_ && !player_) {
		const std::string function = item.ai_function.empty() ? "none" : item.ai_function;
		words_ = item.found ? item.name + "'s class (ai_function " + function +
						") never sets a held weapon, only the player's does: the game draws none "
						"in its hands."
							: "No item pairs the map with a class, and only the player's sets a "
							  "held weapon: none draws.";
		note_ += "Pick a weapon to see one held: an NPC holds one only on a vehicle's seat 1.";
	} else if (!named_) {
		for (size_t i = 0; i < choices_.size() && drawn_ == SIZE_MAX; ++i)
			if (auto_pairs(choices_[i].route))
				drawn_ = i;
		if (drawn_ == SIZE_MAX) {
			words_ = "No weapon of weapon.def plays " + row + ": the game draws none with it.";
			note_ += "Pick a weapon to see one held.";
		}
	}
	// What the game's present row says of it (inmatch::write_present_held_weapon): drawn on a live
	// body standing in the preview's frame, its aim level, the attach triple and the frame by the
	// hold state's flags.
	attach_deg_[0] = attach_deg_[1] = attach_deg_[2] = 0.0f;
	hand_frame_ = false;
	model_.reset();
	model_file_.clear();
	model_path_.clear();
	record_ = ReferenceTarget();
	if (const HeldWeaponChoice *weapon = drawn()) {
		anim::AimOverlayInputs in;
		in.aim_yaw = world::bam_heading_from_mission_yaw_deg(kPreviewBodyYawDeg);
		in.aim_state = (world::infantry_anim_flags(slot) & 0x40u) != 0;
		in.rolling = slot == as::kRollLeft || slot == as::kRollRight;
		std::vector<float> present(size_t(world::PF_STRIDE), 0.0f);
		inmatch::write_present_held_weapon(
				present.data(), uint8_t(weapon->adm), death_row(slot), in, weapon->state);
		const bool shown = present[world::PF_HELD_WEAPON_ADM] != 0.0f;
		attach_deg_[0] = present[world::PF_HELD_WEAPON_PITCH_DEG];
		attach_deg_[1] = present[world::PF_HELD_WEAPON_YAW_DEG];
		attach_deg_[2] = present[world::PF_HELD_WEAPON_ROLL_DEG];
		hand_frame_ = present[world::PF_HELD_WEAPON_HAND_FRAME] != 0.0f;
		// Its record, where its links go: the last block of the name in the weapon.def the game
		// reads (the one it keeps), else the last the graph defines anywhere.
		if (graph && scan) {
			const AssetEntry *catalog = scan->find(kWeaponCatalog);
			const GraphSymbol *found = nullptr, *anywhere = nullptr;
			for (const GraphSymbol *symbol : graph->symbols_of_kind(ReferenceKind::Weapon)) {
				if (!strutil::iequals(symbol->display, weapon->name) &&
						!strutil::iequals(symbol->name, weapon->name))
					continue;
				anywhere = symbol;
				if (catalog && symbol->file == catalog->relative_path)
					found = symbol;
			}
			if (found || anywhere)
				record_ = symbol_target(*scan, found ? *found : *anywhere);
		}
		// Its model as the game's store loaded its gfx3, and the project's file of it.
		const Entry &entry = table_[drawn_];
		const std::string name = weapon->gfx3.empty()
				? std::string()
				: assets::asset_file_name(weapon->gfx3, ".3di", true);
		const AssetEntry *file = scan && !name.empty() ? scan->find(name) : nullptr;
		const std::string how = route_words(*weapon, slot);
		const std::string frame = hand_frame_
				? "turned with the hand (the hand frame: anim_" +
						std::string(anim::kAnimSlotNames[weapon->state]) +
						" carries anim flag 0x80)"
				: "turned with the body (the entity frame: the aim's yaw and pitch)";
		if (!shown) {
			words_ = weapon->name + " is not drawn: " + row +
					" is a death, and a dead body draws no weapon.";
		} else if (weapon->gfx3.empty()) {
			words_ = weapon->name + " names no gfx3: the game draws nothing in the hand.";
		} else if (!entry.model) {
			words_ = weapon->name + "'s gfx3 " + weapon->gfx3 +
					" is drawn in the hand, but the project has no model of it" +
					(file ? " that reads" : "") + ": the game draws nothing there.";
		} else {
			model_ = entry.model;
			model_file_ = file ? file->logical_name : name;
			model_path_ = file ? file->relative_path : std::string();
			words_ = weapon->name + " (" +
					std::string(named_ ? "chosen" : "the first that plays the row") + ") holds " +
					model_file_ + " in the right hand: " + how + ". It hangs from bone " +
					std::to_string(world::kHeldWeaponBoneIndex) + " as the game hangs it, " +
					frame + ".";
		}
		if (named_ && !auto_pairs(weapon->route))
			note_ += weapon->name + ": " + how + ".";
	}
	if (!none && !named_ && !options.weapon.empty() && drawn())
		note_ += drawn()->name + " shows.";
	if (model_.get() != was_model.get())
		++serial_;
	const std::string now = words_ + '\n' + note_ + '\n' + model_path_ + '\n' + record_.locator;
	return moved || !was_active || was_row != row_ || was_drawn != drawn_ ||
			model_.get() != was_model.get() || was_hand != hand_frame_ ||
			was_attach[0] != attach_deg_[0] || was_attach[1] != attach_deg_[1] ||
			was_attach[2] != attach_deg_[2] || said != now;
}

bool HeldWeapon::place(const std::vector<PreviewJoint> &joints, HeldWeaponPlace &out) const {
	const size_t bone = size_t(world::kHeldWeaponBoneIndex);
	if (!model_ || joints.size() <= bone)
		return false;
	const PreviewJoint &hand = joints[bone];
	out.point = hand.name;
	// Bone 16's joint, the nudge carried by the hand's model-to-world turn (its deform) [orig: the
	// translation overwrite @0x4b22cf..0x4b22f8]; the nudge is the render frame's, which the
	// preview's space is.
	const PreviewVec3 nudge{ world::kHeldWeaponAttachNudgeX, world::kHeldWeaponAttachNudgeY,
		world::kHeldWeaponAttachNudgeZ };
	const PreviewVec3 carried = turn(hand, nudge);
	out.at = PreviewVec3{ hand.at.x + carried.x, hand.at.y + carried.y, hand.at.z + carried.z };
	if (hand_frame_) {
		// The hand frame: the hand's turn times Rz then Ry of the calibration [orig: Rz
		// @0x4b2215..0x4b2251, Ry
		// @0x4b2256..0x4b22c2], each a rotation by its authored angle
		// (EntityPresenter::held_weapon_hand_frame_basis).
		const double z = world::kHeldWeaponHandFrameZRad;
		const double y = world::kHeldWeaponHandFrameYRad;
		const PreviewVec3 muzzle{ float(std::sin(y) * std::cos(z)),
			float(std::sin(y) * std::sin(z)), float(std::cos(y)) };
		const PreviewVec3 up{ float(-std::sin(z)), float(std::cos(z)), 0.0f };
		out.muzzle = turn(hand, muzzle);
		out.up = turn(hand, up);
	} else {
		// The entity frame: the attach triple placed as a placed model is (its columns the axes'
		// images).
		const mission::PlacementBasis basis =
				mission::bms_to_presentation_basis(attach_deg_[0], attach_deg_[1], attach_deg_[2]);
		out.muzzle = PreviewVec3{ basis.z.x, basis.z.y, basis.z.z };
		out.up = PreviewVec3{ basis.y.x, basis.y.y, basis.y.z };
	}
	return true;
}

// --- the envelope ------------------------------------------------------------------------------

io::JsonValue held_weapon_json(const HeldWeapon &held, const HeldWeaponOptions &options,
		const std::vector<PreviewJoint> &joints, const OrbitCamera &camera, int width, int height) {
	if (!held.active())
		return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	const HeldWeaponChoice *weapon = held.drawn();
	out.set("weapon", json_string(weapon ? weapon->name : std::string()));
	out.set("chosen", json_string(options.weapon));
	out.set("named", JsonValue::make_bool(held.named()));
	out.set("player", JsonValue::make_bool(held.player()));
	out.set("row", json_number(held.row()));
	out.set("adm", json_number(weapon ? weapon->adm : 0));
	out.set("gfx3", json_string(weapon ? weapon->gfx3 : std::string()));
	out.set("model", json_string(held.model_file()));
	out.set("model_path", json_string(held.model_path()));
	out.set("drawn", JsonValue::make_bool(held.model() != nullptr));
	JsonValue record = JsonValue::make_null();
	if (!held.record().file.empty()) {
		record = JsonValue::make_object();
		record.set("file", json_string(held.record().file));
		record.set("locator", json_string(held.record().locator));
		record.set("field", json_string(held.record().field));
	}
	out.set("record", std::move(record));
	out.set("hold", json_number(weapon ? weapon->hold : 0));
	out.set("attack", json_number(weapon ? weapon->attack : 0));
	out.set("route",
			json_string(kHeldRouteTokens[size_t(weapon ? weapon->route : HeldRoute::None)]));
	out.set("state", json_number(weapon ? weapon->state : -1));
	out.set("frame", json_string(held.hand_frame() ? "hand" : "entity"));
	JsonValue attach = JsonValue::make_array();
	for (int i = 0; i < 3; ++i)
		attach.push(json_number(held.attach_deg()[i]));
	out.set("attach", std::move(attach));
	out.set("bone", json_number(world::kHeldWeaponBoneIndex));
	HeldWeaponPlace place;
	const bool placed = held.place(joints, place);
	out.set("point", json_string(placed ? place.point : std::string()));
	out.set("position", placed ? preview_point_json(place.at) : JsonValue::make_null());
	out.set("muzzle", placed ? preview_point_json(place.muzzle) : JsonValue::make_null());
	out.set("up", placed ? preview_point_json(place.up) : JsonValue::make_null());
	float x = 0.0f, y = 0.0f;
	if (placed && camera.project(place.at, width, height, x, y)) {
		JsonValue screen = JsonValue::make_array();
		screen.push(json_number(x));
		screen.push(json_number(y));
		out.set("screen", std::move(screen));
	} else {
		out.set("screen", JsonValue::make_null());
	}
	out.set("words", json_string(held.words()));
	out.set("note", json_string(held.note()));
	JsonValue choices = JsonValue::make_array();
	for (const HeldWeaponChoice &choice : held.choices()) {
		JsonValue row = JsonValue::make_object();
		row.set("weapon", json_string(choice.name));
		row.set("adm", json_number(choice.adm));
		row.set("gfx3", json_string(choice.gfx3));
		row.set("hold", json_number(choice.hold));
		row.set("attack", json_number(choice.attack));
		row.set("route", json_string(kHeldRouteTokens[size_t(choice.route)]));
		row.set("state", json_number(choice.state));
		choices.push(std::move(row));
	}
	out.set("choices", std::move(choices));
	return out;
}

} // namespace opennova::editor
