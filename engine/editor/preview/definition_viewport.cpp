#include <editor/preview/definition_viewport.h>

#include <algorithm>
#include <cmath>

#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_edge.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/model_preview_rig.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/viewport_device.h>
#include <editor/project/project_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/mission/authoring.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <runtime/anim/adm_root_motion.h>
#include <runtime/world/destruction.h>
#include <runtime/world/item_effects.h>
#include <runtime/world/present_passes.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The size its device draws at where no canvas sizes it (a headless Shell's): the model's.
constexpr ViewportState kHeadlessSize{800, 600};
// The highest record id a person's warmup reads (the record's SSN is a word).
constexpr int kMostSsn = 65535;

std::string file_of(const std::string &path) {
	return path.substr(path.find_last_of("/\\") + 1);
}

// A name as a fixed char field holds it.
template <size_t N> std::string text_of(const char (&field)[N]) {
	return strutil::fixed_string(field, N);
}

// The scan's model file of a name the game loads a model by (its .3di where the name has no extension), as
// the model preview's damage state finds it; "" for none.
std::string model_file_named(const AssetScan *scan, const std::string &name) {
	if (!scan || name.empty()) return std::string();
	const AssetEntry *entry = scan->find(name);
	if (!entry && !strutil::ends_with_icase(name, ".3di")) entry = scan->find(name + ".3di");
	return entry && entry->kind == AssetKind::Model ? entry->logical_name : std::string();
}

JsonValue vec3(const particle::Vec3 &v) {
	JsonValue out = JsonValue::make_array();
	out.array.push_back(json_number(v.x));
	out.array.push_back(json_number(v.y));
	out.array.push_back(json_number(v.z));
	return out;
}

particle::Vec3 vec_of(const PreviewVec3 &v) {
	return particle::Vec3{v.x, v.y, v.z};
}

// A mission-local point of an item at the origin (x forward, y left, z up), in the preview's space.
particle::Vec3 from_mission(const world::Vec3 &v) {
	return vec_of(preview_from_mission(v.x, v.y, v.z));
}

const char *weapon_view_token(DefinitionWeaponView view) {
	return view == DefinitionWeaponView::First ? "first" : "third";
}

const char *spawn_status_token(particle::EffectSpawnStatus status) {
	switch (status) {
	case particle::EffectSpawnStatus::Spawned: return "spawned";
	case particle::EffectSpawnStatus::Suppressed: return "suppressed";
	case particle::EffectSpawnStatus::InvalidHandle: return "not_spawned";
	case particle::EffectSpawnStatus::EmptyEffect: return "empty_effect";
	case particle::EffectSpawnStatus::MissingSlot: return "missing_slot";
	case particle::EffectSpawnStatus::MissingOwner: return "missing_owner";
	case particle::EffectSpawnStatus::GroupCapacityReached: return "group_capacity_reached";
	case particle::EffectSpawnStatus::EmitterCapacityReached: return "emitter_capacity_reached";
	case particle::EffectSpawnStatus::Disabled: return "disabled";
	}
	return "not_spawned";
}

bool state_from_token(const std::string &token, DefinitionState &out) {
	for (const DefinitionState state :
			{DefinitionState::Alive, DefinitionState::Destroying, DefinitionState::Husk, DefinitionState::HuskFinal})
		if (token == definition_state_token(state)) {
			out = state;
			return true;
		}
	return false;
}

// A SetViewport's options over `held`: {state, enemy, weapon, occupied, ssn, mute, grid}, each optional.
bool read_options(const JsonValue &json, DefinitionViewportOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options is an object {state, enemy, weapon, occupied, ssn, mute, grid}.";
		return false;
	}
	DefinitionViewportOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "state") {
			if (!value.is_string() || !state_from_token(value.string, options.state)) {
				error = "options.state is \"alive\", \"destroying\", \"husk\" or \"husk_final\".";
				return false;
			}
		} else if (key == "weapon") {
			if (!value.is_string() || (value.string != "third" && value.string != "first")) {
				error = "options.weapon is \"third\" (the gun in a soldier's hands, gfx3) or \"first\" (the player's own "
				        "view, gfx1).";
				return false;
			}
			options.weapon = value.string == "first" ? DefinitionWeaponView::First : DefinitionWeaponView::Third;
		} else if (key == "enemy" || key == "occupied" || key == "mute" || key == "grid") {
			if (!value.is_bool()) {
				error = "options." + key + " is true or false.";
				return false;
			}
			(key == "enemy" ? options.enemy : key == "occupied" ? options.occupied : key == "mute" ? options.mute
			                                                                                      : options.grid) =
					value.boolean;
		} else if (key == "ssn") {
			int64_t whole = 0;
			if (!io::json_whole_in(value, 0, kMostSsn, whole)) {
				error = "options.ssn is the record id a person's spawn warms up by, a whole number from 0 to 65535.";
				return false;
			}
			options.ssn = int(whole);
		} else {
			error = "Unknown definition option \"" + key + "\" (it takes state, enemy, weapon, occupied, ssn, mute, grid).";
			return false;
		}
	}
	held = options;
	return true;
}

} // namespace

