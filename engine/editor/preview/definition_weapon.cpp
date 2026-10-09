#include <editor/preview/definition_weapon.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/sound_preview.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/anim/adm_fallback.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/weapon_fsm.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// A long hold or a long run is many events: the envelope carries the last of them.
constexpr size_t kEventsShown = 64;

std::string file_of(const std::string &path) { return path.substr(path.find_last_of("/\\") + 1); }

template <size_t N> std::string text_of(const char (&field)[N]) { return strutil::fixed_string(field, N); }

PreviewVec3 add(const PreviewVec3 &a, const PreviewVec3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
PreviewVec3 scale(const PreviewVec3 &a, float s) { return {a.x * s, a.y * s, a.z * s}; }
PreviewVec3 cross(const PreviewVec3 &a, const PreviewVec3 &b) {
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const PreviewVec3 &a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
PreviewVec3 unit(const PreviewVec3 &a) {
	const float n = length(a);
	return n > 0.0f ? scale(a, 1.0f / n) : a;
}
particle::Vec3 particle_of(const PreviewVec3 &v) { return particle::Vec3{v.x, v.y, v.z}; }

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

// The scan's file of an animation map the game loads by `name` (its .adm, else default.adm where the project holds
// it [orig: AnimMap_LoadAdmFile @0x40CC40]); "" none.
std::string adm_file(const AssetScan *scan, const std::string &name) {
	if (!scan || name.empty()) return std::string();
	std::string base = file_of(name);
	const size_t dot = base.find_last_of('.');
	if (dot != std::string::npos) base.resize(dot);
	const AssetEntry *entry = scan->find(base + ".adm");
	if (!entry) entry = scan->find(anim::adm_name_or_default(base + ".adm", false));
	return entry ? entry->logical_name : std::string();
}

// The first user point of `name` on `model` (case aside), the game's lookup by name
// (threedi_3di3_find_user_point [orig: ModelGPM_FindUserpointByName @0x5B21E0]); null none.
const threedi::ThreediUserPoint *user_point(const assets::Model &model, const std::string &name) {
	if (!model || name.empty()) return nullptr;
	const int found = threedi::threedi_3di3_find_user_point(model.get(), name.c_str());
	return found >= 0 ? &model->user_points[found] : nullptr;
}

} // namespace

// --- the options -------------------------------------------------------------------------------------

io::JsonValue definition_fire_options_to_json(const DefinitionFireOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("view", json_string(options.eye ? "eye" : "orbit"));
	out.set("character", json_number(options.character));
	out.set("shooter", json_string(options.shooter == WeaponShotView::Player ? "player" : "soldier"));
	out.set("surface", json_string(tag_name(options.target.tag)));
	out.set("range", json_number(options.target.range));
	out.set("target", JsonValue::make_bool(options.target.shown));
	return out;
}

bool read_definition_fire_options(const io::JsonValue &json, DefinitionFireOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options.fire is an object {view, character, shooter, surface, range, target}.";
		return false;
	}
	DefinitionFireOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "view") {
			if (!value.is_string() || (value.string != "eye" && value.string != "orbit")) {
				error = "options.fire.view is eye (the first-person eye) or orbit.";
				return false;
			}
			options.eye = value.string == "eye";
		} else if (key == "character") {
			int64_t id = 0;
			if (!io::json_whole_in(value, -1.0, 65535.0, id)) {
				error = "options.fire.character is a character's id, 0 to 65535 (-1 the one a fresh profile seeds).";
				return false;
			}
			options.character = int32_t(id);
		} else if (key == "shooter") {
			if (!value.is_string() || (value.string != "soldier" && value.string != "player")) {
				error = "options.fire.shooter is soldier (the ammo's ai_launch and ai_launcheffect) or player (the "
				        "weapon's FIRE and RECOIL rows), how another sees a third-person shot.";
				return false;
			}
			options.shooter = value.string == "player" ? WeaponShotView::Player : WeaponShotView::Soldier;
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
		} else if (key == "target") {
			if (!value.is_bool()) {
				error = "options.fire.target is true or false (the target stands, or the rounds fly on).";
				return false;
			}
			options.target.shown = value.boolean;
		} else {
			error = "Unknown fire option \"" + key + "\" (it takes view, character, shooter, surface, range, target).";
			return false;
		}
	}
	held = options;
	return true;
}

