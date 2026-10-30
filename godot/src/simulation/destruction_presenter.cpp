#include "simulation/destruction_presenter.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

#include <runtime/world/present_passes.h>

#include "audio/mission_audio.h"
#include "lights/effect_light_director.h"
#include "mission/mission_data.h"
#include "render/object_lod_frame.h"
#include "simulation/entity_presenter.h"
#include "simulation/simulation.h"
#include "util/axes.h"
#include "util/string_convert.h"

namespace godot {

namespace {

// The engine's presentation identity key, as the HashMap key type.
String identity_key(int p_bms_id, int64_t p_spawn_origin, int p_wire_handle) {
	return String(opennova::world::husk_identity_key(p_bms_id, p_spawn_origin, p_wire_handle).c_str());
}

bool uses_dynamic_husk_identity(int p_bms_id, int64_t p_spawn_origin, int p_wire_handle) {
	return opennova::world::husk_identity_is_dynamic(p_bms_id, p_spawn_origin, p_wire_handle);
}

Node3D *live_node3d(const ObjectID &p_id) {
	return p_id.is_valid() ? Object::cast_to<Node3D>(ObjectDB::get_instance(p_id)) : nullptr;
}

ObjectModel *live_model(const ObjectID &p_id) {
	return p_id.is_valid() ? Object::cast_to<ObjectModel>(ObjectDB::get_instance(p_id)) : nullptr;
}

} // namespace

void DestructionPresenter::_bind_methods() {}

Simulation *DestructionPresenter::sim() const {
	return sim_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_))
			: nullptr;
}

Node3D *DestructionPresenter::container() const {
	return live_node3d(container_id_);
}

MissionAudio *DestructionPresenter::audio() const {
	return audio_id_.is_valid()
			? Object::cast_to<MissionAudio>(ObjectDB::get_instance(audio_id_))
			: nullptr;
}

EffectLightDirector *DestructionPresenter::lights() const {
	return lights_id_.is_valid()
			? Object::cast_to<EffectLightDirector>(ObjectDB::get_instance(lights_id_))
			: nullptr;
}

void DestructionPresenter::setup(EntityPresenter *p_owner, Simulation *p_sim, Node3D *p_container,
		const Ref<EntityIndex> &p_index, const Ref<MissionObjectPlacer> &p_placer,
		const Ref<ItemDatabase> &p_item_db, MissionAudio *p_audio,
		EffectLightDirector *p_lights) {
	owner_ = p_owner;
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	container_id_ = p_container != nullptr ? p_container->get_instance_id() : ObjectID();
	index_ = p_index;
	placer_ = p_placer;
	item_db_ = p_item_db;
	audio_id_ = p_audio != nullptr ? p_audio->get_instance_id() : ObjectID();
	lights_id_ = p_lights != nullptr ? p_lights->get_instance_id() : ObjectID();
}

void DestructionPresenter::teardown() {
	reset_runtime_state();
}

void DestructionPresenter::reset_runtime_state() {
	Vector<int> piece_slots;
	for (const KeyValue<int, int64_t> &kv : piece_generation_) {
		piece_slots.push_back(kv.key);
	}
	for (int slot : piece_slots) {
		free_piece_model(slot);
	}
	piece_generation_.clear();
	piece_models_.clear();

	for (const KeyValue<String, FinalOverlay> &kv : final_overlays_) {
		if (ObjectModel *entity = live_model(kv.value.entity)) {
			entity->set_final_husk_overlay(ObjectID());
		}
		if (Node3D *overlay = live_node3d(kv.value.overlay)) {
			overlay->set_visible(false);
			overlay->queue_free();
		}
	}
	final_overlays_.clear();
	pending_final_overlays_.clear();
	final_husked_.clear();
	for (const KeyValue<String, HuskRestore> &kv : husk_restore_) {
		if (ObjectModel *entity = live_model(kv.value.entity)) {
			entity->set_husk_visual(ObjectID());
		}
		husk_graft_restore(placer_.ptr(), kv.value.graft);
	}
	husk_restore_.clear();

	for (const KeyValue<String, ObjectID> &kv : husked_) {
		Node3D *husk = live_node3d(kv.value);
		if (husk != nullptr) {
			husk->set_visible(false);
			husk->queue_free();
		}
	}
	husked_.clear();
}

