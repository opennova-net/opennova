#include <editor/preview/preview_clip_fire.h>

#include <algorithm>
#include <cmath>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/mission_poses.h>
#include <editor/preview/sound_preview.h>
#include <editor/session/view/session_view.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/anim_event_bits.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/impact_scar.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The envelope carries the last of a long run's shots and events.
constexpr size_t kShotsShown = 32;
constexpr size_t kEventsShown = 64;

constexpr uint32_t kFireBits =
		anim::kAnimEventFirePrimary | anim::kAnimEventFireSecondary | anim::kAnimEventFireMarker3;

JsonValue vec3(const PreviewVec3 &v) {
	JsonValue out = JsonValue::make_array();
	out.push(json_number(v.x));
	out.push(json_number(v.y));
	out.push(json_number(v.z));
	return out;
}

const char *tag_name(int tag) {
	return tag >= 0 && tag < world::kImpactEffectTagCount ? world::kImpactEffectTagNames[tag] : "";
}

particle::Vec3 particle_of(const PreviewVec3 &v) { return particle::Vec3{v.x, v.y, v.z}; }

// A mission-frame direction (x forward, y left, z up) in the preview's space (preview_from_mission).
PreviewVec3 direction_to_preview(const world::Vec3 &d) { return PreviewVec3{d.y, d.z, d.x}; }

// What a bit asks of the body, as a timeline names it (anim::kAnimEventBits' words).
std::string bit_words(uint32_t bit) {
	for (const anim::AnimEventBit &row : anim::kAnimEventBits)
		if (row.mask == bit) return row.words;
	return "the walking fire's latch";
}

template <size_t N> std::string text_of(const char (&field)[N]) { return strutil::fixed_string(field, N); }

} // namespace

io::JsonValue clip_fire_options_to_json(const ClipFireOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("target", JsonValue::make_bool(options.target.shown));
	out.set("surface", json_string(tag_name(options.target.tag)));
	out.set("range", json_number(options.target.range));
	out.set("enemy", JsonValue::make_bool(options.enemy));
	return out;
}

bool read_clip_fire_options(const io::JsonValue &json, ClipFireOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options.fire is an object {target, surface, range, enemy}.";
		return false;
	}
	ClipFireOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "target" || key == "enemy") {
			if (!value.is_bool()) {
				error = key == "target" ? "options.fire.target is true or false (the target stands, or the rounds fly on)."
				                        : "options.fire.enemy is true or false (the shots seen by the other side).";
				return false;
			}
			(key == "target" ? options.target.shown : options.enemy) = value.boolean;
		} else if (key == "surface") {
			const int tag = value.is_string() ? world::impact_effect_tag_index(value.string.c_str()) : -1;
			if (tag < kWeaponRangeFirstTag) {
				error = "options.fire.surface is an effects-table row a face can play, obj to uwatersurface (dirt, "
				        "grass, cement, wood, metal, glass, water, ...).";
				return false;
			}
			options.target.tag = tag;
		} else if (key == "range") {
			if (!value.is_number() || value.number < kWeaponRangeNearest || value.number > kWeaponRangeFarthest) {
				error = "options.fire.range is the target's distance in metres, 2 to 500.";
				return false;
			}
			options.target.range = float(value.number);
		} else {
			error = "Unknown fire option \"" + key + "\" (it takes target, surface, range, enemy).";
			return false;
		}
	}
	held = options;
	return true;
}

ClipFire::ClipFire() = default;
ClipFire::~ClipFire() = default;

void ClipFire::clear() {
	if (!active_) return;
	active_ = false;
	armed_ = false;
	reads_ = false;
	words_.clear();
	item_ = ClipSoundItem();
	weapons_ = world::OrganicWeapons();
	model_.reset();
	rig_.reset();
	key_.clear();
	variant_ = 0;
	timeline_ = anim::ClipTimeline();
	has_clip_ = false;
	track_ = ClipSoundTrack();
	period_ = 0;
	height_ = 0.0f;
	inputs_ = 0;
	shots_.clear();
	scheduled_to_ = 0;
	range_.configure(WeaponRangeSetup());
	range_.set_shots({});
	catalog_.clear();
	effects_.close();
	planned_runs_ = UINT64_MAX;
	planned_events_ = 0;
	spawns_.clear();
}

