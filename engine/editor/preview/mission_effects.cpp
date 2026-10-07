#include <editor/preview/mission_effects.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <editor/assets/project_asset_source.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/mission_poses.h>
#include <editor/preview/mission_scene.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/mission/mission.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/mission/placement_traits.h>
#include <runtime/particle/emitter.h>
#include <runtime/world/item_effects.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

// A project file by its name as the asset source serves it (a path's file name).
std::string served_name(const std::string &file) {
	const size_t slash = file.find_last_of('/');
	return slash == std::string::npos ? file : file.substr(slash + 1);
}

uint64_t stamp_of(const SessionView &view, const std::string &file) {
	return view.findings.assets && !file.empty() ? view.findings.assets->stamp(served_name(file)) : 0;
}

// The game's entity kind of a scene pool (the kind the start's pool gates read).
int kind_of(MissionPool pool) {
	switch (pool) {
	case MissionPool::Item: return int(mission::EntityKind::Item);
	case MissionPool::Building: return int(mission::EntityKind::Building);
	case MissionPool::Marker: return int(mission::EntityKind::Marker);
	case MissionPool::Organic: return int(mission::EntityKind::Organic);
	}
	return int(mission::EntityKind::Item);
}

// The graphic an item's record loads, as the graph resolves its `graphic` field ("" none).
std::string graphic_of(const AssetGraph &graph, const GraphSymbol &item) {
	for (const GraphEdge *edge : graph.references_of(item.file)) {
		if (edge->record != item.record || edge->field != "graphic") continue;
		std::string file;
		if (graph.resolve(*edge, &file) == ReferenceStatus::Present) return file;
	}
	return std::string();
}

particle::Vec3 times(const particle::Vec3 &v, float s) {
	return particle::Vec3{ v.x * s, v.y * s, v.z * s };
}

particle::Vec3 plus(const particle::Vec3 &a, const particle::Vec3 &b) {
	return particle::Vec3{ a.x + b.x, a.y + b.y, a.z + b.z };
}

// The pose's axes over a local vector (right x + up y + forward z).
particle::Vec3 through(const particle::EffectPose &pose, const particle::Vec3 &v) {
	return plus(plus(times(pose.right, v.x), times(pose.up, v.y)), times(pose.forward, v.z));
}

// A local pose under its owner's, as the engine composes an attached group's (EffectScene's compose_pose,
// a Transform3D product).
particle::EffectPose composed(const particle::EffectPose &owner, const particle::EffectPose &local) {
	particle::EffectPose out;
	out.position = plus(owner.position, through(owner, local.position));
	out.right = through(owner, local.right);
	out.up = through(owner, local.up);
	out.forward = through(owner, local.forward);
	return out;
}

bool same_vec(const particle::Vec3 &a, const particle::Vec3 &b) {
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same_pose(const particle::EffectPose &a, const particle::EffectPose &b) {
	return same_vec(a.position, b.position) && same_vec(a.right, b.right) && same_vec(a.up, b.up) &&
			same_vec(a.forward, b.forward);
}

bool same_locals(const std::vector<particle::EffectPose> &a, const std::vector<particle::EffectPose> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (!same_pose(a[i], b[i])) return false;
	return true;
}

// The entity's pose as the placement draws it, in the presentation frame (the device's): its position and
// its rotation (mission::bms_to_presentation_basis, MissionObjectPlacer::entity_transform's), the item's
// model scale through the basis (items.def `scale`, the placer's item_entity_transform), a posed person
// raised by its spawn's lift (its model's node, as the device stands it).
particle::EffectPose owner_pose(const MissionEntityMark &entity, int32_t scale_q16, double lift) {
	const mission::PlacementVec3 at = mission::bms_to_presentation_position(
			mission::PlacementVec3{ float(entity.x), float(entity.y), float(entity.z) });
	const mission::PlacementBasis basis =
			mission::bms_to_presentation_basis(float(entity.pitch), float(entity.yaw), float(entity.roll));
	const float scale = scale_q16 != 0 ? float(scale_q16) / float(io::kFp16OneInt) : 1.0f;
	particle::EffectPose pose;
	pose.position = particle::Vec3{ at.x, at.y + float(lift), at.z };
	pose.right = times(particle::Vec3{ basis.x.x, basis.x.y, basis.x.z }, scale);
	pose.up = times(particle::Vec3{ basis.y.x, basis.y.y, basis.y.z }, scale);
	pose.forward = times(particle::Vec3{ basis.z.x, basis.z.y, basis.z.z }, scale);
	return pose;
}

particle::EffectOwnerToken owner_of(NodeId row) {
	return particle::EffectOwnerToken{ uint64_t(row) };
}

} // namespace