const char *definition_view_status_token(DefinitionViewStatus status) {
	switch (status) {
	case DefinitionViewStatus::NoProject: return "no_project";
	case DefinitionViewStatus::NoCatalog: return "no_catalog";
	case DefinitionViewStatus::NoRecord: return "no_record";
	case DefinitionViewStatus::NoModel: return "no_model";
	case DefinitionViewStatus::Missing: return "missing";
	case DefinitionViewStatus::Unreadable: return "unreadable";
	case DefinitionViewStatus::Ready: return "ready";
	}
	return "no_project";
}

const char *definition_state_token(DefinitionState state) {
	switch (state) {
	case DefinitionState::Alive: return "alive";
	case DefinitionState::Destroying: return "destroying";
	case DefinitionState::Husk: return "husk";
	case DefinitionState::HuskFinal: return "husk_final";
	}
	return "alive";
}

io::JsonValue definition_options_to_json(const DefinitionViewportOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("state", json_string(definition_state_token(options.state)));
	out.set("enemy", JsonValue::make_bool(options.enemy));
	out.set("weapon", json_string(weapon_view_token(options.weapon)));
	out.set("occupied", JsonValue::make_bool(options.occupied));
	out.set("ssn", json_number(options.ssn));
	out.set("mute", JsonValue::make_bool(options.mute));
	out.set("grid", JsonValue::make_bool(options.grid));
	return out;
}

std::string definition_options_change(const DefinitionViewportOptions &options) {
	return viewport_change(ViewportKind::Definition, "options", definition_options_to_json(options));
}

std::string definition_camera_change(const OrbitCamera &camera) {
	return viewport_change(ViewportKind::Definition, "camera", orbit_camera_to_json(camera));
}

// --- DefinitionViewport ---------------------------------------------------------------------------

DefinitionViewport::DefinitionViewport(std::string path) :
		ViewportModel(ViewportKind::Definition, std::move(path), kHeadlessSize) {}

DefinitionViewport::~DefinitionViewport() = default;

std::unique_ptr<ViewportModel> DefinitionViewport::make(const std::string &path) {
	return std::make_unique<DefinitionViewport>(path);
}

ViewportStatus DefinitionViewport::status() const {
	switch (reason_) {
	case DefinitionViewStatus::Ready: return ViewportStatus::Ready;
	case DefinitionViewStatus::Missing:
	case DefinitionViewStatus::Unreadable: return ViewportStatus::Failed;
	default: return ViewportStatus::Empty;
	}
}

std::string DefinitionViewport::message() const {
	switch (reason_) {
	case DefinitionViewStatus::NoProject: return "Open a project to preview its definitions.";
	case DefinitionViewStatus::NoCatalog: return "Open a definition table to preview its records.";
	case DefinitionViewStatus::NoRecord: return "Select a record to see it as the game draws it.";
	case DefinitionViewStatus::NoModel: return detail_;
	case DefinitionViewStatus::Missing: return "The project has no model " + detail_ + ": the game draws nothing for it.";
	case DefinitionViewStatus::Unreadable: return detail_ + " does not read as a model.";
	case DefinitionViewStatus::Ready: break;
	}
	return std::string();
}

std::string DefinitionViewport::caption() const {
	if (drawn_.record.empty()) return std::string();
	return " - " + drawn_.record + (drawn_.name.empty() ? std::string() : " (" + drawn_.name + ")");
}

int DefinitionViewport::lod() const {
	if (!model_ || model_->lod_count == 0) return -1;
	return model_preview_auto_lod(*model_, camera_, size().width);
}

ViewportAction DefinitionViewport::stop_(DefinitionViewStatus reason, const std::string &detail) {
	reason_ = reason;
	detail_ = detail;
	const bool held = model_ != nullptr || effects_.scene() != nullptr;
	model_.reset();
	model_file_.clear();
	effects_.close();
	if (skeleton_) ++skeleton_serial_;
	skeleton_.reset();
	shown_none();
	const ViewportAction action = picture_.stop();
	return held ? action : ViewportAction::Keep;
}

assets::Model DefinitionViewport::held_(const SessionView &view, const std::string &name, std::string &file) {
	static const assets::Model none;
	file = model_file_named(view.project.scan.get(), name);
	if (file.empty() || !view.findings.assets) return none;
	const FileSource &files = *view.findings.assets;
	const uint64_t stamp = files.stamp(file);
	for (HeldModel &held : models_)
		if (strutil::iequals(held.file, file)) {
			if (held.stamp != stamp) {
				held.stamp = stamp;
				std::vector<uint8_t> bytes;
				held.model = files.read(file, bytes) ? assets::parse_model(bytes.data(), bytes.size()) : assets::Model();
			}
			return held.model;
		}
	HeldModel held;
	held.file = file;
	held.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (files.read(file, bytes)) held.model = assets::parse_model(bytes.data(), bytes.size());
	// A few models at a time: the record's graphic, its husks, an enemy model.
	if (models_.size() >= 8) models_.erase(models_.begin());
	models_.push_back(std::move(held));
	return models_.back().model;
}