void ClipFire::refresh(const SessionView &view, const ClipSoundItem &item, const ClipSoundBinding &binding,
		const assets::Model &model, const std::shared_ptr<const anim::SkeletalClips> &rig, const std::string &key,
		int variant, const anim::ClipTimeline *timeline, const ClipSoundTrack &track, int32_t period,
		const ClipFireOptions &options) {
	active_ = true;
	item_ = item;
	model_ = model;
	rig_ = rig;
	key_ = key;
	variant_ = variant;
	has_clip_ = timeline != nullptr && !key.empty();
	timeline_ = timeline ? *timeline : anim::ClipTimeline();
	track_ = track;
	period_ = period;
	// The body: an NPC's has the fire block, a player's none (DI-04's binding, the item's move_function or chosen).
	reads_ = !binding.player;
	bool fire_bits = false;
	for (uint32_t word : track.triggers) fire_bits = fire_bits || (word & kFireBits) != 0;
	const bool person = item.found && world::organic_init_class(item.ai_function.c_str());
	// The range, read only where a shot could fire (a clip of fire bits on an NPC's body of a person class): a
	// soldier's shots alone, over the project's ammo.def and weapon.def as the load reads them.
	if (has_clip_ && fire_bits && reads_ && person) {
		WeaponRangeSetup setup;
		setup.files = view.findings.assets;
		setup.catalog = "weapon.def";
		setup.view = WeaponShotView::Soldier;
		setup.enemy = options.enemy;
		setup.target = options.target;
		if (range_.configure(setup)) planned_runs_ = UINT64_MAX;
	}
	// The item's bytes as its organic init resolves them: the person classes' definition callback alone runs it.
	weapons_ = world::OrganicWeapons();
	const world::AmmoTable *table = range_.ammo_table();
	if (person && table) {
		const char *ammo[world::kOrganicAmmoSlots];
		const char *launch[world::kOrganicLaunchSlots];
		for (int slot = 0; slot < world::kOrganicAmmoSlots; ++slot) ammo[slot] = item.ammo[slot].c_str();
		for (int slot = 0; slot < world::kOrganicLaunchSlots; ++slot) launch[slot] = item.launch[slot].c_str();
		world::resolve_organic_weapons(ammo, launch, *table, model ? model.get() : nullptr, weapons_);
	}
	const bool any_ammo = std::any_of(weapons_.ammo.begin(), weapons_.ammo.end(), [](uint8_t b) { return b != 0; });
	armed_ = has_clip_ && fire_bits && reads_ && any_ammo && range_.ready();
	// Why, in words.
	if (!has_clip_) {
		words_ = "No clip plays.";
	} else if (!fire_bits) {
		words_ = "The clip carries no fire bit: nothing fires.";
	} else if (!reads_) {
		words_ = binding.body_words + " A player's body has no fire block: its fire bits fire nothing.";
	} else if (!item.found) {
		words_ = "No item pairs the clip, so no organic init sets an ammo byte: its fire bits fire nothing.";
	} else if (!person) {
		words_ = item.name + "'s ai_function " + (item.ai_function.empty() ? std::string("(none)") : item.ai_function) +
		         " is no person class (org0, org1): the organic init never sets its ammo bytes, so nothing fires.";
	} else if (!range_.ready()) {
		words_ = range_.why() + " Nothing fires.";
	} else if (!any_ammo) {
		words_ = item.name + " names no ammo of ammo.def (ammo_closeattack, ammo_easyrocket, ammo_advancedrocket, "
		                     "ammo_marker3): its fire bits fire nothing.";
	} else {
		words_ = binding.body_words + " Its fire bits fire " + item.name + "'s ammo on those ticks.";
	}
	// The range's frame stands at the height of the item's first launch point at rest, so its target faces the gun.
	height_ = 0.0f;
	for (int slot = 0; slot < world::kOrganicLaunchSlots && model_; ++slot) {
		const uint8_t byte = weapons_.launch[size_t(slot)];
		if (byte == 0 || byte > model_->user_point_count) continue;
		float p[3];
		threedi::threedi_user_point_position(&model_->user_points[byte - 1], p);
		height_ = preview_from_model(p).y;
		break;
	}
	// What the shots are made of: another starts the schedule (and the run) again.
	std::string key_text = key + '\n' + std::to_string(variant) + '\n' + std::to_string(period) + '\n' +
	                       std::to_string(reinterpret_cast<uintptr_t>(rig.get())) + '\n' +
	                       std::to_string(reinterpret_cast<uintptr_t>(model.get())) + '\n' + (armed_ ? "1" : "0") +
	                       std::to_string(height_) + '\n';
	for (uint8_t b : weapons_.ammo) key_text += std::to_string(b) + ',';
	for (uint8_t b : weapons_.launch) key_text += std::to_string(b) + ',';
	for (uint32_t word : track.triggers) key_text += std::to_string(word) + ',';
	uint64_t inputs = io::fnv1a64_bytes(io::kFnv1a64Offset, key_text.data(), key_text.size());
	if (inputs == 0) inputs = 1;
	if (inputs != inputs_) {
		inputs_ = inputs;
		shots_.clear();
		scheduled_to_ = 0;
		range_.set_shots({});
	}
	// The effects the shots spawn, over the catalog the game would load, read only while anything fires.
	if (armed_) catalog_.follow(view.project.scan, view.findings.assets);
}