// --- the weapon ---------------------------------------------------------------------------------------

DefinitionWeapon::DefinitionWeapon() = default;
DefinitionWeapon::~DefinitionWeapon() = default;

void DefinitionWeapon::clear() {
	const uint64_t rig_serial = rig_serial_ + (rig_ ? 1 : 0);
	active_ = false;
	first_ = false;
	record_.clear();
	gun_.reset();
	gun_file_.clear();
	first_person_.clear();
	rig_files_ = PreviewRig();
	rig_.reset();
	rig_serial_ = rig_serial;
	rig_read_.clear();
	eye_ = false;
	spawns_.clear();
	notes_.clear();
	planned_runs_ = UINT64_MAX;
	planned_events_ = 0;
	range_.configure(WeaponRangeSetup());
	range_.set_gestures({});
}

bool DefinitionWeapon::refresh(const SessionView &view, const std::string &catalog, const def::DefWeaponDef &row,
		bool first, bool enemy, const DefinitionFireOptions &options, const assets::Model &gun, const std::string &gun_file,
		const OrbitCamera &orbit, int width, int height) {
	const bool gun_moved = gun != gun_;
	bool moved = !active_ || first != first_ || gun_moved;
	active_ = true;
	first_ = first;
	record_ = text_of(row.weapon_name);
	gun_ = gun;
	gun_file_ = gun_file;
	notes_.clear();
	const std::shared_ptr<const FileSource> files = view.findings.assets;
	const AssetScan *scan = view.project.scan.get();
	if (first) {
		// The first person (DI-13): the weapon paired with its map, its gfx1 the rig's model, its animadm the rig.
		PreviewRig rig;
		rig.model = gun_file;
		rig.table = adm_file(scan, text_of(row.animadm));
		rig.record_file = catalog;
		rig.record = record_;
		rig.record_field = "animadm";
		const uint64_t arms_serial = first_person_.arms_serial();
		FirstPersonOptions fp;
		fp.eye = options.eye;
		fp.character = options.character;
		const size_t gun_parts = gun && gun->lod_count > 0 ? gun->lods[0].render_object_count : 0;
		if (first_person_.refresh(files, scan, rig, fp, gun_parts)) moved = true;
		if (first_person_.arms_serial() != arms_serial) moved = true;
		// The gun's rig: its map over the gun's bones, loaded again for another model or table, or a file it read
		// moved.
		const bool rig_moved = !files || rig.model != rig_files_.model || rig.table != rig_files_.table || gun_moved ||
		                       rig_read_.moved(*files);
		if (rig_moved && files && gun) {
			auto stamped = std::make_shared<StampedFiles>(files);
			const PreviewRigFiles rig_files(stamped);
			rig_ = rig.table.empty() ? nullptr : load_preview_rig(rig, *gun, rig_files);
			rig_read_ = stamped->stamps();
			++rig_serial_;
			moved = true;
		}
		rig_files_ = rig;
		if (!rig_)
			notes_.push_back(rig.table.empty() ? record_ + " names no animation map the project holds (animadm): the gun "
			                                                "stands unposed."
			                                   : rig.table + " does not load over " + gun_file + "'s bones: the gun stands "
			                                                                                     "unposed.");
		if (!first_person_.arms_note().empty()) notes_.push_back(first_person_.arms_note());
	} else {
		if (rig_) ++rig_serial_;
		rig_.reset();
		rig_files_ = PreviewRig();
		rig_read_.clear();
		first_person_.clear();
	}
	const PreviewVec3 origin = origin_, forward = forward_, up = up_;
	frame_(row, orbit, width, height);
	if (origin.x != origin_.x || origin.y != origin_.y || origin.z != origin_.z || forward.x != forward_.x ||
	    forward.y != forward_.y || forward.z != forward_.z || up.x != up_.x || up.y != up_.y || up.z != up_.z || moved)
		planned_runs_ = UINT64_MAX; // every spawn placed again
	// The range: the project's weapon table and ammo, the shots in the view the picture shows.
	WeaponRangeSetup setup;
	setup.files = files;
	setup.catalog = file_of(catalog);
	setup.weapon = record_;
	setup.view = first ? WeaponShotView::Own : options.shooter;
	setup.enemy = enemy;
	setup.target = options.target;
	range_.configure(setup);
	if (!range_.ready()) notes_.push_back(range_.why() + " Nothing fires.");
	else if (range_.ammo().empty())
		notes_.push_back(record_ + "'s round_type names no ammo of ammo.def: the game spawns no round for it.");
	if ((row.flags & def::DEF_WEAPON_FLAG_EMPLACED) != 0)
		notes_.push_back("An emplaced weapon fires from its mount: the range has none, and the game refuses its rounds.");
	if (!frame_words_.empty()) notes_.push_back(frame_words_);
	return moved;
}