MissionEffects::MissionEffects() = default;
MissionEffects::~MissionEffects() = default;

const MissionEffectSlot *MissionEffects::slot(NodeId row) const {
	const auto found = slot_index_.find(row);
	return found == slot_index_.end() ? nullptr : &slots_[found->second];
}

const particle::EffectClosure *MissionEffects::closure_of(const std::string &effect) const {
	for (size_t i = 0; i < names_.size() && i < closures_.size(); ++i)
		if (strutil::iequals(names_[i], effect)) return &closures_[i];
	return nullptr;
}

size_t MissionEffects::alive(NodeId row) const {
	if (!scene_) return 0;
	for (const Held &held : held_) {
		if (held.slot.row != row) continue;
		size_t count = 0;
		for (const particle::EffectGroupId group : held.groups) count += scene_->contains_group(group) ? 1 : 0;
		return count;
	}
	return 0;
}

const MissionEffects::Catalog &MissionEffects::catalog_(const SessionView &view, const std::string &file) {
	Catalog &catalog = catalogs_[file];
	const uint64_t stamp = stamp_of(view, file);
	if (catalog.stamp == stamp && stamp != 0) return catalog;
	catalog = Catalog();
	catalog.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (!view.findings.assets || !view.findings.assets->read(served_name(file), bytes) || bytes.empty()) return catalog;
	def::DefItemsFile items{};
	if (def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0) {
		for (size_t i = 0; i < items.count; ++i) {
			const def::DefItemDef &row = items.entries[i];
			// A type id resolves to its first row [docs/world/itemdef-re.md, 2026-09-23].
			if (catalog.items.count(int64_t(row.id))) continue;
			ItemSlot slot;
			slot.defined = true;
			slot.effect = strutil::fixed_string(row.particlefx.effect, sizeof(row.particlefx.effect));
			slot.point = strutil::fixed_string(row.particlefx.userpoint, sizeof(row.particlefx.userpoint));
			slot.attrib = row.attrib;
			slot.scale_q16 = row.scale_q16;
			catalog.items.emplace(int64_t(row.id), std::move(slot));
		}
	}
	def::def_free_items(&items);
	return catalog;
}

const assets::Model &MissionEffects::model_(const SessionView &view, const std::string &file) {
	Model &model = models_[file];
	const uint64_t stamp = stamp_of(view, file);
	if (model.stamp == stamp && stamp != 0) return model.model;
	model = Model();
	model.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (!file.empty() && view.findings.assets && view.findings.assets->read(served_name(file), bytes) && !bytes.empty())
		model.model = assets::parse_model(bytes.data(), bytes.size());
	return model.model;
}

void MissionEffects::open_(const std::vector<std::string> &names) {
	scene_.reset();
	closures_.clear();
	names_ = names;
	played_ = false;
	++serial_;
	++opens_;
	for (Held &held : held_) held.groups.clear();
	if (names_.empty()) return;
	const particle::EffectSceneConfig config =
			catalog_files_.closures(names_, particle::EffectSceneConfig(), closures_);
	if (config.documents.empty()) return;
	scene_ = std::make_shared<particle::EffectScene>();
	scene_->open(config);
	// The mission's wind, which every GLOBALWIND particle drifts with [orig: Weather_SetMissionWind @
	// 0x5de970, from Game_StartMission @ 0x524aff].
	scene_->set_global_wind(particle::mission_wind_vector(wind_speed_, wind_direction_));
}

void MissionEffects::spawn_(Held &held, int32_t age) {
	held.groups.clear();
	held.slot.spawned = 0;
	if (!scene_) return;
	const particle::EffectOwnerToken owner = owner_of(held.slot.row);
	scene_->apply_owner_poses({ particle::EffectOwnerPoseUpdate{ owner, held.owner, true } });
	const particle::EffectHandle effect = scene_->intern(held.slot.effect);
	for (const particle::EffectPose &local : held.locals) {
		// The attached spawn as the game's EffectWorld makes it: its local pose under the owner's
		// (EffectWorld::spawn_effect_attached_request), following it.
		particle::EffectSpawnRequest request;
		request.effect = effect;
		request.binding = particle::EffectBinding::FollowOwner;
		request.owner = owner;
		request.owner_relative_pose = local;
		request.pose = composed(held.owner, local);
		request.initial_age_ticks = uint32_t(std::max(age, 0));
		const particle::EffectSpawnReceipt receipt = scene_->spawn(request);
		++spawns_made_;
		// A spawn whose catch-up outlived every emitter is gone already (EffectScene::spawn releases it).
		if (!receipt.spawned()) continue;
		++held.slot.spawned;
		if (scene_->contains_group(receipt.group)) held.groups.push_back(receipt.group);
	}
	const auto found = slot_index_.find(held.slot.row);
	if (found != slot_index_.end()) slots_[found->second].spawned = held.slot.spawned;
}