bool DefinitionViewport::subject_(const ViewportInput &input, const DefCatalogDocument &document, const CatalogRow &row,
		const PreviewClock &clock, std::string &why) {
	const SessionView &view = input.view;
	drawn_ = DefinitionDrawn();
	drawn_.record = row.name();
	item_record_ = false;
	item_ = DamageItem();
	damage_models_ = DamageModels();
	plan_ = DamagePlan();
	husk_file_.clear();
	piece_file_.clear();
	piece_model_.reset();
	anim_def_.clear();
	attrib_ = 0;
	type_ = 0;
	slot_ = DefinitionParticleSlot();
	intact_.clear();
	notes_.clear();
	switch (row.record_kind()) {
	case def::DefRecordKind::Item: {
		drawn_.kind = "item";
		const auto &def = row.native.as<def::DefItemDef>();
		item_record_ = true;
		item_ = damage_item_of(def, file_of(document.path()));
		anim_def_ = def.anim_def;
		attrib_ = def.attrib;
		type_ = def.type;
		slot_.effect = text_of(def.particlefx.effect);
		slot_.point = text_of(def.particlefx.userpoint);
		// The husk the game draws (husk first) and the model the pieces come from (huskfinal first), each the
		// project's file the game loads by its name: their points place the death's banks and its blasts.
		const assets::Model &husk = held_(view, world::husk_render_graphic(item_.husk, item_.huskfinal), husk_file_);
		const assets::Model &pieces = held_(view, world::death_piece_graphic(item_.husk, item_.huskfinal), piece_file_);
		if (husk) {
			note_damage_husk(*husk, damage_models_);
			// The KZ walk reads the first husk alone: a def with only a final husk has no KZ point.
			if (item_.husk.empty()) damage_models_.kz_points = 0;
		}
		if (pieces) {
			note_damage_pieces(*pieces, damage_models_);
			piece_model_ = pieces;
		}
		plan_ = damage_plan(item_, damage_models_);
		// The intact model: its graphic, its graphic_enemy as the enemy sees it.
		std::string field = "graphic";
		std::string name = item_.graphic;
		if (options_.enemy) {
			if (item_.graphic_enemy.empty())
				notes_.push_back(item_.name + " authors no enemy model (graphic_enemy): its graphic is drawn.");
			else {
				field = "graphic_enemy";
				name = item_.graphic_enemy;
				notes_.push_back("The game loads graphic_enemy at the mission's start, but no reader of it is witnessed: "
				                 "the game may draw the graphic for its enemies too.");
			}
		}
		intact_ = name;
		const DamageFrame frame = frame_at(clock);
		switch (options_.state) {
		case DefinitionState::Alive: break;
		case DefinitionState::Destroying:
			if (!plan_.swaps) notes_.push_back(plan_.class_words);
			else if (plan_.husk.empty())
				notes_.push_back(item_.name + " authors no husk model: the game keeps drawing its graphic as it dies.");
			else if (husk_file_.empty())
				notes_.push_back("The project has no " + plan_.husk + ": the preview keeps drawing the graphic.");
			else if (frame.husked) {
				field = item_.husk.empty() ? "huskfinal" : "husk";
				name = plan_.husk;
			}
			break;
		case DefinitionState::Husk:
			if (!plan_.swaps) notes_.push_back(plan_.class_words);
			if (plan_.husk.empty()) {
				notes_.push_back(item_.name + " authors no husk model: the game keeps drawing its graphic once it dies.");
			} else {
				field = item_.husk.empty() ? "huskfinal" : "husk";
				name = plan_.husk;
			}
			break;
		case DefinitionState::HuskFinal: {
			const std::string pieces_from = world::death_piece_graphic(item_.husk, item_.huskfinal);
			if (pieces_from.empty()) {
				why = item_.name + " authors no husk model: it has no final husk.";
				return false;
			}
			field = item_.huskfinal.empty() ? "husk" : "huskfinal";
			name = pieces_from;
			notes_.push_back("The game draws " + pieces_from + " only as the death pieces, each its own section; its "
			                 "Dead, Fire and Other points place the death's banks.");
			break;
		}
		}
		if (name.empty()) {
			why = item_.name + " names no model (" + field + "): the game draws nothing for it.";
			return false;
		}
		drawn_.field = field;
		drawn_.name = name;
		return true;
	}
	case def::DefRecordKind::Weapon: {
		drawn_.kind = "weapon";
		const auto &def = row.native.as<def::DefWeaponDef>();
		const bool first = options_.weapon == DefinitionWeaponView::First;
		drawn_.field = first ? "gfx1" : "gfx3";
		drawn_.name = first ? text_of(def.gfx1) : text_of(def.gfx3);
		if (drawn_.name.empty()) {
			why = drawn_.record + " names no " + (first ? "first-person model (gfx1)" : "third-person model (gfx3)") +
			      ": the game draws no gun for it there.";
			return false;
		}
		return true;
	}
	case def::DefRecordKind::Ammo: {
		drawn_.kind = "ammo";
		const auto &def = row.native.as<def::DefAmmoDef>();
		// The item a round becomes: its own side's, the enemy's as the enemy sees it, the friendly one where
		// it names no enemy item [orig: RoundData_SpawnRound @ 0x4EC79B].
		int id = def.frndly_trcr_type_id;
		drawn_.field = "frndly_trcr_type_id";
		if (options_.enemy && def.foe_trcr_type_id != 0) {
			id = def.foe_trcr_type_id;
			drawn_.field = "foe_trcr_type_id";
		} else if (options_.enemy) {
			notes_.push_back(drawn_.record + " names no enemy tracer item: the enemy sees its friendly one.");
		}
		if (id == 0) {
			why = drawn_.record + " names no tracer item (frndly_trcr_type_id): its rounds draw no model of their own.";
			return false;
		}
		const AssetGraph *graph = view.findings.graph.get();
		const GraphSymbol *symbol = graph ? graph->resolve_symbol(ReferenceKind::Item, std::to_string(id)) : nullptr;
		if (!symbol) {
			why = "No item catalog of the project defines item " + std::to_string(id) + ", the item " + drawn_.record +
			      "'s rounds become.";
			return false;
		}
		drawn_.via = symbol->record;
		drawn_.via_file = symbol->file;
		drawn_.via_locator = symbol->locator;
		for (const GraphEdge *edge : graph->references_of(symbol->file))
			if (edge->record == symbol->record && edge->field == "graphic") drawn_.name = edge->value;
		if (drawn_.name.empty()) {
			why = symbol->record + ", the item " + drawn_.record + "'s rounds become, names no model.";
			return false;
		}
		return true;
	}
	case def::DefRecordKind::Powerup:
		drawn_.kind = "powerup";
		why = "A powerup row draws nothing of its own: an item naming it (powerupdef) is what a mission places.";
		return false;
	default:
		drawn_.kind = "carry";
		why = "A carry limit draws nothing: it caps how many of an ammo class a soldier carries.";
		return false;
	}
}

