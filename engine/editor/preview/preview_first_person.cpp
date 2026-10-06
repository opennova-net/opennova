#include <editor/preview/preview_first_person.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/avatars/avatars.h>
#include <formats/def/def.h>
#include <runtime/anim/adm_clip_index.h>
#include <runtime/anim/adm_fallback.h>
#include <runtime/inmatch/character_registry.h>
#include <runtime/world/player_view.h>
#include <runtime/world/weapon_table_build.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
namespace wa = world::weapon_action;

std::string file_of(const std::string &path) { return path.substr(path.find_last_of("/\\") + 1); }

std::string upper(const std::string &text) {
	std::string out = text;
	for (char &c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
	return out;
}

// How long a run of an action is followed: a minute of the game's ticks.
constexpr int32_t kRunTicks = 62 * 60;

const char *handler_name(const world::WeaponFsmAction &desc, int32_t action) {
	const int8_t handler = desc.handler == world::weapon_handler::kPlaceholder ? world::weapon_action_default_handler(action)
	                                                                         : desc.handler;
	return handler >= 0 && handler < world::weapon_handler::kCount ? world::kWeaponHandlerNames[handler] : "null";
}

// A slot of the anim table a key names (its tail past the first five characters); -1 none.
int slot_of(const std::string &key) { return key.empty() ? -1 : anim::adm_slot_index(key); }

// What a run comes to, in words.
std::string run_words(const WeaponActionRun &run) {
	const std::string name = upper(run.suffix);
	std::string out = name + " runs " + run.handler + " (delaystart " + std::to_string(run.delay_start) + ", delayend " +
	                  std::to_string(run.delay_end) + ")";
	if (run.anim_key.empty()) out += " and plays no clip";
	const std::string begins = run.anim_key.empty() ? ": its active phase begins" : ": its clip plays as its active phase begins";
	if (!run.begins) {
		out += ": its entry " + std::string(run.anim_key.empty() ? "plays nothing" : "plays its clip") +
		       " without beginning its active phase, so neither its soundset nor its soundsetend plays";
	} else if (run.finish < 0) {
		out += begins + ", and it never finishes that phase (its soundsetend never plays)";
	} else if (run.finish == 0) {
		out += begins + ", and it finishes on that tick";
	} else {
		out += begins + ", and it finishes " + std::to_string(run.finish) + " ticks on";
	}
	if (!run.anim_key.empty() && run.stepped >= 0)
		out += "; the game steps the clip " + std::to_string(run.stepped) + " tick" + (run.stepped == 1 ? "" : "s") +
		       " while it runs";
	out += run.next.empty() ? ", and it runs until another action is asked for." : ", then " + upper(run.next) + ".";
	return out;
}

} // namespace

// --- the options -------------------------------------------------------------------------------------

io::JsonValue first_person_options_to_json(const FirstPersonOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("view", json_string(options.eye ? "eye" : "orbit"));
	out.set("character", json_number(options.character));
	out.set("action", json_string(options.action));
	return out;
}

bool read_first_person_options(const io::JsonValue &json, FirstPersonOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options.first_person is an object {view, character, action}.";
		return false;
	}
	FirstPersonOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "view") {
			if (!value.is_string() || (value.string != "eye" && value.string != "orbit")) {
				error = "options.first_person.view is eye (the first-person eye) or orbit.";
				return false;
			}
			options.eye = value.string == "eye";
		} else if (key == "character") {
			int64_t id = 0;
			if (!io::json_whole_in(value, -1.0, 65535.0, id)) {
				error = "options.first_person.character is a character's id, 0 to 65535 (-1 the one a fresh profile seeds).";
				return false;
			}
			options.character = int32_t(id);
		} else if (key == "action") {
			bool known = value.is_string() && value.string.empty();
			for (int32_t i = 0; value.is_string() && !known && i < wa::kCount; ++i)
				known = strutil::iequals(value.string, world::kWeaponActionSuffixes[i]);
			if (!known) {
				error = "options.first_person.action is an action's suffix (idle, fire, reload, ...; \"\" the first "
				        "playing the row).";
				return false;
			}
			options.action = strutil::to_lower(value.string);
		} else {
			error = "Unknown first-person option \"" + key + "\" (it takes view, character, action).";
			return false;
		}
	}
	held = options;
	return true;
}