void MissionEffects::let_go_(Held &held) {
	if (scene_) scene_->apply_owner_poses({ particle::EffectOwnerPoseUpdate{ owner_of(held.slot.row), {}, false } });
	held.groups.clear();
	held.slot.spawned = 0;
}

bool MissionEffects::refresh(const SessionView &view, const MissionScene &scene, const MissionPoses &poses) {
	const bool catalog_moved = catalog_files_.follow(view.project.scan, view.findings.assets) ||
			catalog_files_.serial() != catalog_serial_;
	catalog_serial_ = catalog_files_.serial();
	const AssetGraph *graph = view.findings.graph.get();
	const uint64_t generation = graph ? graph->generation() : 0;
	// Nothing the slots read moved: they stand (every frame's follow asks).
	const Followed key{ scene.serial(), generation, view.findings.assets ? view.findings.assets->generation() : 0,
		poses.serial() };
	if (followed_ && !catalog_moved && key == followed_key_) return false;
	followed_key_ = key;
	if (!graph_read_ || generation != graph_generation_) {
		// Another graph (any file's edit takes one): each item resolved again, its files read again only where
		// their stamps moved.
		resolved_.clear();
		graph_generation_ = generation;
		graph_read_ = true;
	}
	const MissionSceneHeader &header = scene.header();
	const bool wind_moved = header.wind_speed != wind_speed_ || header.wind_direction != wind_direction_;
	wind_speed_ = header.wind_speed;
	wind_direction_ = header.wind_direction;

	// What the start attaches, in its walk's order: the item pool, the buildings, the markers (the organics
	// are never walked) [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522ee0].
	std::vector<MissionEffectSlot> slots;
	std::vector<Held> wanted;
	for (const MissionPool pool : { MissionPool::Item, MissionPool::Building, MissionPool::Marker }) {
		for (const MissionEntityMark &entity : scene.entities()) {
			if (entity.pool != pool) continue;
			auto resolved = resolved_.find(entity.item);
			if (resolved == resolved_.end()) {
				Resolved read;
				if (const GraphSymbol *symbol =
								graph ? graph->resolve_symbol(ReferenceKind::Item, std::to_string(entity.item)) : nullptr) {
					read.catalog = symbol->file;
					read.graphic = graphic_of(*graph, *symbol);
				}
				resolved = resolved_.emplace(entity.item, std::move(read)).first;
			}
			if (resolved->second.catalog.empty()) continue;
			const Catalog &catalog = catalog_(view, resolved->second.catalog);
			const auto item = catalog.items.find(entity.item);
			if (item == catalog.items.end() || item->second.effect.empty()) continue;
			MissionEffectSlot slot;
			slot.row = entity.row;
			slot.item = entity.item;
			slot.effect = item->second.effect;
			slot.point = item->second.point;
			const int kind = kind_of(entity.pool);
			if (!world::item_effect_pool_allows(kind, item->second.attrib)) {
				// A drivable item's slot waits for a driver, which no record is at the start.
				slot.status = world::item_effect_controller_allows(kind, item->second.attrib) ? "controller" : "gated";
				slots.push_back(std::move(slot));
				continue;
			}
			const assets::Model &model = model_(view, resolved->second.graphic);
			if (!model) {
				slot.status = "no_model";
				slots.push_back(std::move(slot));
				continue;
			}
			slot.status = "attached";
			Held held;
			// Each emitter at its point along the point's direction, else once at the origin [orig:
			// ItemDef_GetBoneMaskByName @ 0x49ea40 -> Entity_SpawnBoneTrailEffect @ 0x43bef0].
			const world::ItemEffectAttachPlan plan = world::item_effect_attach_plan(*model, slot.point.c_str());
			for (const int i : plan.user_points) {
				const threedi::ThreediUserPoint &point = model->user_points[size_t(i)];
				float at[3], direction[3];
				threedi::threedi_user_point_position(&point, at);
				threedi::threedi_user_point_direction(&point, direction);
				// The model's point in the presentation frame (ObjectData's godot_vec3: x mirrored).
				held.locals.push_back(effect_forward_pose(particle::Vec3{ -at[0], at[1], at[2] },
						particle::Vec3{ -direction[0], direction[1], direction[2] }));
				slot.points.push_back(strutil::fixed_string(point.name, sizeof(point.name)));
			}
			if (plan.origin_fallback) {
				held.locals.push_back(particle::EffectPose());
				slot.points.emplace_back();
			}
			const MissionPose *person = poses.pose(entity.row);
			held.owner = owner_pose(entity, item->second.scale_q16,
					person != nullptr && person->status == "posed" ? person->lift : 0.0);
			held.slot = slot;
			wanted.push_back(std::move(held));
			slots.push_back(std::move(slot));
		}
	}
	slots_ = std::move(slots);
	slot_index_.clear();
	for (size_t i = 0; i < slots_.size(); ++i) slot_index_[slots_[i].row] = i;

	// The effects the slots name, each once in the order first named.
	std::vector<std::string> names;
	for (const Held &held : wanted) {
		bool known = false;
		for (const std::string &name : names) known = known || strutil::iequals(name, held.slot.effect);
		if (!known) names.push_back(held.slot.effect);
	}
	bool held_names = true;
	for (const std::string &name : names) {
		bool known = false;
		for (const std::string &opened : names_) known = known || strutil::iequals(opened, name);
		held_names = held_names && known;
	}

	// Each slot's age counts from the tick it was first followed (0 for the mission's first read), or since
	// it was added or given another item or effect.
	std::unordered_map<NodeId, const Held *> before;
	for (const Held &held : held_) before[held.slot.row] = &held;
	const int32_t born_now = followed_ ? tick_ : 0;
	followed_ = true;
	bool changed = false;
	if (catalog_moved || !held_names) {
		// The scene opened again over the new closures, every slot at its age (spawned at the next play).
		for (Held &held : wanted) {
			const auto found = before.find(held.slot.row);
			held.born = found != before.end() && found->second->slot.effect == held.slot.effect ? found->second->born
																							   : born_now;
		}
		held_ = std::move(wanted);
		open_(names);
		return true;
	}
	if (wind_moved && scene_) {
		scene_->set_global_wind(particle::mission_wind_vector(wind_speed_, wind_direction_));
		changed = true;
	}
	// The slots that stand keep their groups (a moved owner moves them); one let go drains; one new or of
	// another item, effect or points spawns now.
	std::vector<particle::EffectOwnerPoseUpdate> moves;
	std::vector<Held> kept;
	kept.reserve(wanted.size());
	std::unordered_map<NodeId, bool> stays;
	for (Held &held : wanted) {
		const auto found = before.find(held.slot.row);
		Held *was = found != before.end() ? const_cast<Held *>(found->second) : nullptr;
		if (was && was->slot.effect == held.slot.effect && same_locals(was->locals, held.locals)) {
			if (!same_pose(was->owner, held.owner)) {
				moves.push_back(particle::EffectOwnerPoseUpdate{ owner_of(held.slot.row), held.owner, true });
				changed = true;
			}
			held.born = was->born;
			held.groups = std::move(was->groups);
			held.slot.spawned = was->slot.spawned;
			stays[held.slot.row] = true;
		} else {
			if (was) let_go_(*was);
			held.born = born_now;
			stays[held.slot.row] = true;
			changed = true;
		}
		kept.push_back(std::move(held));
	}
	for (Held &held : held_)
		if (!stays.count(held.slot.row)) {
			let_go_(held);
			changed = true;
		}
	if (scene_ && !moves.empty()) scene_->apply_owner_poses(moves);
	// What spawns now: a new slot, made at once while the scene plays (else at the next play's jump).
	std::vector<bool> fresh(kept.size(), false);
	for (size_t i = 0; i < kept.size(); ++i) {
		const auto found = before.find(kept[i].slot.row);
		fresh[i] = found == before.end() || found->second->slot.effect != kept[i].slot.effect ||
				!same_locals(found->second->locals, kept[i].locals);
	}
	held_ = std::move(kept);
	for (size_t i = 0; i < held_.size(); ++i) {
		const auto found = slot_index_.find(held_[i].slot.row);
		if (found != slot_index_.end()) slots_[found->second].spawned = held_[i].slot.spawned;
		if (fresh[i] && played_) spawn_(held_[i], tick_ - held_[i].born);
	}
	if (changed) ++serial_;
	return changed;
}