void DefinitionViewport::pose_(const SessionView &view, const std::string &model_file) {
	// A person's spawn: its class, its .adm, its attributes, the SSN the preview picks, and the model whose bones
	// the .adm plays on, posed again only where one of them or a file the pose read moved.
	const FileSource &files = *view.findings.assets;
	const std::string key = item_.ai_function + '\n' + anim_def_ + '\n' + std::to_string(attrib_) + '\n' +
	                        std::to_string(options_.ssn) + '\n' + model_file;
	if (key == person_key_ && !rig_read_.moved(files)) return;
	person_key_ = key;
	person_ = MissionPose();
	rig_read_.clear();
	const bool had = skeleton_ != nullptr;
	skeleton_.reset();
	if (!item_record_ || !model_ || options_.state == DefinitionState::HuskFinal) {
		if (had) ++skeleton_serial_;
		return;
	}
	auto source = view.findings.assets;
	auto stamped = std::make_shared<StampedFiles>(source);
	const PreviewRigFiles rig_files(stamped);
	anim::AdmRootMotion motion;
	world::AnimVariantRings rings;
	PersonDefinition definition;
	definition.ai_function = item_.ai_function;
	definition.anim_def = anim_def_;
	definition.attrib = attrib_;
	PersonRecord record;
	record.ssn = options_.ssn;
	pose_person(definition, record, *source, *stamped, rig_files, motion, rings, person_);
	if (person_.status == "posed") {
		// The .adm the spawn loaded over the drawn model's bones, as the game binds it to the entity's model.
		PreviewRig rig;
		rig.model = model_file;
		rig.table = person_.adm;
		skeleton_ = load_preview_rig(rig, *model_, rig_files);
		if (!skeleton_) notes_.push_back(person_.adm + " does not load over " + model_file + "'s bones: the person stands unposed.");
	}
	rig_read_ = stamped->stamps();
	if (had || skeleton_) ++skeleton_serial_;
}

void DefinitionViewport::particle_slot_(const assets::Model &intact) {
	slot_spawns_.clear();
	slot_.admitted = false;
	slot_.controller = false;
	slot_.user_points.clear();
	slot_.words.clear();
	if (!item_record_ || slot_.effect.empty()) return;
	// The mission's start walks the item, building and marker pools, each with its attrib gate; a drivable
	// item's slot attaches while a driver controls it [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @
	// 0x522ee0; Entity_UpdateHeloRotorSpin @ 0x48fa70].
	const int pool = int(mission::authoring::entity_kind_for_item_type(type_));
	if (world::item_effect_pool_allows(pool, attrib_)) {
		slot_.admitted = true;
	} else if (world::item_effect_controller_allows(pool, attrib_)) {
		slot_.admitted = options_.occupied;
		slot_.controller = true;
		slot_.words = options_.occupied ? "Attached while a driver controls it (Occupied)."
		                                : "A drivable item's slot attaches only while a driver controls it: Occupied "
		                                  "shows it.";
	} else {
		slot_.words = (attrib_ & def::DEF_ITEM_ATTRIB_POWERUP) != 0
		                      ? "A powerup's particle slot never attaches."
		                      : "The mission's start attaches no particle slot to an item of this type.";
	}
	if (!slot_.admitted || !intact) return;
	// The first 16 points of the name, else once at the origin; each emitter at its point along the point's
	// direction (EffectWorld's forward pose of the model's user point, in the preview's space).
	const world::ItemEffectAttachPlan plan = world::item_effect_attach_plan(*intact, slot_.point.c_str());
	slot_.user_points = plan.user_points;
	for (const int i : plan.user_points) {
		const threedi::ThreediUserPoint &point = intact->user_points[size_t(i)];
		float at[3], direction[3];
		threedi::threedi_user_point_position(&point, at);
		threedi::threedi_user_point_direction(&point, direction);
		slot_spawns_.push_back({slot_.effect,
		                        effect_forward_pose(vec_of(preview_from_model(at)), vec_of(preview_from_model(direction))), 0,
		                        "particle_slot", strutil::fixed_string(point.name, sizeof(point.name))});
	}
	if (plan.origin_fallback)
		slot_spawns_.push_back(
				{slot_.effect, effect_forward_pose({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}), 0, "particle_slot", std::string()});
}