void DestructionPresenter::present() {
	Simulation *s = sim();
	if (s == nullptr) {
		return;
	}
	opennova::world::DestructionEvents events;
	s->drain_destruction_events(events);
	std::vector<opennova::world::DeathPieceRow> pieces;
	s->fill_death_pieces(pieces);
	present_drained(events, pieces);
}

// The events cross in mission space (x, y, z-up); every position axis-maps to
// Godot (x, z, -y) here, the fire pass's rule.
void DestructionPresenter::present_drained(const opennova::world::DestructionEvents &p_events,
		const std::vector<opennova::world::DeathPieceRow> &p_pieces) {
	for (const opennova::world::HuskSwapEvent &husk : p_events.husk_swaps) {
		apply_husk_swap(husk);
	}
	for (const opennova::world::DestructionSoundEvent &sound : p_events.sounds) {
		apply_sound(opennova::to_gd(sound.sound), mission_to_godot(sound.pos));
	}
	if (EffectLightDirector *light_director = lights()) {
		for (const opennova::world::DeathLightEvent &light : p_events.death_lights) {
			light_director->on_death_light(mission_to_godot(light.pos), light.radius);
		}
	}
	stat_debris_triangles_ += p_events.debris_triangles;
	// Sim-side rolls (S12b): the crackle EFFECT is the engine's (world::DestructionEmitters);
	// its sound rides the fire pass's drain_fire_sounds.
	stat_crackles_ += p_events.crackles;
	sync_static_husks();
	present_pieces(p_pieces);
}

Node3D *DestructionPresenter::resolve_entity_node(int p_bms_id, int64_t p_spawn_origin,
		int p_wire_handle) const {
	const bool dynamic_identity = uses_dynamic_husk_identity(
			p_bms_id, p_spawn_origin, p_wire_handle);
	if (dynamic_identity) {
		// A runtime-only owner has no authored mission identity. Never fall
		// through to the BMS/static lookup when its wire node is unavailable.
		if (owner_ == nullptr) {
			return nullptr;
		}
		return owner_->resolve_wire_handle(p_wire_handle);
	}
	if (index_.is_null()) {
		return nullptr;
	}
	ObjectModel *node = index_->resolve(p_bms_id,
			Simulation::spawn_origin_kind(p_spawn_origin),
			Simulation::spawn_origin_index(p_spawn_origin));
	return node;
}

// The husk model swap: on a per-entity node, hide the intact node's visual
// children and graft the husk model as a child (it inherits the node transform,
// so settling wrecks keep moving with the present pass). Batched statics have no
// node: the instance is carved out of its graphic's MultiMesh batches and the
// husk grafts into the mission container at the placed transform. No husk
// authored -> the intact graphic keeps standing, dead — the witnessed
// render-pick fallback (batched statics stay in their batches).
void DestructionPresenter::restore_intact(const String &key) {
	drop_final_overlay(key);
	final_husked_.erase(key);
	if (const HuskRestore *restore = husk_restore_.getptr(key)) {
		if (ObjectModel *entity = live_model(restore->entity)) {
			entity->set_husk_visual(ObjectID());
		}
		husk_graft_restore(placer_.ptr(), restore->graft);
	}
	husk_restore_.erase(key);
	if (const ObjectID *id = husked_.getptr(key)) {
		if (Node3D *husk = live_node3d(*id)) {
			husk->set_visible(false);
			husk->queue_free();
		}
	}
	husked_.erase(key);
}

std::vector<String> DestructionPresenter::husk_graphics(int p_def_id, bool p_husk_final) const {
	std::vector<String> out;
	if (item_db_.is_null()) {
		return out;
	}
	const std::string husk = opennova::to_std(item_db_->get_husk(p_def_id));
	if (!p_husk_final) {
		const std::string graphic = opennova::world::husk_render_graphic(husk);
		if (!graphic.empty()) {
			out.push_back(opennova::to_gd(graphic));
		}
		return out;
	}
	for (const std::string &graphic : opennova::world::final_husk_graphics(
				 husk, opennova::to_std(item_db_->get_huskfinal(p_def_id)))) {
		out.push_back(opennova::to_gd(graphic));
	}
	return out;
}