void MissionEffects::play_to(int32_t tick) {
	tick = std::max(tick, 0);
	if (!scene_) {
		tick_ = tick;
		return;
	}
	const auto start = std::chrono::steady_clock::now();
	struct Timed {
		std::chrono::steady_clock::time_point start;
		int64_t &out;
		~Timed() {
			out = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		}
	} timed{ start, step_us_ };
	const bool continuous = played_ && tick >= tick_ && tick - tick_ <= int32_t(particle::kEffectAdvanceTickLimit);
	if (!continuous) {
		// A jump: the scene emptied, each slot spawned again pre-aged by its age then.
		scene_->reset_runtime_state();
		tick_ = tick;
		for (Held &held : held_) spawn_(held, tick - held.born);
		played_ = true;
		++serial_;
		return;
	}
	// A game tick at a time (the scene's own fixed step).
	particle::EffectAdvanceRequest step;
	step.delta_seconds = 1.0f / static_cast<float>(io::kTickHz);
	for (int32_t at = tick_ + 1; at <= tick; ++at) scene_->advance_simulation(step);
	if (tick != tick_) ++serial_;
	tick_ = tick;
}

void MissionEffects::close() {
	if (scene_ || !held_.empty() || !slots_.empty()) ++serial_;
	scene_.reset();
	names_.clear();
	closures_.clear();
	held_.clear();
	slots_.clear();
	slot_index_.clear();
	resolved_.clear();
	catalog_files_.clear();
	catalog_serial_ = UINT64_MAX;
	graph_read_ = false;
	played_ = false;
	followed_ = false;
	tick_ = 0;
}