std::vector<DefinitionSpawn> DefinitionViewport::spawns_() const {
	std::vector<DefinitionSpawn> out;
	if (!item_record_ || !model_ || options_.state == DefinitionState::HuskFinal) return out;
	// The particle slot from the mission's start (the clock's tick 0): the wreck keeps the item's emitters.
	out = slot_spawns_;
	if (options_.state == DefinitionState::Alive || !plan_.swaps) return out;
	// The death's banks over the piece model's points, as the game's death tail spawns them.
	world::ItemDeathTraits traits;
	traits.husk_model_loaded = damage_models_.husk_read || damage_models_.piece_read;
	traits.particledeath = item_.particledeath;
	traits.particleh2odeath = item_.particleh2odeath;
	traits.particlefire = item_.particlefire;
	traits.particleother = item_.particleother;
	if (piece_model_) world::death_effect_banks_of(*piece_model_, traits.effect_banks);
	const std::vector<world::DeathBankSpawn> banks = world::death_bank_spawns(traits, false);
	static const char *const kSources[] = {"death", "dead", "fire", "other"};
	const auto bank = [&](int family, int32_t tick) {
		for (const world::DeathBankSpawn &spawn : banks) {
			if (spawn.family != family) continue;
			std::string point = spawn.section_tagged ? std::string(family == 1 ? "Dead" : family == 2 ? "Fire" : "Other") +
			                                                   " " + std::to_string(spawn.slot + 1)
			                                         : std::string();
			out.push_back({spawn.effect,
			               effect_descriptor_pose(from_mission(spawn.point.local_pos), from_mission(spawn.point.local_dir)),
			               tick, kSources[family], point});
		}
	};
	if (options_.state == DefinitionState::Husk) {
		// The wreck standing: its Fire and Other banks burn on it.
		bank(2, 0);
		bank(3, 0);
		return out;
	}
	for (const DamageLeg &leg : plan_.legs) {
		if (leg.kind != "effect") continue;
		if (leg.bank > 0) bank(leg.bank, leg.tick);
		else if (leg.at_item)
			out.push_back({leg.name, effect_descriptor_pose({0.0f, leg.above, 0.0f}, {0.0f, 0.0f, 0.0f}), leg.tick, "death",
			               std::string()});
	}
	return out;
}

DamageFrame DefinitionViewport::frame_at(const PreviewClock &clock) const {
	if (!item_record_ || !item_.found) return DamageFrame();
	switch (options_.state) {
	case DefinitionState::Destroying: return damage_frame(plan_, item_, clock.ticks());
	case DefinitionState::Husk: {
		// The wreck standing: husked from the swap, the fade at its end.
		DamageFrame frame = damage_frame(plan_, item_, damage_fade_end_tick(plan_, item_));
		frame.husked = true;
		frame.hidden_sections = plan_.hidden_sections;
		return frame;
	}
	default: return DamageFrame();
	}
}

std::map<std::string, int64_t> DefinitionViewport::ctrl_at(const PreviewClock &clock) const {
	std::map<std::string, int64_t> out;
	if (reason_ != DefinitionViewStatus::Ready || drawn_.field.rfind("husk", 0) != 0) return out;
	// The game writes the six every time the husk draws: zero first, the fade's phases once husked [orig:
	// Entity_PublishSwapFadePhases @ 0x5C3F40].
	const DamageFrame frame = frame_at(clock);
	if (!frame.husked) return out;
	for (int i = 0; i < 6; ++i)
		if (frame.fade.phases_q16[size_t(i)] != 0)
			out[threedi::threedi_ctrl_register_name(size_t(threedi::THREEDI_CTRL_OBJECT_DESTROY + i))] =
					frame.fade.phases_q16[size_t(i)];
	return out;
}

uint32_t DefinitionViewport::hidden_sections_at(const PreviewClock &clock) const {
	if (reason_ != DefinitionViewStatus::Ready || drawn_.field.rfind("husk", 0) != 0) return 0;
	if (options_.state == DefinitionState::HuskFinal) return 0;
	return frame_at(clock).hidden_sections;
}