world::Vec3 ClipFire::to_range_(const PreviewVec3 &point) const {
	// The preview's (y, z, x) of the mission frame, less the frame's height.
	return world::Vec3{point.z, point.x, point.y - height_};
}

PreviewVec3 ClipFire::to_preview(const world::Vec3 &point) const {
	return PreviewVec3{point.y, point.z + height_, point.x};
}

std::string ClipFire::ammo_name_(uint8_t byte) const {
	const world::AmmoTable *table = range_.ammo_table();
	const world::AmmoTableEntry *row = table && byte != 0 ? table->by_index(byte) : nullptr;
	return row ? row->name : std::string();
}

PreviewVec3 ClipFire::launch_point_(int launch_slot, int32_t clip_tick, std::string &name) const {
	name.clear();
	const uint8_t byte = launch_slot >= 0 && launch_slot < world::kOrganicLaunchSlots ? weapons_.launch[size_t(launch_slot)] : 0;
	// A zero byte: the body's raw position [orig: Entity_GetAttachmentWorldPosition @0x4b2767..0x4b278e].
	if (byte == 0 || !model_ || byte > model_->user_point_count) return PreviewVec3{};
	const threedi::ThreediUserPoint &point = model_->user_points[byte - 1];
	name = text_of(point.name);
	float p[3];
	threedi::threedi_user_point_position(&point, p);
	PreviewVec3 at = preview_from_model(p);
	// The point rides its bone as the clip poses it on the tick [orig: Entity_GetAttachmentWorldPosition @0x4B2670].
	if (rig_ && !key_.empty()) {
		const std::vector<PreviewJoint> joints = preview_posed_joints(*rig_, key_, variant_, clip_tick);
		const int bone = point.subobject_index;
		if (bone >= 0 && size_t(bone) < joints.size()) at = preview_joint_carry(joints[size_t(bone)], at);
	}
	return at;
}

std::vector<ClipFireShot> ClipFire::shots_of(uint32_t word, int32_t tick, int32_t clip_tick, int frame) const {
	std::vector<ClipFireShot> out;
	if (!reads_ || (word & kFireBits) == 0) return out;
	// One pass of the block on a tick it reads the word, no walking fire raised.
	const world::OrganicFirePass pass = world::organic_fire_pass(word, true, false, weapons_.ammo);
	for (int i = 0; i < pass.count; ++i) {
		const world::OrganicFireShot &planned = pass.shots[i];
		ClipFireShot shot;
		shot.tick = tick;
		shot.clip_tick = clip_tick;
		shot.frame = frame;
		shot.bit = planned.bit;
		shot.ammo_slot = planned.ammo_slot;
		shot.ammo = weapons_.ammo[planned.ammo_slot];
		shot.ammo_name = ammo_name_(shot.ammo);
		shot.launch_slot = planned.launch_slot;
		shot.at = launch_point_(planned.launch_slot, clip_tick, shot.point);
		const std::string field = world::kOrganicAmmoFields[planned.ammo_slot];
		const std::string from = shot.point.empty() ? std::string("the body's origin (no ") +
		                                                      world::kOrganicLaunchFields[planned.launch_slot] + " on the model)"
		                                            : shot.point + " (" + world::kOrganicLaunchFields[planned.launch_slot] + ")";
		shot.words = "Frame " + std::to_string(frame) + " (" + bit_words(planned.bit) + "): " +
		             (shot.ammo != 0 ? shot.ammo_name + " (" + field + ") from " + from
		                             : std::string("its ") + field + " names no ammo: nothing fires") +
		             ".";
		out.push_back(std::move(shot));
	}
	return out;
}