// --- an action's run -----------------------------------------------------------------------------------

WeaponActionRun weapon_action_run(const world::WeaponFsmDef &def, int32_t action) {
	WeaponActionRun run;
	if (action < 0 || action >= wa::kCount) return run;
	const world::WeaponFsmAction &desc = def.actions[action];
	run.action = action;
	run.suffix = world::kWeaponActionSuffixes[action];
	run.anim_key = desc.has_anim ? desc.anim_key : "";
	run.handler = handler_name(desc, action);
	run.delay_start = desc.delay_start;
	run.delay_end = desc.delay_end;
	// The slot done on another action with this one queued, as a request writer leaves it: the pump's
	// transition enters it on the next tick [orig: WeaponAction_ProcessFrame @0x540e60, the transition
	// @0x5413d0]; a full clip and rounds to spare, so FIRE's check passes [orig: WeaponSlot_CanFire
	// @0x541ba0].
	world::WeaponSlotState slot;
	const int32_t rounds = def.clip_capacity > 0 ? def.clip_capacity : 1;
	slot.clip = rounds;
	slot.reserve = rounds * 4;
	slot.current = action == wa::kIdle ? wa::kRecoil : wa::kIdle;
	slot.prev = wa::kIdle;
	slot.next = action;
	slot.phase = world::weapon_phase::kDone;
	world::WeaponFsmInputs in;
	bool entered = false;
	bool playing = false; // its clip is the one the channel plays
	int32_t clip_start = -1;
	int32_t begin = -1;
	int32_t finish = -1;
	for (int32_t tick = 0; tick < kRunTicks; ++tick) {
		in.current_tick = tick;
		world::WeaponFsmEvents events;
		world::weapon_fsm_tick(def, slot, in, events);
		const bool current = slot.current == action;
		if (current) entered = true;
		if (events.play_anim) {
			playing = current && !run.anim_key.empty() && slot_of(events.anim_key) == slot_of(run.anim_key);
			if (playing && clip_start < 0) clip_start = tick;
		}
		if (events.action_started == action && begin < 0) begin = tick;
		if (events.advance_anim && playing && current) ++run.stepped;
		if (events.action_finished == action && finish < 0) finish = tick;
		if (entered && !current) {
			run.next = world::kWeaponActionSuffixes[slot.current];
			break;
		}
	}
	if (run.next.empty()) run.stepped = -1; // it never leaves: the channel steps it as long as it runs
	run.begins = begin >= 0;
	run.finish = begin >= 0 && finish >= 0 ? finish - begin : -1;
	// The legs, each at its tick of the clip (the clip starts as the phase begins; one with no clip counts
	// from the begin).
	const int32_t zero = clip_start >= 0 ? clip_start : begin;
	if (desc.soundset[0] != '\0') {
		WeaponActionLeg leg;
		leg.set = desc.soundset;
		leg.tick = begin >= 0 ? begin - zero : -1;
		leg.words = leg.tick >= 0 ? leg.set + " plays as " + upper(run.suffix) + " begins (its soundset), tick " +
		                                    std::to_string(leg.tick) + " of its clip."
		                          : leg.set + " (its soundset) never plays: " + upper(run.suffix) +
		                                    "'s entry never begins its active phase.";
		run.legs.push_back(std::move(leg));
	}
	if (desc.soundsetend[0] != '\0') {
		WeaponActionLeg leg;
		leg.end = true;
		leg.set = desc.soundsetend;
		leg.tick = begin >= 0 && finish >= 0 ? finish - zero : -1;
		leg.words = leg.tick >= 0 ? leg.set + " plays as " + upper(run.suffix) + " finishes (its soundsetend), tick " +
		                                    std::to_string(leg.tick) + " of its clip."
		                          : leg.set + " (its soundsetend) never plays: " + upper(run.suffix) + " runs " +
		                                    run.handler + ", which never finishes its active phase.";
		run.legs.push_back(std::move(leg));
	}
	run.words = run_words(run);
	return run;
}

// --- the sources ------------------------------------------------------------------------------------

const FirstPersonCharacter *FirstPersonSources::character() const {
	return character_ < characters_.size() ? &characters_[character_] : nullptr;
}