ViewportAction DefinitionViewport::follow_(const ViewportInput &input, PreviewClock &clock) {
	const SessionView &view = input.view;
	if (!view.project.open || !view.findings.assets) {
		catalog_.clear();
		row_ = 0;
		drawn_ = DefinitionDrawn();
		return stop_(DefinitionViewStatus::NoProject, std::string());
	}
	const auto *document = dynamic_cast<const DefCatalogDocument *>(input.document ? records_of(*input.document) : nullptr);
	if (!document) {
		drawn_ = DefinitionDrawn();
		return stop_(DefinitionViewStatus::NoCatalog, std::string());
	}
	// The record: the Preview window's while it follows this table, else the one it showed. Another record shows
	// from tick 0 of the clock (its state played from its start), as another clip or effect does.
	const PreviewTarget &target = view.documents.previews[ViewportKind::Definition];
	if (target.path == path() && target.part && target.part != row_) {
		row_ = target.part;
		clock.seek_ticks(0);
	}
	const Node *node = document->row(row_);
	if (!node) {
		drawn_ = DefinitionDrawn();
		const ViewportAction action = stop_(DefinitionViewStatus::NoRecord, std::string());
		shown(*input.document);
		return action;
	}
	catalog_.follow(view.project.scan, view.findings.assets);
	std::string why;
	if (!subject_(input, *document, static_cast<const CatalogRow &>(*node), clock, why)) {
		const ViewportAction action = stop_(DefinitionViewStatus::NoModel, why);
		shown(*input.document);
		return action;
	}
	std::string file;
	const assets::Model &drawn = held_(view, drawn_.name, file);
	drawn_.file.clear();
	if (const AssetEntry *entry = file.empty() ? nullptr : view.project.scan->find(file)) drawn_.file = entry->relative_path;
	if (file.empty()) {
		const ViewportAction action = stop_(DefinitionViewStatus::Missing, drawn_.name);
		shown(*input.document);
		return action;
	}
	if (!drawn) {
		const ViewportAction action = stop_(DefinitionViewStatus::Unreadable, file);
		shown(*input.document);
		return action;
	}
	const FileSource &files = *view.findings.assets;
	const uint64_t generation = view.findings.assets->generation();
	if (drawn != model_) ++model_serial_;
	model_ = drawn;
	model_file_ = file;
	// The item's intact model: its particle slot's points, a person's bones.
	std::string intact_file;
	const assets::Model &intact = item_record_ ? held_(view, intact_, intact_file) : model_;
	particle_slot_(intact);
	pose_(view, file);
	if (!person_.status.empty() && person_.status != "posed" && type_ == def::DEF_ITEM_TYPE_PERSON)
		notes_.push_back(person_.status == "no_adm"     ? item_.name + " names no animation map (anim_def): the person stands unposed."
		                 : person_.status == "no_clips" ? person_.adm + " registers no clip the project holds: the person stands unposed."
		                 : person_.status == "class"    ? item_.name + "'s ai_function is no person's (org0, org1): its spawn poses nothing."
		                                                : std::string());
	reason_ = DefinitionViewStatus::Ready;
	detail_.clear();
	shown(*input.document);
	// Framed as another record, or another model of it as alive, first shows (a death's swap keeps the camera).
	const std::string framing = std::to_string(row_) + '\n' + (item_record_ ? intact_ : drawn_.name);
	if (framed_ != framing) {
		framed_ = framing;
		frame_();
		state_moved();
	}
	// Its effects over the catalog the game would load, played to the clock.
	effects_.plan(catalog_, catalog_.serial(), spawns_());
	effects_.play_to(clock.ticks());
	// The death's sounds play from the project's banks, read as the game reads them (DI-04's sources).
	if (options_.state == DefinitionState::Destroying)
		sound_sources_.refresh(*view.findings.assets,
		                       view.project.document ? view.project.document->expansion.name : std::string(), PreviewRig());
	// The picture: the model drawn (by its file and stamp) and the rig over it; anew where either moved.
	PreviewFollow::Key key;
	key.part = row_;
	const std::string picture = file + '\n' + std::to_string(files.stamp(file)) + '\n' + std::to_string(skeleton_serial_);
	key.state = io::fnv1a64_bytes(io::kFnv1a64Offset, picture.data(), picture.size());
	FileStamps read;
	read.note(file, files.stamp(file));
	read.add(rig_read_);
	switch (picture_.follow(key, false, files, generation)) {
	case PreviewFollow::Found::Same: {
		const bool moved = options_moved_;
		options_moved_ = false;
		return moved ? ViewportAction::Update : ViewportAction::Keep;
	}
	case PreviewFollow::Found::Files:
		options_moved_ = false;
		return picture_.built(read);
	case PreviewFollow::Found::Anew: break;
	}
	picture_.show(key, generation);
	options_moved_ = false;
	missing_.clear();
	return picture_.built(read);
}

std::vector<ClipSoundFired> DefinitionViewport::fire_sounds(const PreviewClock &clock, const AssetScan *scan,
		audio::SoundSelector &selector, uint64_t &next_seq) {
	std::vector<ClipSoundFired> out;
	const int32_t now = clock.ticks();
	// A seek since (a replay, a scrub) fires nothing for what it passed over: the sounds go on from the clock.
	const bool sought = clock.tick_seeks() != sound_seeks_;
	sound_seeks_ = clock.tick_seeks();
	const int32_t from = sound_cursor_;
	sound_cursor_ = now;
	if (reason_ != DefinitionViewStatus::Ready || options_.state != DefinitionState::Destroying || !plan_.swaps ||
	    from < 0 || sought || now <= from || now - from > kClipSoundCatchUpTicks)
		return out;
	// The legs on the ticks [from, now): the death on tick 0 heard as the clock leaves it.
	for (const DamageLeg &leg : plan_.legs) {
		if (leg.kind != "sound" || leg.tick < from || leg.tick >= now) continue;
		ClipSoundFired fired = plan_set_at_origin(leg.name, leg.tick, "the death sound", sound_sources_, camera_.eye(), selector);
		fired.seq = ++next_seq;
		fired.path = path();
		if (options_.mute && fired.state == "played") fired.state = "muted";
		if (scan) find_clip_sound_waves(fired, *scan);
		fired_.push_back(fired);
		out.push_back(std::move(fired));
	}
	if (fired_.size() > kSoundsFiredKept) fired_.erase(fired_.begin(), fired_.end() - kSoundsFiredKept);
	return out;
}