std::vector<std::string> ClipFire::event_words(uint32_t word) const {
	std::vector<std::string> out;
	if ((word & kFireBits) == 0 || !active_) return out;
	if (!reads_ || !armed_) {
		out.push_back("fires nothing: " + words_);
		return out;
	}
	const world::AmmoTable *table = range_.ammo_table();
	const world::OrganicFirePass pass = world::organic_fire_pass(word, true, false, weapons_.ammo);
	if (pass.count == 0)
		out.push_back(bit_words(anim::kAnimEventFireSecondary) + ": " + item_.name +
		              " names no ammo_easyrocket or ammo_advancedrocket: nothing fires");
	for (int i = 0; i < pass.count; ++i) {
		const world::OrganicFireShot &planned = pass.shots[i];
		const uint8_t byte = weapons_.ammo[planned.ammo_slot];
		const std::string field = world::kOrganicAmmoFields[planned.ammo_slot];
		const std::string &point = item_.launch[planned.launch_slot];
		const bool found = weapons_.launch[planned.launch_slot] != 0;
		std::string line = bit_words(planned.bit) + ": ";
		if (byte == 0) {
			line += "its " + field + " names no ammo: nothing fires";
		} else {
			const world::AmmoTableEntry *row = table ? table->by_index(byte) : nullptr;
			line += (row ? row->name : std::string("?")) + " (" + field + ") from " +
			        (found ? point + " (" + world::kOrganicLaunchFields[planned.launch_slot] + ")"
			               : std::string("the body's origin"));
			if (row) {
				line += row->ai_launch_set.empty() ? ", no ai_launch" : ", " + row->ai_launch_set + " heard";
				line += row->ai_launch_effect.empty() ? ", no ai_launcheffect" : ", " + row->ai_launch_effect + " at the point";
			}
		}
		out.push_back(line);
	}
	return out;
}

void ClipFire::schedule_to_(int32_t tick) {
	if (!armed_ || !has_clip_) return;
	const int32_t to = std::min(tick, kWeaponRangeMostTicks);
	if (to <= scheduled_to_) return;
	// Every tick the NPC body reads (its odd ticks) in (scheduled, to], the word its channel stands on there.
	const std::vector<ClipEventDue> due = clip_events_due(timeline_, track_, period_, scheduled_to_, to, false, false);
	scheduled_to_ = to;
	int number = 0;
	for (const ClipFireShot &shot : shots_) number = std::max(number, shot.number);
	bool grew = false;
	for (const ClipEventDue &read : due) {
		if ((read.word & kFireBits) == 0) continue;
		for (ClipFireShot &shot : shots_of(read.word, read.tick, read.clip_tick, read.frame)) {
			if (shot.ammo != 0) {
				shot.number = ++number;
				grew = true;
			}
			shots_.push_back(std::move(shot));
		}
	}
	if (!grew) return;
	std::vector<WeaponRangeShot> fired;
	for (const ClipFireShot &shot : shots_) {
		if (shot.ammo == 0) continue;
		WeaponRangeShot row;
		row.tick = shot.tick;
		row.ammo = shot.ammo;
		row.at = to_range_(shot.at);
		// The entity's heading and pitch: the body stands level facing the mission frame's +x.
		row.yaw_bam = 0;
		row.pitch_bam = 0;
		row.shot = shot.number;
		row.words = shot.words;
		fired.push_back(std::move(row));
	}
	range_.set_shots(std::move(fired));
}

void ClipFire::run_to(int32_t tick) {
	if (!armed_) {
		effects_.close();
		return;
	}
	schedule_to_(tick);
	// The range stands after the clock's tick: a shot on the tick the clock shows has left.
	range_.run_to(tick + 1);
	if (range_.runs() != planned_runs_) {
		spawns_.clear();
		planned_events_ = 0;
		planned_runs_ = range_.runs();
	}
	if (planned_events_ != range_.events().size()) plan_spawns_();
	effects_.plan(catalog_, catalog_.serial(), spawns_);
	effects_.play_to(tick);
}