bool DefinitionWeapon::refresh_ammo(const SessionView &view, const std::string &ammo, bool enemy,
		const DefinitionFireOptions &options) {
	bool moved = !active_ || first_ || gun_ || !record_.empty();
	if (gun_ || first_person_.active()) moved = true;
	active_ = true;
	first_ = false;
	record_.clear();
	gun_.reset();
	gun_file_.clear();
	notes_.clear();
	eye_ = false;
	if (rig_) ++rig_serial_;
	rig_.reset();
	rig_files_ = PreviewRig();
	rig_read_.clear();
	first_person_.clear();
	// The range's frame: the picture's origin along +Z, up the picture's (the round stands for its soldier).
	const PreviewVec3 origin = origin_, forward = forward_;
	origin_ = PreviewVec3{0.0f, 0.0f, 0.0f};
	forward_ = PreviewVec3{0.0f, 0.0f, 1.0f};
	up_ = PreviewVec3{0.0f, 1.0f, 0.0f};
	left_ = unit(cross(up_, forward_));
	frame_words_ = "Fired alone, as the game fires an ammo for a soldier: the round leaves the picture's origin along "
	               "+Z here (a soldier's leaves its eye along its aim).";
	if (origin.x != origin_.x || origin.y != origin_.y || origin.z != origin_.z || forward.x != forward_.x ||
	    forward.y != forward_.y || forward.z != forward_.z || moved)
		planned_runs_ = UINT64_MAX;
	WeaponRangeSetup setup;
	setup.files = view.findings.assets;
	// A soldier's shots (DI-24's range): weapon.def and ammo.def as the load reads them, each by its name.
	setup.catalog = "weapon.def";
	setup.ammo = ammo;
	setup.view = WeaponShotView::Soldier;
	setup.enemy = enemy;
	setup.target = options.target;
	range_.configure(setup);
	if (!range_.ready()) notes_.push_back(range_.why() + " Nothing fires.");
	notes_.push_back(frame_words_);
	return moved;
}