void DestructionPresenter::drop_final_overlay(const String &p_key) {
	if (const FinalOverlay *held = final_overlays_.getptr(p_key)) {
		if (ObjectModel *entity = live_model(held->entity)) {
			entity->set_final_husk_overlay(ObjectID());
		}
		if (Node3D *overlay = live_node3d(held->overlay)) {
			overlay->set_visible(false);
			overlay->queue_free();
		}
	}
	final_overlays_.erase(p_key);
	pending_final_overlays_.erase(p_key);
}

void DestructionPresenter::retry_pending_final_overlay(const String &p_key) {
	const opennova::world::HuskSwapEvent *pending = pending_final_overlays_.getptr(p_key);
	if (pending == nullptr) {
		return;
	}
	const opennova::world::HuskSwapEvent show = *pending;
	pending_final_overlays_.erase(p_key);
	apply_final_overlay(p_key, show);
}

// The pending final husk draws beside the husk, in the husk's place and frame
// (engine: world::update_husk_swap_stage carries the witness).
void DestructionPresenter::apply_final_overlay(const String &p_key,
		const opennova::world::HuskSwapEvent &p_husk) {
	drop_final_overlay(p_key);
	if (p_husk.final_overlay != opennova::world::HuskSwapEvent::Overlay::kShow ||
			placer_.is_null()) {
		return;
	}
	Node3D *parent = nullptr;
	// The entity model the overlay is stamped on: the node a graft hangs under, or
	// the resolved node itself; a carved static's graft hangs under the container.
	ObjectModel *entity = nullptr;
	Transform3D local;
	const ObjectID *graft_id = husked_.getptr(p_key);
	Node3D *graft = graft_id != nullptr ? live_node3d(*graft_id) : nullptr;
	if (graft != nullptr) {
		parent = Object::cast_to<Node3D>(graft->get_parent());
		local = graft->get_transform();
		if (const HuskRestore *restore = husk_restore_.getptr(p_key)) {
			entity = live_model(restore->entity);
		}
	} else {
		parent = resolve_entity_node(p_husk.bms_id, static_cast<int64_t>(p_husk.spawn_origin),
				static_cast<int>(p_husk.wire_handle));
		entity = Object::cast_to<ObjectModel>(parent);
	}
	if (parent == nullptr) {
		// The show is the stage's one event for this window: keep it until the node
		// builds and its husk grafts (apply_husk_swap retries it).
		pending_final_overlays_[p_key] = p_husk;
		return;
	}
	// The overlay draws wherever the entity draws: the reflected subscene too.
	ObjectModel *source = Object::cast_to<ObjectModel>(graft != nullptr ? graft : parent);
	const bool mirror_reflected = source != nullptr && source->get_mirror_reflected();
	const int def_id = p_husk.item_id + MissionData::ITEM_ID_OFFSET;
	for (const String &graphic : husk_graphics(def_id, true)) {
		ObjectModel *model = placer_->build_model_from_graphic(
				graphic, String(), parent, String(), String(), true);
		if (model == nullptr) {
			continue;
		}
		model->set_name("FinalHuskOverlay");
		model->set_transform(local);
		if (ObjectModel *husk = Object::cast_to<ObjectModel>(graft)) {
			model->set_destroyed_section_mask(husk->get_destroyed_section_mask());
		}
		if (mirror_reflected) {
			model->set_mirror_reflected(true);
			model->rebuild();
		}
		FinalOverlay held;
		held.overlay = model->get_instance_id();
		if (entity != nullptr) {
			held.entity = entity->get_instance_id();
			entity->set_final_husk_overlay(held.overlay);
		}
		final_overlays_[p_key] = held;
		return;
	}
}