void ClipFire::plan_spawns_() {
	using Kind = WeaponRangeEvent::Kind;
	const std::vector<WeaponRangeEvent> &events = range_.events();
	for (; planned_events_ < events.size(); ++planned_events_) {
		const WeaponRangeEvent &event = events[planned_events_];
		if (event.effect.empty() || (event.kind != Kind::Launch && event.kind != Kind::Impact)) continue;
		// The ammo arm's launch at the fire origin along the aim, an impact's at the stop along the flight: each a
		// transient descriptor [orig: NetPacket_DeserializeRoundEvent @ 0x42F270's ammo arm @ 0x42F521;
		// AmmoDef_ProcessImpactEffect @ 0x40A170].
		DefinitionSpawn spawn;
		spawn.effect = event.effect;
		spawn.tick = event.tick;
		spawn.pose = particle::descriptor_pose(particle_of(to_preview(event.at)),
		                                    particle_of(direction_to_preview(event.direction)));
		spawn.source = event.kind == Kind::Launch ? "launch" : "impact";
		spawn.point = event.kind == Kind::Impact ? tag_name(event.tag) : std::string();
		spawns_.push_back(std::move(spawn));
	}
}

std::vector<DefinitionTrail> ClipFire::trails() const {
	std::vector<DefinitionTrail> out;
	if (!armed_) return out;
	for (const WeaponRangeTrail &trail : range_.trails()) {
		DefinitionTrail drawn;
		drawn.style = trail.style;
		drawn.age = trail.age;
		for (const world::Vec3 &point : trail.points) drawn.points.push_back(to_preview(point));
		drawn.widths = trail.widths;
		out.push_back(std::move(drawn));
	}
	return out;
}

renderer::ScarDrawList ClipFire::scars() const {
	renderer::ScarDrawList list;
	if (!armed_) return list;
	list = range_.scars();
	for (renderer::ScarDrawBatch &batch : list.batches) {
		if (batch.entity_local) continue;
		for (uint32_t i = batch.first_vertex; i < batch.first_vertex + batch.vertex_count && i < list.vertices.size(); ++i) {
			renderer::ScarVertex &vertex = list.vertices[i];
			const PreviewVec3 at = to_preview(world::Vec3{vertex.x, vertex.y, vertex.z});
			vertex.x = at.x;
			vertex.y = at.y;
			vertex.z = at.z;
		}
	}
	return list;
}

bool ClipFire::target_corners(PreviewVec3 out[4]) const {
	world::Vec3 corners[4];
	if (!armed_ || !range_.ready() || !range_.target_corners(corners)) return false;
	for (int i = 0; i < 4; ++i) out[i] = to_preview(corners[i]);
	return true;
}

std::vector<ClipSoundFired> ClipFire::press(uint32_t word, int32_t clip_tick, int frame, const ClipSoundSources &sources,
		const PreviewVec3 &listener, audio::SoundSelector &selector) const {
	std::vector<ClipSoundFired> out;
	if (!armed_) return out;
	const world::AmmoTable *table = range_.ammo_table();
	for (const ClipFireShot &shot : shots_of(word, clip_tick, clip_tick, frame)) {
		const world::AmmoTableEntry *row = table && shot.ammo != 0 ? table->by_index(shot.ammo) : nullptr;
		ClipSoundFired fired;
		fired.tick = clip_tick;
		fired.frame = frame;
		fired.bits = word;
		fired.slot = -1;
		fired.pressed = true;
		fired.set = row ? row->ai_launch_set : std::string();
		if (!row || row->ai_launch_set.empty()) {
			fired.state = "empty";
			fired.words = shot.words + (row ? " Its ammo names no ai_launch: nothing is heard." : std::string());
			out.push_back(std::move(fired));
			continue;
		}
		// The ammo's ai_launch at the launch point, heard at the camera [orig: WeaponSlot_FireAndSpawnEffects
		// @0x53f440 -> Sound_PlayWithDistanceAttenuation @0x528E40].
		PreviewHearing heard;
		heard.source[0] = shot.at.x;
		heard.source[1] = shot.at.y;
		heard.source[2] = shot.at.z;
		heard.listener[0] = listener.x;
		heard.listener[1] = listener.y;
		heard.listener[2] = listener.z;
		const PreviewPlay play = plan_set_play(sources.banks(), sources.expansion(), row->ai_launch_set, std::string(),
		                                       selector, kClipSoundListenerView, &heard);
		fired.bank = play.bank;
		fired.words = shot.words + " " + play.words;
		fired.state = !play.found ? "missing" : !play.in_range ? "out_of_range" : play.voices.empty() ? "silent" : "played";
		for (const PreviewVoice &voice : play.voices)
			fired.voices.push_back({voice.wave, voice.file, std::string(), voice.pitch_q16, voice.volume});
		out.push_back(std::move(fired));
	}
	return out;
}