void DefinitionWeapon::frame_(const def::DefWeaponDef &row, const OrbitCamera &orbit, int width, int height) {
	frame_words_.clear();
	eye_ = false;
	if (first_ && first_person_.active() && !first_person_.spec().gun.empty()) {
		// The eye the game stands the view model before: the shooter's eye, the round's origin.
		eye_camera_ = first_person_eye(first_person_, orbit, width, height);
		origin_ = eye_camera_.pose.eye;
		forward_ = unit(scale(eye_camera_.pose.back, -1.0f));
		up_ = unit(eye_camera_.pose.up);
		left_ = unit(cross(up_, forward_));
		eye_ = true;
		return;
	}
	// The gun alone: its launch point along its direction made level, else its origin along its forward.
	origin_ = PreviewVec3{0.0f, 0.0f, 0.0f};
	forward_ = PreviewVec3{0.0f, 0.0f, 1.0f};
	up_ = PreviewVec3{0.0f, 1.0f, 0.0f};
	const std::string launch = text_of(row.launch_user_point);
	PreviewVec3 at, direction;
	if (!launch.empty() && model_point_(launch, at, direction)) {
		origin_ = at;
		const PreviewVec3 level{direction.x, 0.0f, direction.z};
		if (length(level) > 0.001f) forward_ = unit(level);
		frame_words_ = "Drawn alone, the gun stands for the soldier: the round leaves its launch point " + launch +
		               " here (the game's leaves the soldier's eye along its aim).";
	} else {
		frame_words_ = "Drawn alone, the gun stands for the soldier: the round leaves the gun's origin along its "
		               "forward here (" + (launch.empty() ? record_ + " names no launchuserpoint"
		                                                  : gun_file_ + " has no point " + launch) +
		               "; the game's leaves the soldier's eye along its aim).";
	}
	left_ = unit(cross(up_, forward_));
}

PreviewVec3 DefinitionWeapon::to_preview(const world::Vec3 &point) const {
	return add(origin_, add(scale(forward_, point.x), add(scale(left_, point.y), scale(up_, point.z))));
}

PreviewVec3 DefinitionWeapon::direction_to_preview(const world::Vec3 &direction) const {
	return add(scale(forward_, direction.x), add(scale(left_, direction.y), scale(up_, direction.z)));
}

bool DefinitionWeapon::model_point_(const std::string &name, PreviewVec3 &at, PreviewVec3 &direction) const {
	const threedi::ThreediUserPoint *point = user_point(gun_, name);
	if (!point) return false;
	float p[3], d[3];
	threedi::threedi_user_point_position(point, p);
	threedi::threedi_user_point_direction(point, d);
	at = preview_from_model(p);
	direction = preview_from_model(d);
	// A point with no direction: the held gun's -Z, the presenter's fallback [orig: the third-person action point,
	// PlayerWeaponEffects::third_person_action_particle].
	if (length(direction) <= 0.001f) direction = PreviewVec3{0.0f, 0.0f, -1.0f};
	return true;
}

bool DefinitionWeapon::first_person_point_(const std::string &name, const std::string &clip, int32_t variant,
		int32_t ticks, PreviewVec3 &at, PreviewVec3 &direction) const {
	// The view model's parts in the presenter's order, the gun then the arms; each point rides its part's bone
	// (arms part i the gun's bone i), carried from the rest to the channel's pose [orig: Entity_ComputeActionTransform
	// @0x401310].
	const assets::Model parts[2] = {gun_, first_person_.arms_model()};
	for (const assets::Model &part : parts) {
		const threedi::ThreediUserPoint *point = user_point(part, name);
		if (!point) continue;
		float p[3], d[3];
		threedi::threedi_user_point_position(point, p);
		threedi::threedi_user_point_direction(point, d);
		at = preview_from_model(p);
		direction = preview_from_model(d);
		if (rig_ && !clip.empty()) {
			const std::vector<PreviewJoint> joints = preview_posed_joints(*rig_, clip, variant, ticks);
			const int bone = point->subobject_index;
			if (bone >= 0 && size_t(bone) < joints.size()) {
				at = preview_joint_carry(joints[size_t(bone)], at);
				direction = preview_joint_carry(joints[size_t(bone)], direction, true);
			}
		}
		// A point with no direction: the camera's forward [orig: the presenter's fallback].
		if (length(direction) <= 0.001f) direction = forward_;
		return true;
	}
	return false;
}

void DefinitionWeapon::run_to(int32_t tick) {
	range_.run_to(tick);
	// The spawns the run's new events make, placed once; every one again after the run starts over or the frame
	// moved.
	if (range_.runs() != planned_runs_) {
		spawns_.clear();
		planned_events_ = 0;
		planned_runs_ = range_.runs();
	}
	if (planned_events_ != range_.events().size()) plan_spawns_();
}