bool DestructionPresenter::apply_husk_swap(const opennova::world::HuskSwapEvent &p_husk) {
	const int bms_id = p_husk.bms_id;
	const int64_t spawn_origin = static_cast<int64_t>(p_husk.spawn_origin);
	const int wire_handle = static_cast<int>(p_husk.wire_handle);
	const String husk_key = identity_key(bms_id, spawn_origin, wire_handle);
	if (p_husk.restore_intact) {
		restore_intact(husk_key);
		return true;
	}
	if (p_husk.final_overlay != opennova::world::HuskSwapEvent::Overlay::kNone) {
		apply_final_overlay(husk_key, p_husk);
		return true;
	}
	// A promotion ends the pending draw, whatever husk stands.
	if (p_husk.husk_final) {
		drop_final_overlay(husk_key);
	}
	// The husk already applied stands; a promotion swaps the husk for the final
	// husk (engine: world::update_husk_swap_stage).
	if (husked_.has(husk_key)) {
		if (final_husked_.has(husk_key) == p_husk.husk_final) {
			return true;
		}
		restore_intact(husk_key);
	}
	if (p_husk.husk_final) {
		final_husked_.insert(husk_key);
	}
	++stat_husk_swaps_;
	const int item_id = p_husk.item_id;
	const int def_id = item_id + MissionData::ITEM_ID_OFFSET; // wire type id -> items.def id
	// The husk (runtime/world/present_passes.h), the first candidate that builds.
	const std::vector<String> graphics = husk_graphics(def_id, p_husk.husk_final);
	if (graphics.empty() || placer_.is_null()) {
		husked_[husk_key] = ObjectID();
		++stat_no_husk_;
		return true;
	}
	Node3D *node = resolve_entity_node(bms_id, spawn_origin, wire_handle);
	if (node != nullptr) {
		// The husk grafts under the entity's own node (simulation/husk_swap.h).
		HuskRestore restore;
		restore.spawn_origin = spawn_origin;
		restore.wire_handle = wire_handle;
		for (const String &husk_graphic : graphics) {
			if (husk_graft_individual(**placer_, *node, husk_graphic, bms_id, restore.graft) ==
					HuskGraftResult::Grafted) {
				if (ObjectModel *entity = Object::cast_to<ObjectModel>(node)) {
					restore.entity = entity->get_instance_id();
					entity->set_husk_visual(restore.graft.husk);
				}
				husked_[husk_key] = restore.graft.husk;
				husk_restore_[husk_key] = restore;
				retry_pending_final_overlay(husk_key);
				return true;
			}
		}
		husked_[husk_key] = ObjectID();
		++stat_no_husk_;
		return true;
	}
	if (uses_dynamic_husk_identity(bms_id, spawn_origin, wire_handle)) {
		// A budgeted cold row can build its node later. Its retained PF_HUSK
		// pick retries then; do not cache this temporary resolution miss.
		++stat_no_husk_;
		return false;
	}
	// Batched static (world-wac-ai-re §24.6): carve the instance, graft at its
	// placed transform. An unknown bms_id (individual entity whose node is gone)
	// grafts nothing.
	Node3D *container_node = container();
	if (container_node == nullptr) {
		husked_[husk_key] = ObjectID();
		return true;
	}
	HuskRestore restore;
	restore.spawn_origin = spawn_origin;
	HuskGraftResult grafted = HuskGraftResult::NoModel;
	for (const String &husk_graphic : graphics) {
		grafted = husk_graft_static(**placer_, *container_node, bms_id, husk_graphic, restore.graft);
		if (grafted != HuskGraftResult::NoModel) {
			break;
		}
	}
	if (grafted != HuskGraftResult::Grafted) {
		husked_[husk_key] = ObjectID();
		if (grafted == HuskGraftResult::NoModel) {
			++stat_no_husk_;
		}
		return true;
	}
	husk_restore_[husk_key] = restore;
	husked_[husk_key] = restore.graft.husk;
	retry_pending_final_overlay(husk_key);
	return true;
}