std::vector<ClipSoundFired> ClipFire::sounds_between(int32_t from, int32_t to, const ClipSoundSources &sources,
		const PreviewVec3 &listener, audio::SoundSelector &selector) const {
	std::vector<ClipSoundFired> out;
	if (!armed_) return out;
	for (const WeaponRangeEvent &event : range_.events()) {
		if (event.kind != WeaponRangeEvent::Kind::Sound || event.tick <= from || event.tick > to || event.set.empty())
			continue;
		ClipSoundFired fired;
		fired.tick = event.tick;
		fired.slot = -1;
		fired.set = event.set;
		// Where it plays, heard at the camera: a 3D one-shot at its distance [orig: Sound_Play3DPositional @0x527CB0].
		PreviewHearing heard;
		const PreviewVec3 at = to_preview(event.at);
		heard.source[0] = at.x;
		heard.source[1] = at.y;
		heard.source[2] = at.z;
		heard.listener[0] = listener.x;
		heard.listener[1] = listener.y;
		heard.listener[2] = listener.z;
		const PreviewPlay play = plan_set_play(sources.banks(), sources.expansion(), event.set, std::string(), selector,
		                                       kClipSoundListenerView, &heard);
		fired.bank = play.bank;
		fired.words = "Tick " + std::to_string(event.tick) + " (" + event.words + "): " + play.words;
		fired.state = !play.found ? "missing" : !play.in_range ? "out_of_range" : play.voices.empty() ? "silent" : "played";
		for (const PreviewVoice &voice : play.voices)
			fired.voices.push_back({voice.wave, voice.file, std::string(), voice.pitch_q16, voice.volume});
		out.push_back(std::move(fired));
	}
	return out;
}