void DefinitionWeapon::plan_spawns_() {
	using Kind = WeaponRangeEvent::Kind;
	const std::vector<WeaponRangeEvent> &events = range_.events();
	for (; planned_events_ < events.size(); ++planned_events_) {
		const WeaponRangeEvent &event = events[planned_events_];
		if (event.effect.empty()) continue;
		DefinitionSpawn spawn;
		spawn.effect = event.effect;
		spawn.tick = event.tick;
		spawn.point = event.point;
		PreviewVec3 at, direction;
		switch (event.kind) {
		case Kind::Begin:
		case Kind::Effect: {
			// The shooter's own legs, at the first-person gun; an unresolved point at the entity's origin, along the
			// camera [orig: Entity_ComputeActionTransform @0x401310, the unresolved path @0x401867..0x401887].
			if (!first_ || (event.kind == Kind::Begin && !event.admitted)) continue;
			if (!first_person_point_(event.point, event.clip, event.clip_variant, event.clip_ticks, at, direction)) {
				at = origin_;
				direction = forward_;
			}
			// The begin leg's group is owner-bound at the action's transform; the direct effect a transient
			// descriptor [orig: ActionSlot_SpawnEffect @0x401F20; WeaponAction_Recoil @0x542F64].
			spawn.pose = event.kind == Kind::Begin ? particle::forward_pose(particle_of(at), particle_of(direction))
			                                       : particle::descriptor_pose(particle_of(at), particle_of(direction));
			spawn.source = event.kind == Kind::Begin ? "begin" : "direct";
			break;
		}
		case Kind::Launch: {
			// Another's view: the adm arm at the gun's point, the ammo arm at the fire origin along the aim.
			if (event.point.empty() || !model_point_(event.point, at, direction)) {
				at = to_preview(event.at);
				direction = direction_to_preview(event.direction);
			}
			spawn.pose = particle::descriptor_pose(particle_of(at), particle_of(direction));
			spawn.source = "launch";
			break;
		}
		case Kind::Impact:
			spawn.pose = particle::descriptor_pose(particle_of(to_preview(event.at)),
			                                    particle_of(direction_to_preview(event.direction)));
			spawn.source = "impact";
			spawn.point = tag_name(event.tag);
			break;
		default: continue;
		}
		spawns_.push_back(std::move(spawn));
	}
}

DefinitionWeaponClip DefinitionWeapon::clip() const {
	DefinitionWeaponClip out;
	const world::LocalPlayerWeaponView &view = range_.weapon_view();
	if (!first_ || !view.active) return out;
	out.key = view.anim_key;
	out.variant = view.anim_variant;
	out.ticks = view.anim_advance_ticks;
	out.blending = view.anim_blending;
	out.blend_key = view.anim_blend_key;
	out.blend_variant = view.anim_blend_variant;
	out.blend_ticks = view.anim_blend_ticks;
	out.blend_weight = view.anim_blend_weight;
	return out;
}