// Node-less wrecks still move while death physics settles them. Resolve the
// same compact present pose consumed by the other shell presentation paths.
Variant DestructionPresenter::present_transform_for_identity(int p_bms_id,
		int64_t p_spawn_origin) const {
	Simulation *s = sim();
	if (s == nullptr) {
		return Variant();
	}
	PackedVector3Array state;
	if (p_bms_id > 0) {
		state = s->get_present_effect_state_for_bms_id(p_bms_id);
	}
	if (state.size() != Simulation::EFFECT_STATE_COUNT &&
			p_spawn_origin != static_cast<int64_t>(Simulation::SPAWN_ORIGIN_NONE)) {
		state = s->get_present_effect_state_for_origin(
				Simulation::spawn_origin_kind(p_spawn_origin),
				Simulation::spawn_origin_index(p_spawn_origin));
	}
	if (state.size() != Simulation::EFFECT_STATE_COUNT) {
		return Variant();
	}
	const Vector3 rotation_deg = state[Simulation::EFFECT_STATE_ROTATION_DEG];
	Basis basis = bms_to_godot_basis(rotation_deg);
	// Compact peer poses carry yaw only. Static death motion changes position but
	// not orientation, so retain the exact authored basis carved from the batch
	// when pitch/roll are unavailable. Host/listen poses carry the full Euler
	// angles and take the live-basis path above (the rule is the engine's:
	// runtime/world/present_passes.h husk_settle_keeps_carved_tilt).
	if (opennova::world::husk_settle_keeps_carved_tilt(rotation_deg.x, rotation_deg.z)) {
		const String husk_key = identity_key(p_bms_id, p_spawn_origin, -1);
		if (const HuskRestore *restore = husk_restore_.getptr(husk_key)) {
			if (restore->graft.kind == HuskGraft::STATIC) {
				basis = restore->graft.placed_transform.basis;
			}
		}
	}
	return Transform3D(basis, state[Simulation::EFFECT_STATE_POSITION]);
}

void DestructionPresenter::sync_static_husks() {
	for (const KeyValue<String, HuskRestore> &kv : husk_restore_) {
		const HuskRestore &restore = kv.value;
		if (restore.graft.kind != HuskGraft::STATIC) {
			continue;
		}
		const ObjectID *graft_id = husked_.getptr(kv.key);
		Node3D *graft = graft_id != nullptr ? live_node3d(*graft_id) : nullptr;
		if (graft == nullptr) {
			continue;
		}
		const Variant live_v =
				present_transform_for_identity(restore.graft.bms_id, restore.spawn_origin);
		if (live_v.get_type() == Variant::TRANSFORM3D) {
			const Transform3D live = live_v;
			// The husk registration set the replacement once; this per-frame
			// sync only re-pushes on an actual transform change.
			if (graft->get_transform().is_equal_approx(live)) {
				continue;
			}
			graft->set_transform(live);
			if (const FinalOverlay *held = final_overlays_.getptr(kv.key)) {
				if (Node3D *overlay = live_node3d(held->overlay)) {
					overlay->set_transform(live);
				}
			}
			if (placer_.is_valid()) {
				placer_->set_static_terrain_shadow_replacement(restore.graft.bms_id,
						restore.graft.husk_graphic, live, restore.graft.casts_static_shadow);
			}
		}
	}
}

void DestructionPresenter::apply_sound(const String &p_name, const Vector3 &p_pos) {
	MissionAudio *audio_node = audio();
	if (audio_node == nullptr) {
		return;
	}
	if (p_name.is_empty()) {
		return;
	}
	audio_node->fire_soundset(p_name, p_pos, 0);
	++stat_sounds_;
}

// Death pieces: the sim owns positions/physics and each piece's trail (the engine's
// world::DestructionEmitters); each live piece carries its own piece model, which the frame's
// piece draws place and show (apply_piece_draws).
void DestructionPresenter::present_pieces(const std::vector<opennova::world::DeathPieceRow> &p_pieces) {
	stat_pieces_peak_ = MAX(stat_pieces_peak_, static_cast<int64_t>(p_pieces.size()));
	HashSet<int> seen;
	for (const opennova::world::DeathPieceRow &piece : p_pieces) {
		const int slot = piece.slot;
		if (slot < 0) {
			continue;
		}
		seen.insert(slot);
		const int64_t generation = static_cast<int64_t>(piece.generation);
		const int64_t *presented = piece_generation_.getptr(slot);
		const bool is_new_generation = (presented != nullptr ? *presented : -1) != generation;
		if (is_new_generation) {
			if (presented != nullptr) {
				free_piece_model(slot);
			}
			piece_generation_[slot] = generation;
			ObjectModel *model = build_piece_model(slot, piece.item_id);
			piece_models_[slot] = model != nullptr ? model->get_instance_id() : ObjectID();
		}
	}
	Vector<int> retired;
	for (const KeyValue<int, int64_t> &kv : piece_generation_) {
		if (!seen.has(kv.key)) {
			retired.push_back(kv.key);
		}
	}
	for (int slot : retired) {
		free_piece_model(slot);
		piece_generation_.erase(slot);
	}
}