io::JsonValue MissionEffects::to_json() const {
	JsonValue out = JsonValue::make_object();
	size_t attached = 0, gated = 0, controller = 0, no_model = 0, emitters = 0;
	for (const MissionEffectSlot &slot : slots_) {
		const std::string status = slot.status;
		attached += status == "attached" ? 1 : 0;
		gated += status == "gated" ? 1 : 0;
		controller += status == "controller" ? 1 : 0;
		no_model += status == "no_model" ? 1 : 0;
		emitters += slot.spawned;
	}
	out.set("slots", json_number(double(slots_.size())));
	out.set("attached", json_number(double(attached)));
	out.set("gated", json_number(double(gated)));
	out.set("controller", json_number(double(controller)));
	out.set("no_model", json_number(double(no_model)));
	out.set("emitters", json_number(double(emitters)));
	out.set("tick", json_number(tick_));
	out.set("step_us", json_number(double(step_us_)));
	if (scene_) {
		const particle::EffectLiveCounts counts = scene_->live_counts();
		out.set("groups", json_number(double(counts.group_count)));
		out.set("particles", json_number(double(counts.particle_count)));
	} else {
		out.set("groups", json_number(0));
		out.set("particles", json_number(0));
	}
	// Each effect the slots name: how many slots and emitters, where the game reads its definition, and
	// whether it spawns at all (an unresolved member spawns nothing).
	JsonValue effects = JsonValue::make_array();
	std::vector<std::string> named;
	for (const MissionEffectSlot &slot : slots_) {
		bool known = false;
		for (const std::string &name : named) known = known || strutil::iequals(name, slot.effect);
		if (!known) named.push_back(slot.effect);
	}
	for (const std::string &name : named) {
		size_t count = 0, spawned = 0;
		for (const MissionEffectSlot &slot : slots_)
			if (strutil::iequals(slot.effect, name)) {
				++count;
				spawned += slot.spawned;
			}
		JsonValue row = JsonValue::make_object();
		row.set("effect", json_string(name));
		row.set("slots", json_number(double(count)));
		row.set("emitters", json_number(double(spawned)));
		// An effect no attached slot spawns (a drivable item's waiting on its driver) resolved as the game would.
		particle::EffectClosure resolved;
		const particle::EffectClosure *closure = closure_of(name);
		if (!closure) {
			resolved = catalog_files_.closure(name, particle::EffectSceneConfig());
			closure = &resolved;
		}
		row.set("defined_in", closure->found ? json_string(closure->source) : JsonValue::make_null());
		row.set("spawns", JsonValue::make_bool(closure->spawns()));
		effects.array.push_back(std::move(row));
	}
	out.set("effects", std::move(effects));
	return out;
}

} // namespace opennova::editor