OrbitCamera DefinitionViewport::framed(int width, int height) const {
	OrbitCamera camera = camera_;
	if (!model_) return camera;
	PreviewVec3 center;
	float radius = 1.0f;
	model_preview_sphere(*model_, center, radius);
	camera.frame(center, radius, width > 0 ? width : kHeadlessSize.width, height > 0 ? height : kHeadlessSize.height);
	return camera;
}

void DefinitionViewport::frame_() {
	camera_ = framed(size().width, size().height);
}

std::unique_ptr<CanvasHalf> DefinitionViewport::make_canvas() const {
	OrbitCanvasHooks hooks;
	hooks.camera = [](const ViewportModel &viewport) -> const OrbitCamera & {
		return static_cast<const DefinitionViewport &>(viewport).camera();
	};
	hooks.framed = [](const ViewportModel &viewport, int width, int height) {
		return static_cast<const DefinitionViewport &>(viewport).framed(width, height);
	};
	hooks.change = definition_camera_change;
	return std::make_unique<OrbitCanvas>(hooks);
}

ViewportHit DefinitionViewport::hit(const ViewportContext &context, float, float) const {
	ViewportHit out;
	out.current = current(context.input);
	return out;
}

bool DefinitionViewport::handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
		std::string &error) const {
	error = "A definition's picture has no handles: the table's fields are edited in the Inspector.";
	return false;
}

bool DefinitionViewport::drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &error) const {
	error = "Nothing is dragged in a definition's picture: its camera is set with set_viewport.";
	return false;
}

bool DefinitionViewport::command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &,
		CanvasRequests &out, std::string &error) const {
	if (name == "frame") {
		out.request(request::set_viewport(path(), definition_camera_change(framed(context.width, context.height))));
		return true;
	}
	if (name == "replay") {
		JsonValue clock = JsonValue::make_object();
		clock.set("ticks", json_number(0));
		clock.set("playing", JsonValue::make_bool(true));
		out.request(request::set_viewport(path(), viewport_change(ViewportKind::Definition, "clock", std::move(clock))));
		return true;
	}
	error = "A definition's picture has no command \"" + name + "\" (frame, replay).";
	return false;
}

bool DefinitionViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera";
}

bool DefinitionViewport::check_(const io::JsonValue &json, std::string &error) const {
	DefinitionViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !read_options(*member, options, error)) return false;
	OrbitCamera camera = camera_;
	bool frame = false;
	if (const JsonValue *member = json.get("camera"); member && !read_orbit_camera(*member, camera, frame, error))
		return false;
	return true;
}

void DefinitionViewport::apply_(const io::JsonValue &json, PreviewClock &clock) {
	std::string error;
	if (const JsonValue *member = json.get("options")) {
		DefinitionViewportOptions options = options_;
		if (read_options(*member, options, error) && options != options_) {
			// Another state plays from its start, as another record does.
			if (options.state != options_.state) clock.seek_ticks(0);
			options_ = options;
			options_moved_ = true;
		}
	}
	if (const JsonValue *member = json.get("camera")) {
		bool frame = false;
		read_orbit_camera(*member, camera_, frame, error);
		if (frame) frame_();
	}
}

bool DefinitionViewport::report_(const ViewportDeviceReport &report) {
	picture_.read(report.files);
	bool moved = false;
	for (const std::string &name : report.missing)
		if (std::find(missing_.begin(), missing_.end(), name) == missing_.end()) {
			missing_.push_back(name);
			moved = true;
		}
	return moved;
}

io::JsonValue DefinitionViewport::options_json() const {
	return definition_options_to_json(options_);
}

io::JsonValue DefinitionViewport::camera_json() const {
	return orbit_camera_to_json(camera_);
}