// One piece slot's model: the def's piece graphic, the LOADED final husk model
// (the husk's "xb" variant, else huskFinal) else the husk model (engine:
// runtime/world/present_passes.h final_husk_graphics; the spawn stores that
// pointer at piece+0).
// Every level is retained and the draws select it; the reflection draws it too
// (retail Water_RenderReflectedWorldScene @ 0x5c85b7..0x5c85c1 runs the same
// collect and draw at the shipped water detail). Hidden until a draw shows it.
ObjectModel *DestructionPresenter::build_piece_model(int p_slot, int p_item_id) {
	Node3D *parent = container();
	if (item_db_.is_null() || placer_.is_null() || parent == nullptr) {
		return nullptr;
	}
	const int def_id = p_item_id + MissionData::ITEM_ID_OFFSET; // wire type id -> items.def id
	// The final husk the load took (its candidates), else the husk.
	std::vector<String> graphics = husk_graphics(def_id, true);
	const std::vector<String> husk = husk_graphics(def_id, false);
	graphics.insert(graphics.end(), husk.begin(), husk.end());
	ObjectModel *model = nullptr;
	for (const String &graphic : graphics) {
		model = placer_->build_model_from_graphic(graphic, String(), parent, String(), String(), true);
		if (model != nullptr) {
			break;
		}
	}
	if (model == nullptr) {
		return nullptr;
	}
	model->set_name(vformat("DeathPiece_%d", p_slot));
	model->set_presenter_driven_lod(true);
	model->set_mirror_reflected(true);
	model->rebuild();
	model->set_present_visible(false);
	return model;
}

void DestructionPresenter::free_piece_model(int p_slot) {
	if (ObjectModel *model = piece_model(p_slot)) {
		model->set_visible(false);
		model->queue_free();
	}
	piece_models_.erase(p_slot);
}

ObjectModel *DestructionPresenter::piece_model(int p_slot) const {
	const ObjectID *id = piece_models_.getptr(p_slot);
	return id != nullptr ? Object::cast_to<ObjectModel>(live_node3d(*id)) : nullptr;
}

// The section draw (engine: runtime/world/death_piece_draw.h): EulerScale(piece pose,
// ftol(scale * 65536)) * T(-centre of the drawn section's COBJ row), every
// bone matrix the same. The pose converts like every entity's (the BAM heading
// is 90 - the mission yaw); the COBJ centre is in the model's own axes, (x, y,
// z) -> the model node's (y, z, x) (ObjectLodFrame::cobj_center_local).
Transform3D DestructionPresenter::piece_draw_transform(
		const opennova::world::DeathPieceDraw &p_draw) {
	const Basis basis = bms_to_godot_basis(
			Vector3(p_draw.pitch, 90.0f - p_draw.heading, p_draw.roll))
								.scaled(Vector3(p_draw.scale, p_draw.scale, p_draw.scale));
	Transform3D transform(basis, mission_to_godot(p_draw.pos));
	if (p_draw.pivoted) {
		const Vector3 centre = ObjectLodFrame::cobj_center_local(p_draw.pivot_q16);
		transform = transform * Transform3D(Basis(), -centre);
	}
	return transform;
}

// The piece draw's device leg: the drawn level, the collapsed sections (bone
// matrix w = 0, retail @ 0x57b7d4..0x57b7e7) as the model's hidden sections,
// and the section matrix. Retail submits the piece only on the frames its
// collect admits it, so a model no row names this frame hides.
void DestructionPresenter::apply_piece_draws(
		const std::vector<opennova::world::DeathPieceDraw> &p_draws) {
	HashSet<int> drawn;
	for (const opennova::world::DeathPieceDraw &draw : p_draws) {
		const int64_t *generation = piece_generation_.getptr(draw.slot);
		if (generation == nullptr || *generation != static_cast<int64_t>(draw.generation)) {
			continue;
		}
		ObjectModel *model = piece_model(draw.slot);
		if (model == nullptr) {
			continue;
		}
		model->set_active_lod(draw.lod_level);
		model->set_destroyed_section_mask(draw.hidden_mask);
		model->set_transform(piece_draw_transform(draw));
		model->set_present_visible(true);
		model->set_occlusion_hidden(false);
		drawn.insert(draw.slot);
	}
	// A piece this view's collect did not draw is the view's verdict (the
	// occlusion bit), so the weapon Inset's own collect can still draw it
	// (apply_piece_draws_inset).
	for (const KeyValue<int, ObjectID> &kv : piece_models_) {
		if (drawn.has(kv.key)) {
			continue;
		}
		if (ObjectModel *model = piece_model(kv.key)) {
			model->set_occlusion_hidden(true);
		}
	}
}