int FirstPersonSources::team() const {
	const FirstPersonCharacter *who = character();
	return who && who->alignment == avatars::AVATAR_ALIGN_EVIL ? 2 : 1;
}

std::vector<const WeaponActionRun *> FirstPersonSources::actions_playing(const std::string &clip_key) const {
	std::vector<const WeaponActionRun *> out;
	const int slot = slot_of(clip_key);
	if (slot < 0) return out;
	for (const WeaponActionRun &run : actions_)
		if (slot_of(run.anim_key) == slot) out.push_back(&run);
	return out;
}

void FirstPersonSources::clear() {
	const uint64_t serial = arms_serial_ + (arms_model_ ? 1 : 0);
	*this = FirstPersonSources();
	arms_serial_ = serial;
}

void FirstPersonSources::read_weapon_(const FileSource &files, const std::shared_ptr<const FileSource> &source,
                                      const PreviewRig &rig) {
	weapon_read_ = true;
	weapon_.clear();
	gfx1_.clear();
	animadm_.clear();
	flags_ = 0;
	std::fill(std::begin(pos_units_), std::end(pos_units_), 0.0f);
	std::fill(std::begin(rot_bias_deg_), std::end(rot_bias_deg_), 0.0f);
	std::fill(std::begin(tpos_units_), std::end(tpos_units_), 0.0f);
	renderfov_ = renderer::kWeaponRenderFovHDegDefault;
	fsm_ = world::WeaponFsmDef();
	baked_ = false;
	actions_.clear();
	bake_read_.clear();
	std::vector<uint8_t> bytes;
	if (!files.read(catalog_, bytes) || bytes.empty()) return;
	def::DefWeaponsFile file{};
	if (def::def_parse_weapons_memory(bytes.data(), bytes.size(), &file) != 0) {
		def::def_free_weapons(&file);
		return;
	}
	// The weapon of the record's name: a later block of a name replaces an earlier one [orig:
	// WeaponDefs_ParseLineCallback @0x5436e1], so the last of the name is the one the game keeps.
	const def::DefWeaponDef *found = nullptr;
	for (size_t i = 0; i < file.count; ++i)
		if (strutil::iequals(file.entries[i].weapon_name, rig.record)) found = &file.entries[i];
	if (found) {
		const def::DefWeaponDef &row = *found;
		weapon_ = row.weapon_name;
		gfx1_ = row.gfx1;
		animadm_ = row.animadm;
		flags_ = uint32_t(row.flags);
		renderfov_ = row.renderfov;
		for (int i = 0; i < 3; ++i) {
			pos_units_[i] = row.pos[i];
			tpos_units_[i] = row.tpos[i];
			// The cant's columns as the game presenter reads them: Q16 degrees [orig: the parser's
			// degrees -> BAM @0x54471f].
			rot_bias_deg_[i] = float(row.pos_rotation_deg_q16[i] / io::kFp16OneD);
		}
		// The bake, as the game's weapon table bakes this weapon: every weapon on its map before it, in the
		// file's order, reads the map's shared rings first (an earlier weapon's `auto` reads move a later
		// one's heads) [orig: AnimMap_LoadAdmFile @0x40CC40, the cached entry @0x40CD45..0x40CD5C;
		// Anim_InitActions @0x541fa0]; the map and its clips read from the project's files.
		auto stamped = std::make_shared<StampedFiles>(source);
		ResourceIndex index;
		index.mount_source(stamped);
		const auto on_map = [&](const def::DefWeaponDef &other) {
			std::string a = other.animadm, b = row.animadm;
			const auto fallback = [&](std::string &name) {
				if (name.empty()) return;
				std::string base = name;
				const size_t dot = base.find_last_of('.');
				if (dot != std::string::npos) base.resize(dot);
				name = anim::adm_name_or_default(name, index.has_file(base + ".adm"));
			};
			fallback(a);
			fallback(b);
			return strutil::iequals(a, b);
		};
		std::vector<def::DefWeaponDef> rows;
		for (size_t i = 0; i < file.count; ++i) {
			if (&file.entries[i] != found && !on_map(file.entries[i])) continue;
			rows.push_back(file.entries[i]);
			if (&file.entries[i] == found) break;
		}
		def::DefWeaponsFile subset = file;
		subset.entries = rows.data();
		subset.count = rows.size();
		assets::AssetStore store(&index);
		const world::WeaponTable table = world::build_weapon_table(subset, &store);
		const int entry = table.index_of(weapon_.c_str());
		if (entry >= 0) {
			fsm_ = table.entries[size_t(entry)].action_fsm;
			baked_ = true;
		}
		bake_read_ = stamped->stamps();
		for (int32_t action = 0; baked_ && action < wa::kCount; ++action)
			actions_.push_back(weapon_action_run(fsm_, action));
	}
	def::def_free_weapons(&file);
}