io::JsonValue DefinitionViewport::body_json(const ViewportInput &input) const {
	JsonValue out = JsonValue::make_object();
	// The record shown and what it draws.
	JsonValue record = JsonValue::make_object();
	record.set("row", json_number(double(row_)));
	record.set("kind", json_string(drawn_.kind));
	record.set("name", json_string(drawn_.record));
	out.set("record", std::move(record));
	JsonValue draws = JsonValue::make_object();
	draws.set("field", json_string(drawn_.field));
	draws.set("name", json_string(drawn_.name));
	draws.set("file", json_string(drawn_.file));
	if (!drawn_.via.empty()) {
		JsonValue via = JsonValue::make_object();
		via.set("item", json_string(drawn_.via));
		via.set("file", json_string(drawn_.via_file));
		via.set("locator", json_string(drawn_.via_locator));
		draws.set("via", std::move(via));
	}
	draws.set("lod", json_number(lod()));
	out.set("draws", std::move(draws));
	if (item_record_) {
		// Its death as the game runs it, and the state the game draws at the clock.
		JsonValue death = JsonValue::make_object();
		death.set("class", json_string(plan_.class_words));
		death.set("swaps", JsonValue::make_bool(plan_.swaps));
		death.set("swap_tick", json_number(plan_.swap_tick));
		death.set("husk", json_string(plan_.husk));
		death.set("husk_file", json_string(husk_file_));
		death.set("pieces_from", json_string(plan_.pieces_from));
		death.set("pieces_file", json_string(piece_file_));
		death.set("hidden_sections", json_number(double(plan_.hidden_sections)));
		JsonValue legs = JsonValue::make_array();
		for (const DamageLeg &leg : plan_.legs) {
			JsonValue row = JsonValue::make_object();
			row.set("tick", json_number(leg.tick));
			row.set("kind", json_string(leg.kind));
			row.set("name", json_string(leg.name));
			row.set("words", json_string(leg.words));
			row.set("cite", json_string(leg.cite));
			// What this preview does with it: an effect leg's spawn and a sound's play are the picture's here.
			const bool drawn = leg.kind == "effect" && (leg.bank > 0 || leg.at_item);
			row.set("shown", json_string(drawn ? std::string("played") : leg.shown));
			legs.array.push_back(std::move(row));
		}
		death.set("legs", std::move(legs));
		out.set("death", std::move(death));
		const DamageFrame frame = frame_at(input.clock);
		JsonValue drawn_frame = JsonValue::make_object();
		drawn_frame.set("husked", JsonValue::make_bool(frame.husked));
		drawn_frame.set("fade_elapsed", json_number(frame.fade_elapsed));
		drawn_frame.set("hidden_sections", json_number(double(hidden_sections_at(input.clock))));
		JsonValue registers = JsonValue::make_object();
		for (const auto &entry : ctrl_at(input.clock)) registers.set(entry.first, json_number(double(entry.second)));
		drawn_frame.set("registers", std::move(registers));
		out.set("frame", std::move(drawn_frame));
		JsonValue slot = JsonValue::make_object();
		slot.set("effect", json_string(slot_.effect));
		slot.set("point", json_string(slot_.point));
		slot.set("admitted", JsonValue::make_bool(slot_.admitted));
		JsonValue points = JsonValue::make_array();
		for (const int point : slot_.user_points) points.array.push_back(json_number(point));
		slot.set("user_points", std::move(points));
		slot.set("words", json_string(slot_.words));
		out.set("particle_slot", std::move(slot));
		if (!person_.status.empty()) out.set("person", mission_pose_json(person_));
	}
	// The effects the picture spawns, each as the game resolves its name, and what the scene holds now.
	JsonValue effects = JsonValue::make_array();
	for (size_t i = 0; i < effects_.spawns().size(); ++i) {
		const DefinitionSpawn &spawn = effects_.spawns()[i];
		JsonValue row = JsonValue::make_object();
		row.set("effect", json_string(spawn.effect));
		row.set("source", json_string(spawn.source));
		row.set("point", json_string(spawn.point));
		row.set("tick", json_number(spawn.tick));
		row.set("at", vec3(spawn.pose.position));
		row.set("forward", vec3(spawn.pose.forward));
		row.set("status", json_string(spawn_status_token(effects_.status(i))));
		row.set("alive", JsonValue::make_bool(effects_.alive(i)));
		if (const particle::EffectClosure *closure = effects_.closure_of(spawn.effect)) {
			row.set("spawns", JsonValue::make_bool(closure->spawns()));
			row.set("defined_in", json_string(closure->source));
			row.set("stock", JsonValue::make_bool(closure->stock));
		}
		effects.array.push_back(std::move(row));
	}
	out.set("effects", std::move(effects));
	JsonValue play = JsonValue::make_object();
	play.set("tick", json_number(effects_.tick()));
	if (const std::shared_ptr<particle::EffectScene> &scene = effects_.scene()) {
		const particle::EffectLiveCounts counts = scene->live_counts();
		play.set("groups", json_number(double(counts.group_count)));
		play.set("emitters", json_number(double(counts.emitter_count)));
		play.set("particles", json_number(double(counts.particle_count)));
	}
	out.set("play", std::move(play));
	JsonValue sounds = JsonValue::make_array();
	for (const ClipSoundFired &fired : fired_) sounds.array.push_back(clip_sound_fired_to_json(fired));
	out.set("sounds_fired", std::move(sounds));
	JsonValue notes = JsonValue::make_array();
	for (const std::string &note : notes_)
		if (!note.empty()) notes.array.push_back(json_string(note));
	out.set("notes", std::move(notes));
	JsonValue missing = JsonValue::make_array();
	for (const std::string &name : missing_) missing.array.push_back(json_string(name));
	out.set("missing_graphics", std::move(missing));
	return out;
}

io::JsonValue DefinitionViewport::items_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_array();
	for (size_t i = 0; i < effects_.spawns().size(); ++i) {
		JsonValue item = JsonValue::make_object();
		item.set("index", json_number(double(i)));
		item.set("name", json_string(effects_.spawns()[i].effect));
		item.set("kind", json_string(effects_.spawns()[i].source));
		out.array.push_back(std::move(item));
	}
	return out;
}

} // namespace opennova::editor