// The weapon Inset pass's own piece draws: its collect runs the piece
// collect again over the Inset camera, and its draws carry their own level
// (the engine's collect_death_piece_draws carries the witness). A piece the
// Inset draws takes that level as the model's Inset state, drawn by its twin
// where the main view's differs; a piece it does not draw is hidden there.
void DestructionPresenter::apply_piece_draws_inset(
		const std::vector<opennova::world::DeathPieceDraw> &p_draws) {
	HashSet<int> drawn;
	for (const opennova::world::DeathPieceDraw &draw : p_draws) {
		const int64_t *generation = piece_generation_.getptr(draw.slot);
		if (generation == nullptr || *generation != static_cast<int64_t>(draw.generation)) {
			continue;
		}
		ObjectModel *model = piece_model(draw.slot);
		if (model == nullptr) {
			continue;
		}
		// The piece's own pose and collapsed sections are the piece's state,
		// not the view's: a piece only the Inset draws still needs them.
		model->set_destroyed_section_mask(draw.hidden_mask);
		model->set_transform(piece_draw_transform(draw));
		model->set_present_visible(true);
		model->set_inset_view_lod(draw.lod_level);
		model->set_inset_occlusion_hidden(false);
		drawn.insert(draw.slot);
	}
	for (const KeyValue<int, ObjectID> &kv : piece_models_) {
		if (drawn.has(kv.key)) {
			continue;
		}
		if (ObjectModel *model = piece_model(kv.key)) {
			model->set_inset_occlusion_hidden(true);
		}
	}
}

// A water mirror's own piece draws: the reflected subscene collects the pieces
// again over the view it draws from (engine inmatch/occlusion_collect.h
// MirrorViewLists), with their own level. A piece the mirror draws takes that
// level as the model's mirror state, drawn by its twin where the node's
// differs; a piece it does not draw is hidden there.
void DestructionPresenter::apply_piece_draws_mirror(int p_view,
		const std::vector<opennova::world::DeathPieceDraw> &p_draws) {
	HashSet<int> drawn;
	for (const opennova::world::DeathPieceDraw &draw : p_draws) {
		const int64_t *generation = piece_generation_.getptr(draw.slot);
		if (generation == nullptr || *generation != static_cast<int64_t>(draw.generation)) {
			continue;
		}
		ObjectModel *model = piece_model(draw.slot);
		if (model == nullptr) {
			continue;
		}
		model->set_destroyed_section_mask(draw.hidden_mask);
		model->set_transform(piece_draw_transform(draw));
		model->set_present_visible(true);
		model->set_mirror_view_lod(p_view, draw.lod_level);
		model->set_mirror_occlusion_hidden(p_view, false);
		drawn.insert(draw.slot);
	}
	for (const KeyValue<int, ObjectID> &kv : piece_models_) {
		if (drawn.has(kv.key)) {
			continue;
		}
		if (ObjectModel *model = piece_model(kv.key)) {
			model->set_mirror_occlusion_hidden(p_view, true);
		}
	}
}

Ref<DestructionPresentStats> DestructionPresenter::get_stats() const {
	Ref<DestructionPresentStats> stats;
	stats.instantiate();
	stats->husk_swaps = stat_husk_swaps_;
	stats->no_husk = stat_no_husk_;
	stats->pieces_peak = stat_pieces_peak_;
	stats->debris_triangles = stat_debris_triangles_;
	stats->sounds = stat_sounds_;
	stats->crackles = stat_crackles_;
	return stats;
}

} // namespace godot