io::JsonValue ClipFire::to_json(const ClipFireOptions &options) const {
	using Kind = WeaponRangeEvent::Kind;
	JsonValue out = JsonValue::make_object();
	out.set("body", json_string(reads_ ? "npc" : "player"));
	out.set("reads", JsonValue::make_bool(reads_));
	out.set("armed", JsonValue::make_bool(armed_));
	out.set("words", json_string(words_));
	out.set("item", json_string(item_.name));
	out.set("ai_function", json_string(item_.ai_function));
	out.set("move_function", json_string(item_.move_function));
	out.set("person", JsonValue::make_bool(item_.found && world::organic_init_class(item_.ai_function.c_str())));
	// The item's slots as the init resolves them.
	const world::AmmoTable *table = range_.ammo_table();
	JsonValue ammo = JsonValue::make_array();
	for (int slot = 0; slot < world::kOrganicAmmoSlots; ++slot) {
		const uint8_t byte = weapons_.ammo[size_t(slot)];
		const world::AmmoTableEntry *row = table && byte != 0 ? table->by_index(byte) : nullptr;
		JsonValue one = JsonValue::make_object();
		one.set("field", json_string(world::kOrganicAmmoFields[slot]));
		one.set("name", json_string(item_.ammo[slot]));
		one.set("byte", json_number(byte));
		one.set("ammo", json_string(row ? row->name : std::string()));
		one.set("ai_launch", json_string(row ? row->ai_launch_set : std::string()));
		one.set("ai_launcheffect", json_string(row ? row->ai_launch_effect : std::string()));
		ammo.push(std::move(one));
	}
	out.set("ammo", std::move(ammo));
	JsonValue launch = JsonValue::make_array();
	for (int slot = 0; slot < world::kOrganicLaunchSlots; ++slot) {
		JsonValue one = JsonValue::make_object();
		one.set("field", json_string(world::kOrganicLaunchFields[slot]));
		one.set("name", json_string(item_.launch[slot]));
		one.set("point", json_number(weapons_.launch[size_t(slot)]));
		launch.push(std::move(one));
	}
	out.set("launch", std::move(launch));
	// The range: its frame in the preview, its target.
	JsonValue range = JsonValue::make_object();
	range.set("ready", JsonValue::make_bool(range_.ready()));
	range.set("why", json_string(range_.why()));
	range.set("origin", vec3(to_preview(world::Vec3{0.0f, 0.0f, 0.0f})));
	range.set("forward", vec3(PreviewVec3{0.0f, 0.0f, 1.0f}));
	range.set("tick", json_number(range_.tick()));
	JsonValue target = clip_fire_options_to_json(options);
	PreviewVec3 corners[4];
	if (target_corners(corners)) {
		JsonValue points = JsonValue::make_array();
		for (const PreviewVec3 &corner : corners) points.push(vec3(corner));
		target.set("corners", std::move(points));
	}
	range.set("target", std::move(target));
	out.set("range", std::move(range));
	// The shots up to the clock (the run's ticks), the last of them.
	size_t shown = 0;
	while (shown < shots_.size() && shots_[shown].tick < range_.tick()) ++shown;
	JsonValue shots = JsonValue::make_array();
	for (size_t i = shown > kShotsShown ? shown - kShotsShown : 0; i < shown; ++i) {
		const ClipFireShot &shot = shots_[i];
		JsonValue row = JsonValue::make_object();
		row.set("tick", json_number(shot.tick));
		row.set("clip_tick", json_number(shot.clip_tick));
		row.set("frame", json_number(shot.frame));
		row.set("bit", json_number(double(shot.bit)));
		row.set("field", json_string(world::kOrganicAmmoFields[shot.ammo_slot]));
		row.set("ammo", json_string(shot.ammo_name));
		row.set("byte", json_number(shot.ammo));
		row.set("point", json_string(shot.point));
		row.set("at", vec3(shot.at));
		row.set("direction", vec3(shot.direction));
		row.set("number", json_number(shot.number));
		row.set("words", json_string(shot.words));
		shots.push(std::move(row));
	}
	out.set("shots", std::move(shots));
	out.set("shots_fired", json_number(range_.shots()));
	// The run's last events, oldest first.
	const std::vector<WeaponRangeEvent> &events = range_.events();
	JsonValue rows = JsonValue::make_array();
	for (size_t i = events.size() > kEventsShown ? events.size() - kEventsShown : 0; i < events.size(); ++i) {
		const WeaponRangeEvent &event = events[i];
		JsonValue row = JsonValue::make_object();
		row.set("tick", json_number(event.tick));
		row.set("kind", json_string(weapon_range_event_token(event.kind)));
		if (!event.set.empty()) row.set("set", json_string(event.set));
		if (!event.effect.empty()) row.set("effect", json_string(event.effect));
		if (event.kind == Kind::Fired || event.kind == Kind::Impact || event.kind == Kind::Sound || event.kind == Kind::Launch)
			row.set("at", vec3(to_preview(event.at)));
		if (event.kind == Kind::Impact) {
			row.set("tag", json_number(event.tag));
			row.set("row", json_string(tag_name(event.tag)));
		}
		if (event.kind == Kind::Fired) row.set("tracer", JsonValue::make_bool(event.tracer));
		if (event.round > 0) row.set("round", json_number(event.round));
		row.set("words", json_string(event.words));
		rows.push(std::move(row));
	}
	out.set("events", std::move(rows));
	out.set("event_count", json_number(double(events.size())));
	JsonValue rounds = JsonValue::make_array();
	for (const WeaponRangeRound &round : range_.rounds()) {
		JsonValue row = JsonValue::make_object();
		row.set("at", vec3(to_preview(round.at)));
		row.set("tracer", JsonValue::make_bool(round.tracer));
		rounds.push(std::move(row));
	}
	out.set("rounds", std::move(rounds));
	JsonValue tracers = JsonValue::make_array();
	for (const DefinitionTrail &trail : trails()) {
		JsonValue row = JsonValue::make_object();
		row.set("style", json_number(trail.style));
		row.set("points", json_number(double(trail.points.size())));
		if (!trail.points.empty()) row.set("newest", vec3(trail.points.back()));
		tracers.push(std::move(row));
	}
	out.set("tracers", std::move(tracers));
	out.set("scars", json_number(armed_ ? range_.scar_count() : 0));
	JsonValue effects = JsonValue::make_array();
	for (size_t i = spawns_.size() > kEventsShown ? spawns_.size() - kEventsShown : 0; i < spawns_.size(); ++i) {
		const DefinitionSpawn &spawn = spawns_[i];
		JsonValue row = JsonValue::make_object();
		row.set("effect", json_string(spawn.effect));
		row.set("tick", json_number(spawn.tick));
		row.set("source", json_string(spawn.source));
		if (!spawn.point.empty()) row.set("point", json_string(spawn.point));
		effects.push(std::move(row));
	}
	out.set("effects", std::move(effects));
	return out;
}

} // namespace opennova::editor