void FirstPersonSources::read_characters_(const FileSource &files) {
	avatars_read_ = true;
	characters_.clear();
	seeded_ = 0;
	std::vector<uint8_t> bytes;
	if (!files.read("Avatars.def", bytes) || bytes.empty()) return;
	avatars::AvatarsFile file{};
	if (avatars::avatars_parse_memory(bytes.data(), bytes.size(), &file) == 0) {
		const inmatch::CharacterRegistry registry = inmatch::CharacterRegistry::from_file(file);
		for (const inmatch::CharacterEntry &entry : registry.entries()) {
			const avatars::AvatarNationality &nationality = file.nationalities[entry.nationality_index];
			const avatars::AvatarDivision &division = nationality.divisions[entry.division_index];
			const avatars::AvatarCombo &combo = division.combos[entry.combo_index];
			FirstPersonCharacter character;
			character.id = entry.packed_id;
			character.alignment = entry.alignment;
			character.words = std::string(nationality.name_key) + ", " + division.name_key + ", combo " +
			                  std::to_string(entry.combo_id);
			if (combo.has_arms) {
				character.arms = combo.arms.graphic;
				for (int i = 0; i < 3; ++i) character.camo[i] = combo.arms.camo[i];
			}
			characters_.push_back(std::move(character));
		}
		// A fresh profile's character: the first of the good side [orig: EntitySlot_LookupAndPackEntry
		// @0x57ad40, the seed PlayerSession_InitFromProfile @0x50cada].
		seeded_ = registry.first_character_id(avatars::AVATAR_ALIGN_GOOD);
	}
	avatars::avatars_free(&file);
}

bool FirstPersonSources::read_arms_(const FileSource &files, const AssetScan *scan) {
	// The arms graphic as the game's model store names its file: its base name with .3di.
	std::string file;
	if (spec_.show_arms && scan) {
		std::string base = file_of(spec_.arms);
		const size_t dot = base.find_last_of('.');
		if (dot != std::string::npos) base.resize(dot);
		const AssetEntry *entry = scan->find(base + ".3di");
		if (entry && entry->kind == AssetKind::Model) file = entry->logical_name;
	}
	const uint64_t stamp = file.empty() ? 0 : files.stamp(file);
	if (file == arms_file_ && stamp == arms_stamp_ && arms_read_) return false;
	arms_file_ = file;
	arms_stamp_ = stamp;
	arms_read_ = true;
	arms_model_.reset();
	std::vector<uint8_t> bytes;
	if (!file.empty() && files.read(file, bytes)) arms_model_ = assets::parse_model(bytes.data(), bytes.size());
	++arms_serial_;
	return true;
}