std::vector<DefinitionTrail> DefinitionWeapon::trails() const {
	std::vector<DefinitionTrail> out;
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

renderer::ScarDrawList DefinitionWeapon::scars() const {
	renderer::ScarDrawList list = range_.scars();
	// The shared ring's slots stand in the range's frame: taken into the preview's, a rotation (the quads keep
	// their winding, the face shot showing them).
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

bool DefinitionWeapon::target_corners(PreviewVec3 out[4]) const {
	world::Vec3 corners[4];
	if (!range_.ready() || !range_.target_corners(corners)) return false;
	for (int i = 0; i < 4; ++i) out[i] = to_preview(corners[i]);
	return true;
}

std::vector<ClipSoundFired> DefinitionWeapon::sounds_between(int32_t from, int32_t to, const ClipSoundSources &sources,
		const PreviewVec3 &listener, audio::SoundSelector &selector) const {
	using Kind = WeaponRangeEvent::Kind;
	std::vector<ClipSoundFired> out;
	for (const WeaponRangeEvent &event : range_.events()) {
		if (event.tick < from || event.tick >= to || event.set.empty()) continue;
		const bool own = (event.kind == Kind::Begin || event.kind == Kind::End) && range_.shot_view() == WeaponShotView::Own;
		if (!own && event.kind != Kind::Sound) continue;
		ClipSoundFired fired;
		fired.tick = event.tick;
		fired.slot = -1;
		fired.action = own ? event.action : std::string();
		fired.leg = own ? (event.kind == Kind::End ? "end" : "begin") : std::string();
		fired.set = event.set;
		PreviewPlay play;
		if (own) {
			// As the player hears their own weapon: a one-shot at the player, in first person [orig:
			// ActionSlot_ExecuteActionWithEffect @0x541860 / ActionSlot_FinishActivePhase @0x53f7b0 -> the set at the
			// owner; the listener's first-person view, audio::layer_matches_listener_view].
			constexpr uint8_t kFirstPersonView = 2;
			play = plan_set_play(sources.banks(), sources.expansion(), event.set, std::string(), selector, kFirstPersonView);
		} else {
			// Where it plays, heard at the camera: a 3D one-shot at their distance [orig: Sound_Play3DPositional
			// @0x527CB0], in the view the camera is in.
			audio::SetHearing heard;
			const PreviewVec3 at = to_preview(event.at);
			heard.source[0] = at.x;
			heard.source[1] = at.y;
			heard.source[2] = at.z;
			heard.listener[0] = listener.x;
			heard.listener[1] = listener.y;
			heard.listener[2] = listener.z;
			play = plan_set_play(sources.banks(), sources.expansion(), event.set, std::string(), selector,
			                     first_ ? 2 : kClipSoundListenerView, &heard);
		}
		fired.bank = play.bank;
		fired.words = "Tick " + std::to_string(event.tick) + " (" + event.words + "): " + play.words;
		fired.state = !play.found ? "missing" : !play.in_range ? "out_of_range" : play.voices.empty() ? "silent" : "played";
		for (const PreviewVoice &voice : play.voices)
			fired.voices.push_back({voice.wave, voice.file, std::string(), voice.pitch_q16, voice.volume});
		out.push_back(std::move(fired));
	}
	return out;
}

io::JsonValue DefinitionWeapon::to_json(const DefinitionFireOptions &options) const {
	using Kind = WeaponRangeEvent::Kind;
	JsonValue out = JsonValue::make_object();
	out.set("view", json_string(first_ ? "first" : "third"));
	out.set("eye", JsonValue::make_bool(eye_ && options.eye));
	out.set("shooter", json_string(weapon_shot_view_token(range_.shot_view())));
	if (first_) {
		FirstPersonOptions fp;
		fp.eye = options.eye;
		fp.character = options.character;
		out.set("first_person", first_person_json(first_person_, fp, std::string(), nullptr));
		JsonValue rig = JsonValue::make_object();
		rig.set("table", json_string(rig_files_.table));
		rig.set("loaded", JsonValue::make_bool(rig_ != nullptr));
		out.set("rig", std::move(rig));
	}
	out.set("ready", JsonValue::make_bool(range_.ready()));
	out.set("why", json_string(range_.why()));
	out.set("ammo", json_string(range_.ammo()));
	// The range: its frame in the preview, and the target.
	JsonValue range = JsonValue::make_object();
	range.set("origin", vec3(origin_));
	range.set("forward", vec3(forward_));
	range.set("up", vec3(up_));
	JsonValue target = JsonValue::make_object();
	target.set("shown", JsonValue::make_bool(range_.target().shown));
	target.set("surface", json_string(tag_name(range_.target().tag)));
	target.set("tag", json_number(range_.target().tag));
	target.set("material", json_number(range_.target().tag - 4));
	target.set("range", json_number(range_.target().range));
	PreviewVec3 corners[4];
	if (target_corners(corners)) {
		JsonValue points = JsonValue::make_array();
		for (const PreviewVec3 &corner : corners) points.push(vec3(corner));
		target.set("corners", std::move(points));
	}
	range.set("target", std::move(target));
	out.set("range", std::move(range));
	// The weapon as the game holds it now.
	const world::LocalPlayerWeaponView &held = range_.weapon_view();
	JsonValue state = JsonValue::make_object();
	const auto action = [](int32_t id) {
		return id >= 0 && id < world::weapon_action::kCount ? world::kWeaponActionSuffixes[id] : "";
	};
	state.set("tick", json_number(range_.tick()));
	state.set("action", json_string(action(held.current_action)));
	state.set("next", json_string(action(held.next_action)));
	state.set("phase", json_number(held.phase));
	state.set("clip", json_number(held.clip));
	state.set("reserve", json_number(held.reserve));
	state.set("heat", json_number(held.heat));
	state.set("scoped", JsonValue::make_bool(range_.scoped()));
	state.set("card", JsonValue::make_bool(card_up()));
	JsonValue channel = JsonValue::make_object();
	const DefinitionWeaponClip pose = clip();
	channel.set("clip", json_string(pose.key));
	channel.set("variant", json_number(pose.variant));
	channel.set("ticks", json_number(pose.ticks));
	channel.set("blending", JsonValue::make_bool(pose.blending));
	state.set("channel", std::move(channel));
	out.set("state", std::move(state));
	JsonValue gestures = JsonValue::make_array();
	for (const WeaponGestureAt &gesture : range_.gestures()) {
		JsonValue row = JsonValue::make_object();
		row.set("tick", json_number(gesture.tick));
		row.set("gesture", json_string(weapon_gesture_token(gesture.gesture)));
		gestures.push(std::move(row));
	}
	out.set("gestures", std::move(gestures));
	// The last events, oldest first.
	const std::vector<WeaponRangeEvent> &events = range_.events();
	JsonValue rows = JsonValue::make_array();
	for (size_t i = events.size() > kEventsShown ? events.size() - kEventsShown : 0; i < events.size(); ++i) {
		const WeaponRangeEvent &event = events[i];
		JsonValue row = JsonValue::make_object();
		row.set("tick", json_number(event.tick));
		row.set("kind", json_string(weapon_range_event_token(event.kind)));
		if (!event.action.empty()) row.set("action", json_string(event.action));
		if (!event.set.empty()) row.set("set", json_string(event.set));
		if (!event.effect.empty()) {
			row.set("effect", json_string(event.effect));
			row.set("point", json_string(event.point));
			row.set("admitted", JsonValue::make_bool(event.admitted));
		}
		if (event.kind == Kind::Clip) {
			row.set("clip", json_string(event.clip));
			row.set("variant", json_number(event.clip_variant));
		}
		if (event.kind == Kind::Fired || event.kind == Kind::Impact || event.kind == Kind::Sound || event.kind == Kind::Launch)
			row.set("at", vec3(to_preview(event.at)));
		if (event.kind == Kind::Impact) {
			row.set("tag", json_number(event.tag));
			row.set("row", json_string(tag_name(event.tag)));
		}
		if (event.round > 0) row.set("round", json_number(event.round));
		// The shooter's own legs show in its own view alone: another sees the shot's arm (`launch`).
		const bool own = event.kind == Kind::Clip || event.kind == Kind::Begin || event.kind == Kind::Effect ||
		                 event.kind == Kind::End;
		row.set("shown", JsonValue::make_bool(!own || range_.shot_view() == WeaponShotView::Own));
		if (event.kind == Kind::Fired) row.set("tracer", JsonValue::make_bool(event.tracer));
		row.set("words", json_string(event.words));
		rows.push(std::move(row));
	}
	out.set("events", std::move(rows));
	out.set("event_count", json_number(double(events.size())));
	out.set("shots", json_number(range_.shots()));
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
	out.set("scars", json_number(range_.scar_count()));
	return out;
}

} // namespace opennova::editor