bool FirstPersonSources::refresh(const std::shared_ptr<const FileSource> &source, const AssetScan *scan,
                                 const PreviewRig &rig, const FirstPersonOptions &options, size_t gun_parts) {
	// A weapon's map, paired by the weapon (a model chosen is no weapon's view).
	const bool weapon_map = source && !rig.record_file.empty() && rig.record_field == "animadm";
	if (!weapon_map) {
		const bool was = active_ || arms_model_ || weapon_read_;
		clear();
		return was;
	}
	const FileSource &files = *source;
	bool moved = false;
	const std::string catalog = file_of(rig.record_file);
	const uint64_t stamp = files.stamp(catalog);
	if (!weapon_read_ || catalog != catalog_ || rig.record != record_ || stamp != catalog_stamp_ || bake_read_.moved(files)) {
		catalog_ = catalog;
		record_ = rig.record;
		catalog_stamp_ = stamp;
		read_weapon_(files, source, rig);
		moved = true;
	}
	const uint64_t avatars = files.stamp("Avatars.def");
	if (!avatars_read_ || avatars != avatars_stamp_) {
		avatars_stamp_ = avatars;
		read_characters_(files);
		moved = true;
	}
	// The character whose arms draw: the one chosen, else (an id no character has, or none chosen) the one a
	// fresh profile seeds.
	const int32_t wanted = options.character >= 0 ? options.character : int32_t(seeded_);
	size_t index = SIZE_MAX;
	for (size_t i = 0; i < characters_.size() && index == SIZE_MAX; ++i)
		if (characters_[i].id == wanted) index = i;
	for (size_t i = 0; i < characters_.size() && index == SIZE_MAX; ++i)
		if (characters_[i].id == seeded_) index = i;
	if (index != character_ || wanted != chosen_) moved = true;
	character_ = index;
	chosen_ = wanted;
	const FirstPersonCharacter *who = character();
	active_ = !weapon_.empty();
	spec_ = renderer::fp_viewmodel_spec(active_, gfx1_, who ? who->arms : std::string(), animadm_, flags_);
	if (!active_) {
		words_ = rig.record + " is not in " + catalog + ": no first person draws.";
	} else if (spec_.gun.empty()) {
		words_ = weapon_ + " names no gfx1: the game draws nothing in first person.";
	} else {
		words_ = weapon_ + " draws " + spec_.gun + " with ";
		if ((flags_ & renderer::kWeaponFlagEmplaced) != 0)
			words_ += "no arms: an emplaced weapon draws its gun alone.";
		else if (!who)
			words_ += "no arms: Avatars.def holds no character.";
		else if (who->arms.empty())
			words_ += "no arms: " + who->words + " has none.";
		else
			words_ += who->arms + ", the arms of " + who->words + ", posed by the gun's bones.";
	}
	if (read_arms_(files, scan)) moved = true;
	// Why the arms draw as they do, where it says anything: a model the project lacks or that does not
	// read, or one reaching past the gun's parts. The arms draw with the gun's bone array, part i by the
	// gun's part i [orig: Player_RenderFirstPersonViewModel @0x4DF088]: a part past the gun's takes no
	// posed matrix.
	std::string note;
	if (spec_.show_arms && arms_file_.empty())
		note = "The project has no " + spec_.arms + ": the game draws the gun alone.";
	else if (spec_.show_arms && !arms_model_)
		note = arms_file_ + " does not read as a model: the game draws the gun alone.";
	else if (arms_model_ && gun_parts > 0) {
		const int reach = renderer::fp_arms_part_reach(*arms_model_);
		if (size_t(reach) > gun_parts)
			note = arms_file_ + " draws with parts up to " + std::to_string(reach - 1) + ", but the gun has " +
			       std::to_string(gun_parts) + ": the game poses arms part i with the gun's part i, so parts " +
			       std::to_string(gun_parts) + " and on take no posed matrix.";
	}
	if (note != arms_note_) moved = true;
	arms_note_ = note;
	return moved;
}

// --- the eye ---------------------------------------------------------------------------------------

void first_person_view_units(const FirstPersonSources &sources, int width, int height, float out[3]) {
	// Standing at the hip: no published scope bias, no motion lead [orig: Player_UpdateFirstPersonCamera
	// @0x4dd380].
	world::PlayerViewState view;
	world::player_view_bias_view_units(view, false, sources.pos_units(), out);
	if (world::player_view_narrow_aspect(width, height))
		out[2] -= static_cast<float>(world::kFpNarrowAspectDropQ16) / io::kFp16One;
}

OrbitCamera first_person_eye(const FirstPersonSources &sources, const OrbitCamera &orbit, int width, int height) {
	float units[3];
	first_person_view_units(sources, width, height, units);
	const float rig[3] = {0.0f, renderer::kViewmodelRigYawDeg, 0.0f};
	const renderer::FpViewmodelPose pose = renderer::fp_viewmodel_pose(units, sources.rot_bias_deg(), rig);
	// The camera the model stands before, in the model's frame: the root's rotation inverted (its rows are
	// the camera's axes) and its origin taken back through it.
	const float *b = pose.basis;
	OrbitCamera eye = orbit;
	eye.posed = true;
	eye.pose.right = PreviewVec3{b[0], b[1], b[2]};
	eye.pose.up = PreviewVec3{b[3], b[4], b[5]};
	eye.pose.back = PreviewVec3{b[6], b[7], b[8]};
	const float *t = pose.origin;
	eye.pose.eye = PreviewVec3{-(b[0] * t[0] + b[3] * t[1] + b[6] * t[2]), -(b[1] * t[0] + b[4] * t[1] + b[7] * t[2]),
	                           -(b[2] * t[0] + b[5] * t[1] + b[8] * t[2])};
	eye.pose.fov_degrees = sources.renderfov() > 0.0f ? sources.renderfov() : renderer::kWeaponRenderFovHDegDefault;
	eye.near_plane = renderer::kViewmodelPassNearZ;
	eye.far_plane = 100.0f;
	return eye;
}

// --- the envelope ------------------------------------------------------------------------------------

io::JsonValue first_person_json(const FirstPersonSources &sources, const FirstPersonOptions &options,
                                const std::string &clip_key, const WeaponActionRun *chosen) {
	if (!sources.active()) return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	out.set("weapon", json_string(sources.weapon()));
	out.set("view", json_string(options.eye ? "eye" : "orbit"));
	out.set("words", json_string(sources.words()));
	const renderer::FpViewmodelSpec &spec = sources.spec();
	out.set("gun", json_string(spec.gun));
	out.set("arms", json_string(spec.show_arms ? spec.arms : std::string()));
	out.set("arms_file", json_string(sources.arms_model() ? sources.arms_file() : std::string()));
	out.set("arms_note", json_string(sources.arms_note()));
	out.set("adm", json_string(spec.adm));
	out.set("emplaced", JsonValue::make_bool((sources.flags() & renderer::kWeaponFlagEmplaced) != 0));
	const auto three = [](const float *v) {
		JsonValue row = JsonValue::make_array();
		for (int i = 0; i < 3; ++i) row.push(json_number(v[i]));
		return row;
	};
	out.set("pos", three(sources.pos_units()));
	out.set("rot", three(sources.rot_bias_deg()));
	out.set("tpos", three(sources.tpos_units()));
	out.set("renderfov", json_number(sources.renderfov()));
	const FirstPersonCharacter *who = sources.character();
	JsonValue characters = JsonValue::make_array();
	for (const FirstPersonCharacter &character : sources.characters()) {
		JsonValue row = JsonValue::make_object();
		row.set("id", json_number(character.id));
		row.set("words", json_string(character.words));
		row.set("side", json_string(character.alignment == avatars::AVATAR_ALIGN_EVIL ? "evil" : "good"));
		row.set("arms", json_string(character.arms));
		row.set("chosen", JsonValue::make_bool(who == &character));
		characters.push(std::move(row));
	}
	out.set("characters", std::move(characters));
	out.set("character", who ? json_number(who->id) : JsonValue::make_null());
	out.set("team", json_number(sources.team()));
	JsonValue actions = JsonValue::make_array();
	for (const WeaponActionRun &run : sources.actions()) {
		JsonValue row = JsonValue::make_object();
		row.set("action", json_string(run.suffix));
		row.set("anim", json_string(run.anim_key));
		row.set("handler", json_string(run.handler));
		row.set("delay_start", json_number(run.delay_start));
		row.set("delay_end", json_number(run.delay_end));
		row.set("begins", JsonValue::make_bool(run.begins));
		row.set("finish", json_number(run.finish));
		row.set("stepped", json_number(run.stepped));
		row.set("next", json_string(run.next));
		JsonValue legs = JsonValue::make_array();
		for (const WeaponActionLeg &leg : run.legs) {
			JsonValue one = JsonValue::make_object();
			one.set("leg", json_string(leg.end ? "end" : "begin"));
			one.set("set", json_string(leg.set));
			one.set("tick", json_number(leg.tick));
			one.set("words", json_string(leg.words));
			legs.push(std::move(one));
		}
		row.set("legs", std::move(legs));
		row.set("words", json_string(run.words));
		actions.push(std::move(row));
	}
	out.set("actions", std::move(actions));
	JsonValue playing = JsonValue::make_array();
	for (const WeaponActionRun *run : sources.actions_playing(clip_key)) playing.push(json_string(run->suffix));
	out.set("playing", std::move(playing));
	out.set("action", json_string(chosen ? chosen->suffix : std::string()));
	out.set("events_read", JsonValue::make_bool(false));
	return out;
}

} // namespace opennova::editor
