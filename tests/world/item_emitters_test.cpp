// The world's effect producers in the shared scene (ADR 0077 R2): slot A, the mission start's
// particlefx walk and its gates, the PlayerControl claim edges, the particle switch's retry and
// the +0x1CC re-pose (world::ItemEmitterSystem); the destruction rows' handles (the transients,
// the death banks and their re-pose, the vehicle smoke in +0x1CC and the fire's mover re-pose,
// the death-piece trails; world::DestructionEmitters); the flying rounds' move emitters
// (world::RoundEmitters); and the scene's group re-pose every holder uses
// (particle::EffectScene::move_group). Each producer moves the handle it holds from its row's
// state, as retail's do: nothing in the scene follows an owner.
// [orig: Entity_SpawnBoneTrailEffect @ 0x43BEF0; Game_ResolveItemMaterialsAndSpawnBoneTrails
//  @ 0x522EE0; CEffect_UpdateEmitterTransform @ 0x5F7410; Entity_UpdateDeadWreckEffects
//  @ 0x493140; Entity_ProcessDeathPiecePhysics @ 0x492DD0; Projectile_UpdatePhysics @ 0x4E9D70]

#include <runtime/particle/effect_scene.h>
#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/destruction.h>
#include <runtime/world/destruction_emitters.h>
#include <runtime/world/item_effects.h>
#include <runtime/world/present_drains.h>
#include <runtime/world/round_emitters.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/world.h>

#include <formats/def/def.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace opennova::world;
namespace p = opennova::particle;

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                       \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

bool near(float a, float b) {
	return std::fabs(a - b) < 0.001f;
}

bool near(const p::Vec3 &a, const p::Vec3 &b) {
	return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

p::Vec3 unit(const p::Vec3 &v) {
	const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	return length > 0.0f ? p::Vec3{ v.x / length, v.y / length, v.z / length } : v;
}

// A catalog holding each named effect over one forever-emitting particle.
std::shared_ptr<p::EffectScene> make_scene(const std::vector<std::string> &effects) {
	p::EffectSceneConfig config;
	config.documents.push_back({ "emitters.ptl", {} });
	p::ParticleDef particle;
	particle.id = "emitter_particle";
	particle.flags = p::particle_flag::ForeverEmit;
	particle.emit_burst = 1;
	particle.emit_rate = 20;
	particle.age = 0.05f;
	config.documents[0].file.particles.push_back(particle);
	for (const std::string &name : effects)
		config.documents[0].file.effects.push_back({ name, { "emitter_particle" } });
	auto scene = std::make_shared<p::EffectScene>();
	scene->open(config);
	return scene;
}

std::vector<p::EffectGroupDebugSnapshot> groups_named(
		const p::EffectScene &scene, const std::string &name, bool live_only = true) {
	std::vector<p::EffectGroupDebugSnapshot> out;
	for (const auto &row : scene.inspect(false).groups)
		if (row.effect_name == name && (!live_only || !row.detached))
			out.push_back(row);
	return out;
}

std::optional<p::EffectGroupDebugSnapshot> group_by_id(
		const p::EffectScene &scene, p::EffectGroupId id) {
	for (const auto &row : scene.inspect(false).groups)
		if (row.id == id)
			return row;
	return std::nullopt;
}

// A model point (mission-local 16.16 words) and its direction.
ModelUserPoint point(
		const char *name, int32_t x, int32_t y, int32_t z, int32_t dx, int32_t dy, int32_t dz) {
	ModelUserPoint out;
	out.name = name;
	out.pos_q16[0] = x;
	out.pos_q16[1] = y;
	out.pos_q16[2] = z;
	out.dir_q16[0] = dx;
	out.dir_q16[1] = dy;
	out.dir_q16[2] = dz;
	return out;
}

// One row of `kind` in its pool, its def resolved with `attrib`, at `position`.
EntityHandle place(World &w, EntityKind kind, int32_t item_id, uint32_t attrib, Vec3 position) {
	Entity e;
	e.kind = kind;
	e.item_id = item_id;
	e.has_item_def = true;
	e.item_type_index = 1;
	e.item_attrib = attrib;
	e.has_graphic_model = true;
	e.position = position;
	const int pool = kind == EntityKind::Item ? 1 : (kind == EntityKind::Building ? 2 : 3);
	return w.registry.spawn(pool, e);
}

// The def's particlefx, its graphic model's points and the mask its userpoint name sets.
void author_slot_a(World &w, int32_t item_id, const char *effect, uint16_t mask,
		std::vector<ModelUserPoint> points, bool model_loaded = true) {
	ItemDeathTraits traits;
	traits.particlefx = effect;
	traits.model_loaded = model_loaded;
	traits.particlefx_mask = mask;
	traits.graphic_userpoints = std::move(points);
	for (size_t i = 0; i < traits.graphic_userpoints.size() && i < 16; ++i) {
		if ((mask & (1u << i)) == 0)
			continue;
		traits.has_particlefx_point = true;
		for (int axis = 0; axis < 3; ++axis) {
			traits.particlefx_point_q16[axis] = traits.graphic_userpoints[i].pos_q16[axis];
			traits.particlefx_direction_q16[axis] = traits.graphic_userpoints[i].dir_q16[axis];
		}
		break;
	}
	w.tables.item_death_traits.set(item_id, traits);
}

std::unique_ptr<World> make_world() {
	auto w = std::make_unique<World>();
	w->registry.configure_pool(1, 16);
	w->registry.configure_pool(2, 16);
	w->registry.configure_pool(3, 16);
	return w;
}

void open(World &w, const std::shared_ptr<p::EffectScene> &scene) {
	w.item_emitters.bind_scene(scene);
	w.item_emitters.open_producers(w, {});
}

// --- particle::EffectScene::move_group ------------------------------------------------------

// The re-pose moves every emitter of a live group; a direction re-aims it, none keeps its aim;
// a detached group, and every group while spawning is off, is not moved.
// [orig: CEffectEmitter_UpdatePositionAndParams @ 0x5F6810; CEffect_UpdateEmitterTransform
//  @ 0x5F7410]
void test_move_group_re_poses_a_live_group() {
	auto scene = make_scene({ "Puff" });
	p::EffectSpawnRequest request;
	request.effect = scene->intern("Puff");
	request.pose = p::descriptor_pose({ 1, 2, 3 }, { 0, 0, 1 });
	request.admission = p::EffectAdmission::StoreOwned;
	request.slot = { 77 };
	const auto receipt = scene->spawn(request);
	CHECK(receipt.spawned());
	CHECK(scene->move_slot({ 77 }, { 4, 5, 6 }));
	p::EffectPose pose;
	CHECK(scene->group_pose(receipt.group, pose));
	CHECK(near(pose.position, { 4, 5, 6 }));
	CHECK(near(pose.forward, { 0, 0, 1 }));
	const p::Vec3 up{ 0, 1, 0 };
	CHECK(scene->move_group(receipt.group, { 7, 8, 9 }, &up));
	CHECK(scene->group_pose(receipt.group, pose));
	CHECK(near(pose.position, { 7, 8, 9 }) && near(pose.forward, up));
	for (const auto &emitter : scene->inspect(false).groups.front().emitters)
		CHECK(near(emitter.position, { 7, 8, 9 }));
	scene->set_spawn_enabled(false);
	CHECK(!scene->move_group(receipt.group, { 0, 0, 0 }));
	scene->set_spawn_enabled(true);
	scene->detach(receipt.group);
	CHECK(!scene->move_group(receipt.group, { 0, 0, 0 }));
	CHECK(!scene->move_slot({ 78 }, { 0, 0, 0 }));
}

// --- slot A -------------------------------------------------------------------------------

// Every masked point of the def's model spawns a world-bound emitter at the point through the
// entity's matrix, along its direction, tagged; each is stored in the slot, the last remaining.
// [orig: Entity_SpawnBoneTrailEffect @ 0x43BEF0 - @ 0x43C02E / @ 0x43C053, the store @ 0x43C074]
void test_slot_a_spawns_every_masked_point_and_keeps_the_last() {
	auto w = make_world();
	auto scene = make_scene({ "Exhaust" });
	author_slot_a(*w, 40, "Exhaust", 0x5,
			{ point("FX00", 2 << 16, 0, 1 << 16, 0, 0, 1 << 16), point("x", 0, 0, 0, 0, 0, 0),
					point("FX00", 0, 3 << 16, 0, 1 << 16, 0, 0) });
	const EntityHandle h = place(*w, EntityKind::Item, 40, 0, { 10, 20, 30 });
	w->registry.get(h)->heading = placed_heading_bam(90);
	open(*w, scene);
	const auto rows = groups_named(*scene, "Exhaust");
	CHECK(rows.size() == 2);
	const Entity &e = *w->registry.get(h);
	const CollisionMatrix matrix = entity_placement_matrix(e);
	const int32_t first[3] = { 2 << 16, 0, 1 << 16 };
	const int32_t second[3] = { 0, 3 << 16, 0 };
	const int32_t second_dir[3] = { 1 << 16, 0, 0 };
	if (rows.size() == 2) {
		CHECK(near(rows[0].pose.position, effect_matrix_point(matrix, first)));
		CHECK(near(rows[1].pose.position, effect_matrix_point(matrix, second)));
		CHECK(near(rows[1].pose.forward, unit(effect_matrix_direction(matrix, second_dir))));
		CHECK(rows[0].binding == p::EffectBinding::World);
		CHECK(rows[0].admission == p::EffectAdmission::StoreOwned);
		CHECK(rows[0].section_gate.tagged && rows[1].section_gate.tagged);
		CHECK(scene->slot_group(entity_effect_slot(h.packed)) == rows[1].id);
	}
	CHECK(w->item_emitters.counts().attached == 1);
	CHECK(w->item_emitters.counts().held_slots == 1);
}

// No masked point (or no graphic model) takes one emitter at the entity's position, its
// descriptor unoriented (aimed up), stored nowhere.
// [orig: Entity_SpawnBoneTrailEffect @ 0x43BF49 / @ 0x43C097 -> Effect_SubmitDescriptor
//  @ 0x43C0A4; Effect_SubmitDescriptor @ 0x5F6F80 hands no orientation]
void test_slot_a_origin_leg_is_unoriented_and_unstored() {
	auto w = make_world();
	auto scene = make_scene({ "Flies" });
	author_slot_a(*w, 41, "Flies", 0, { point("x", 1 << 16, 0, 0, 0, 0, 1 << 16) });
	author_slot_a(*w, 42, "Flies", 0, {}, false);
	const EntityHandle masked_none = place(*w, EntityKind::Item, 41, 0, { 1, 2, 3 });
	const EntityHandle model_less = place(*w, EntityKind::Item, 42, 0, { -5, 6, 7 });
	w->registry.get(model_less)->has_graphic_model = false;
	open(*w, scene);
	const auto rows = groups_named(*scene, "Flies");
	CHECK(rows.size() == 2);
	for (const auto &row : rows) {
		CHECK(near(row.pose.forward, { 0, 1, 0 }));
		CHECK(row.admission == p::EffectAdmission::Always);
		CHECK(row.section_gate.tagged);
	}
	if (rows.size() == 2) {
		CHECK(near(rows[0].pose.position, effect_point({ 1, 2, 3 })));
		CHECK(near(rows[1].pose.position, effect_point({ -5, 6, 7 })));
	}
	CHECK(!scene->slot_group(entity_effect_slot(masked_none.packed)));
	CHECK(!scene->slot_group(entity_effect_slot(model_less.packed)));
}

// The walk's per-pool gates: pool 1 skips attrib 0x42, pools 2 and 3 skip the powerup bit; a
// PlayerControl item waits for its controller; a walked lifetime never attaches twice.
// [orig: Game_ResolveItemMaterialsAndSpawnBoneTrails @ 0x522EE0, gates @ 0x523233 / @ 0x523272 /
//  @ 0x5232AF]
void test_mission_walk_keeps_the_pool_gates_and_attaches_once() {
	using namespace opennova::def;
	auto w = make_world();
	auto scene = make_scene({ "Puff" });
	for (int32_t id : { 50, 51, 52, 53, 54, 55 })
		author_slot_a(*w, id, "Puff", 0, {}, false);
	place(*w, EntityKind::Item, 50, 0, { 0, 0, 0 });
	const EntityHandle drivable =
			place(*w, EntityKind::Item, 51, DEF_ITEM_ATTRIB_PLAYERCONTROL, { 1, 0, 0 });
	place(*w, EntityKind::Item, 52, DEF_ITEM_ATTRIB_POWERUP, { 2, 0, 0 });
	place(*w, EntityKind::Building, 53, DEF_ITEM_ATTRIB_PLAYERCONTROL, { 3, 0, 0 });
	place(*w, EntityKind::Building, 54, DEF_ITEM_ATTRIB_POWERUP, { 4, 0, 0 });
	place(*w, EntityKind::Marker, 55, 0, { 5, 0, 0 });
	open(*w, scene);
	CHECK(groups_named(*scene, "Puff").size() == 3);
	CHECK(w->item_emitters.counts().attached == 3);
	// The walk's entry again (a joiner's stream naming the same lifetime) attaches nothing.
	CHECK(w->item_emitters.attach_entity_effects(*w, *w->registry.get(drivable)) == 0);
	for (int slot = 0; slot < 3; ++slot) {
		if (const Entity *e = w->registry.get(EntityHandle::make(1, slot)))
			w->item_emitters.attach_entity_effects(*w, *e);
	}
	CHECK(groups_named(*scene, "Puff").size() == 3);
}

// The PlayerControl claim edges: the claim spawns slot A, the claimant's leaving releases the
// slot (D-PTL-17: every PlayerControl item).
// [orig: Entity_UpdateHeloRotorSpin @ 0x48FA70 - @ 0x48FB0C; Entity_DetachFromVehicle
//  @ 0x435746..0x435759]
void test_controller_edges_spawn_and_release_slot_a() {
	using namespace opennova::def;
	auto w = make_world();
	auto scene = make_scene({ "Exhaust" });
	author_slot_a(*w, 60, "Exhaust", 0x1, { point("FX00", 1 << 16, 0, 0, 0, 0, 1 << 16) });
	const EntityHandle h =
			place(*w, EntityKind::Item, 60, DEF_ITEM_ATTRIB_PLAYERCONTROL, { 0, 0, 0 });
	open(*w, scene);
	CHECK(groups_named(*scene, "Exhaust").empty());
	w->item_emitters.control_started(*w, *w->registry.get(h));
	CHECK(groups_named(*scene, "Exhaust").size() == 1);
	CHECK(w->item_emitters.counts().controlled == 1);
	w->item_emitters.control_stopped(*w, *w->registry.get(h));
	CHECK(groups_named(*scene, "Exhaust").empty());
	CHECK(w->item_emitters.counts().controlled == 0);
	// A claimed item at the walk takes it there.
	Entity rider;
	rider.kind = EntityKind::Organic;
	w->registry.configure_pool(0, 4);
	w->registry.get(h)->primary_occupant = w->registry.spawn(0, rider);
	open(*w, scene);
	CHECK(groups_named(*scene, "Exhaust").size() == 1);
}

// The retail master particle switch refuses the attach; its lift retries it once.
void test_hidden_attach_retries_once() {
	auto w = make_world();
	auto scene = make_scene({ "Puff" });
	author_slot_a(*w, 70, "Puff", 0, {}, false);
	place(*w, EntityKind::Item, 70, 0, { 0, 0, 0 });
	scene->set_spawn_enabled(false);
	open(*w, scene);
	CHECK(scene->live_counts().group_count == 0);
	CHECK(w->item_emitters.counts().pending_hidden == 1);
	CHECK(w->item_emitters.retry_hidden(*w) == 0);
	scene->set_spawn_enabled(true);
	CHECK(w->item_emitters.retry_hidden(*w) == 1);
	CHECK(w->item_emitters.retry_hidden(*w) == 0);
	CHECK(groups_named(*scene, "Puff").size() == 1);
	CHECK(w->item_emitters.counts().pending_hidden == 0);
}

// The +0x1CC re-pose: the slot's group goes to the def's first masked point (and its
// direction) through the entity's matrix, else to the entity's position with its aim kept; a
// watercraft class keeps it at or above the water.
// [orig: CEffect_UpdateEmitterTransform @ 0x5F7410]
void test_slot_re_pose_follows_the_first_masked_point() {
	auto w = make_world();
	auto scene = make_scene({ "Exhaust", "Smoke" });
	author_slot_a(*w, 80, "Exhaust", 0x6,
			{ point("x", 0, 0, 0, 0, 0, 0), point("FX00", 2 << 16, 0, 0, 0, 1 << 16, 0),
					point("FX00", 0, 0, 5 << 16, 1 << 16, 0, 0) });
	const EntityHandle h = place(*w, EntityKind::Item, 80, 0, { 10, 0, 0 });
	open(*w, scene);
	Entity &e = *w->registry.get(h);
	e.position = Vec3{ 20, 5, 1 };
	w->item_emitters.update_emitter_transform(*w, e);
	const auto slot = scene->slot_group(entity_effect_slot(h.packed));
	const auto row = group_by_id(*scene, slot);
	CHECK(row.has_value());
	const CollisionMatrix matrix = entity_placement_matrix(e);
	const int32_t first[3] = { 2 << 16, 0, 0 };
	const int32_t first_dir[3] = { 0, 1 << 16, 0 };
	if (row) {
		CHECK(near(row->pose.position, effect_matrix_point(matrix, first)));
		CHECK(near(row->pose.forward, unit(effect_matrix_direction(matrix, first_dir))));
	}
	// No masked point: the position leg, the aim kept; a watercraft class clamps to the water.
	ItemDeathTraits boat;
	boat.particlefx = "Smoke";
	boat.unit_type = 7;
	w->tables.item_death_traits.set(81, boat);
	const EntityHandle b = place(*w, EntityKind::Item, 81, 0, { 0, 0, -3 });
	w->env.water_z = 2 << 16;
	Entity &boat_row = *w->registry.get(b);
	w->item_emitters.store_damage_smoke(*w, boat_row, "Smoke", boat_row.position);
	const auto before = group_by_id(*scene, scene->slot_group(entity_effect_slot(b.packed)));
	w->item_emitters.update_emitter_transform(*w, boat_row);
	const auto after = group_by_id(*scene, scene->slot_group(entity_effect_slot(b.packed)));
	CHECK(before.has_value() && after.has_value());
	if (before && after) {
		CHECK(near(after->pose.position, { 0, 2, 0 }));
		CHECK(near(after->pose.forward, before->pose.forward));
	}
	// The smoke is stored only while the slot is empty [orig: @ 0x48B0DE..0x48B0E5].
	w->item_emitters.store_damage_smoke(*w, e, "Smoke", e.position);
	CHECK(scene->slot_group(entity_effect_slot(h.packed)) == slot);
}

// The pool walks' gate: a row whose def binds the null move_function row runs no re-pose.
void test_emitter_update_runs_past_a_non_null_update_callback() {
	auto w = make_world();
	Entity e;
	e.item_type_index = 1;
	CHECK(emitter_update_runs(*w, e));
	w->tables.item_rows.resize(3);
	w->tables.item_rows[1].move_row = PhysicsClass::Null;
	w->tables.item_rows[2].move_row = PhysicsClass::Cveh;
	CHECK(!emitter_update_runs(*w, e));
	e.item_type_index = 2;
	CHECK(emitter_update_runs(*w, e));
	e.item_type_index = 0;
	CHECK(!emitter_update_runs(*w, e));
}

// --- destruction rows ------------------------------------------------------------------------

DestructionEffectEvent bank_row(
		const Entity &e, uint8_t family, uint8_t slot, const char *effect, Vec3 pos) {
	DestructionEffectEvent row;
	row.effect = effect;
	row.pos = pos;
	row.family = family;
	row.bank_slot = slot;
	row.attach_net_id = e.net_id;
	row.attach_bms_id = e.bms_id;
	row.attach_wire_handle = e.handle.packed;
	row.attach_spawn_origin = e.spawn_origin;
	row.section_tagged = true;
	return row;
}

// A transient row spawns where it stands and is forgotten; a bank row holds its handle, which
// the wreck pass re-poses (position only) and the stop releases; a row of a destroyed row's
// identity is released at the tick's end.
// [orig: Entity_InitDeathSounds @ 0x4939B0; Entity_UpdateDeadWreckEffects @ 0x493140 - the
//  re-poses @ 0x49321E / @ 0x4932FD / @ 0x4933BF]
void test_destruction_rows_spawn_hold_re_pose_and_release() {
	auto w = make_world();
	auto scene = make_scene({ "Debris", "Fire" });
	open(*w, scene);
	const EntityHandle h = place(*w, EntityKind::Item, 90, 0, { 0, 0, 0 });
	const Entity &e = *w->registry.get(h);
	DestructionEffectEvent debris;
	debris.effect = "Debris";
	debris.pos = { 1, 2, 3 };
	debris.dir = { 0, 0, 1 };
	w->out.destruction.effects.push_back(debris);
	w->out.destruction.effects.push_back(bank_row(e, 2, 1, "Fire", { 4, 5, 6 }));
	w->destruction_emitters.sync(*w);
	CHECK(w->out.destruction.effects.empty());
	const auto transient = groups_named(*scene, "Debris");
	CHECK(transient.size() == 1);
	if (transient.size() == 1) {
		CHECK(near(transient[0].pose.position, effect_point({ 1, 2, 3 })));
		CHECK(near(transient[0].pose.forward, { 0, 1, 0 }));
		CHECK(!transient[0].section_gate.tagged);
	}
	const p::EffectSlotToken fire = destruction_effect_slot(h.packed, 2, 1);
	const p::EffectGroupId held = scene->slot_group(fire);
	CHECK(static_cast<bool>(held));
	CHECK(w->destruction_emitters.held() == 1);
	p::EffectPose spawn_pose;
	CHECK(scene->group_pose(held, spawn_pose));
	w->destruction_emitters.move_bank(*w, e, 2, 1, { 7, 8, 9 });
	p::EffectPose moved;
	CHECK(scene->group_pose(held, moved));
	CHECK(near(moved.position, effect_point({ 7, 8, 9 })));
	CHECK(near(moved.forward, spawn_pose.forward));
	DestructionEffectEvent stop = bank_row(e, 2, 1, "", {});
	stop.release = true;
	w->out.destruction.effects.push_back(stop);
	w->destruction_emitters.sync(*w);
	CHECK(!scene->slot_group(fire));
	CHECK(w->destruction_emitters.held() == 0);
	// A held bank of a row destroyed since goes with it.
	w->out.destruction.effects.push_back(bank_row(e, 1, 0, "Fire", { 0, 0, 0 }));
	w->destruction_emitters.sync(*w);
	const p::EffectGroupId dead = scene->slot_group(destruction_effect_slot(h.packed, 1, 0));
	CHECK(static_cast<bool>(dead));
	w->registry.despawn(h);
	w->destruction_emitters.sync(*w);
	const auto gone = group_by_id(*scene, dead);
	CHECK(!gone || gone->detached);
	// Closed producers leave the rows to the embedder's drain.
	w->item_emitters.close_producers();
	w->out.destruction.effects.push_back(debris);
	w->destruction_emitters.sync(*w);
	CHECK(w->out.destruction.effects.size() == 1);
}

// The vehicle's heavy-damage smoke is its +0x1CC handle; the critical fire (+0x400) is re-posed
// at its mover's head to the vehicle's position, aimed along its matrix's third row.
// [orig: Entity_UpdateVehiclePhysics @ 0x48B0DE..0x48B0F6, @ 0x48B114]
void test_vehicle_smoke_and_fire_handles() {
	auto w = make_world();
	auto scene = make_scene({ "Effect_smkSigB", "Effect_vehicleFireMed" });
	open(*w, scene);
	const EntityHandle h = place(*w, EntityKind::Item, 91, 0, { 3, 4, 5 });
	Entity &e = *w->registry.get(h);
	w->out.destruction.effects.push_back(bank_row(e, 4, 0, "Effect_smkSigB", { 3, 4, 5 }));
	w->out.destruction.effects.push_back(bank_row(e, 5, 0, "Effect_vehicleFireMed", { 3, 4, 5 }));
	w->destruction_emitters.sync(*w);
	CHECK(groups_named(*scene, "Effect_smkSigB").size() == 1);
	CHECK(static_cast<bool>(scene->slot_group(entity_effect_slot(h.packed))));
	const p::EffectSlotToken fire = destruction_effect_slot(h.packed, 5, 0);
	CHECK(static_cast<bool>(scene->slot_group(fire)));
	e.position = Vec3{ 6, 7, 8 };
	w->destruction_emitters.move_vehicle_fire(*w, e);
	p::EffectPose pose;
	CHECK(scene->group_pose(scene->slot_group(fire), pose));
	const CollisionMatrix matrix = entity_placement_matrix(e);
	const Vec3 row{ float(from_fixed(matrix.m[8])), float(from_fixed(matrix.m[9])),
		float(from_fixed(matrix.m[10])) };
	CHECK(near(pose.position, effect_point({ 6, 7, 8 })));
	CHECK(near(pose.forward, unit(effect_point(row))));
	DestructionEffectEvent smoke_stop = bank_row(e, 4, 0, "", {});
	smoke_stop.release = true;
	w->out.destruction.effects.push_back(smoke_stop);
	w->destruction_emitters.sync(*w);
	CHECK(!scene->slot_group(entity_effect_slot(h.packed)));
}

// A death piece's trail: held in the piece's handle, re-posed while it flies, released on its
// water crossing or when its bounce budget is spent, destroyed outright when the ring reuses
// its slot.
// [orig: Entity_SpawnDeathPieces @ 0x493813..0x49382A; Entity_ProcessDeathPiecePhysics
//  @ 0x493068, @ 0x492E2D, @ 0x492FC9; DeathPiece_AllocSlot @ 0x57B528]
void test_death_piece_trail_handle() {
	auto w = make_world();
	auto scene = make_scene({ "Trail" });
	open(*w, scene);
	DeathPiece piece;
	piece.pos = Vec3{ 1, 2, 3 };
	w->destruction_emitters.spawn_piece_trail(*w, 9, piece, "Trail");
	const p::EffectGroupId trail = scene->slot_group(death_piece_trail_slot(9));
	CHECK(static_cast<bool>(trail));
	piece.pos = Vec3{ 4, 5, 6 };
	w->destruction_emitters.move_piece_trail(*w, 9, piece);
	p::EffectPose pose;
	CHECK(scene->group_pose(trail, pose));
	CHECK(near(pose.position, effect_point({ 4, 5, 6 })));
	CHECK(near(pose.forward, { 0, 1, 0 }));
	w->destruction_emitters.release_piece_trail(*w, 9);
	const auto released = group_by_id(*scene, trail);
	CHECK(released.has_value() && released->detached);
	w->destruction_emitters.spawn_piece_trail(*w, 9, piece, "Trail");
	const p::EffectGroupId next = scene->slot_group(death_piece_trail_slot(9));
	w->destruction_emitters.destroy_piece_trail(*w, 9);
	CHECK(!scene->contains_group(next));
	// A silent death's piece names no trail.
	w->destruction_emitters.spawn_piece_trail(*w, 10, piece, "");
	CHECK(!scene->slot_group(death_piece_trail_slot(10)));
}

// --- the flying rounds' move emitters --------------------------------------------------------

// A live emitter spawns along the round's velocity, tagged; the held handle re-poses to the
// round's position with its aim kept; a released emitter (the water's, or the round's end)
// stops emission, and a surfacing round spawns a fresh one.
// [orig: Projectile_UpdatePhysics @ 0x4E9F94..0x4EA011, @ 0x4EA031, @ 0x4EA039;
//  Projectile_ReleaseEffects @ 0x4E8280]
void test_round_move_emitter() {
	auto w = make_world();
	auto scene = make_scene({ "Toss" });
	open(*w, scene);
	AmmoTableEntry ammo;
	ammo.valid = true;
	ammo.impact_effects[1].effect = "Toss";
	w->tables.ammo.entries.push_back(ammo);
	LiveRound &round = w->round_sim.rounds[3];
	round.active = true;
	round.ammo_index = 0;
	round.presentation_generation = 5;
	round.move_effect_live = true;
	round.pos = Vec3{ 1, 2, 3 };
	round.vel = Vec3{ 0, 1, 0 };
	w->round_emitters.sync(*w);
	const p::EffectSlotToken slot = round_move_slot(round_presentation_key(round, 3));
	const p::EffectGroupId first = scene->slot_group(slot);
	CHECK(static_cast<bool>(first));
	p::EffectPose pose;
	CHECK(scene->group_pose(first, pose));
	CHECK(near(pose.position, effect_point({ 1, 2, 3 })));
	CHECK(near(pose.forward, { 0, 0, -1 }));
	round.pos = Vec3{ 4, 5, 6 };
	w->round_emitters.sync(*w);
	CHECK(scene->slot_group(slot) == first);
	CHECK(scene->group_pose(first, pose));
	CHECK(near(pose.position, effect_point({ 4, 5, 6 })) && near(pose.forward, { 0, 0, -1 }));
	round.move_effect_live = false;
	w->round_emitters.sync(*w);
	const auto released = group_by_id(*scene, first);
	CHECK(released.has_value() && released->detached);
	round.move_effect_live = true;
	w->round_emitters.sync(*w);
	const p::EffectGroupId fresh = scene->slot_group(slot);
	CHECK(static_cast<bool>(fresh) && fresh != first);
	round.active = false;
	w->round_emitters.sync(*w);
	CHECK(!scene->slot_group(slot));
	CHECK(w->round_emitters.held() == 0);
}

} // namespace

int main() {
	test_move_group_re_poses_a_live_group();
	test_slot_a_spawns_every_masked_point_and_keeps_the_last();
	test_slot_a_origin_leg_is_unoriented_and_unstored();
	test_mission_walk_keeps_the_pool_gates_and_attaches_once();
	test_controller_edges_spawn_and_release_slot_a();
	test_hidden_attach_retries_once();
	test_slot_re_pose_follows_the_first_masked_point();
	test_emitter_update_runs_past_a_non_null_update_callback();
	test_destruction_rows_spawn_hold_re_pose_and_release();
	test_vehicle_smoke_and_fire_handles();
	test_death_piece_trail_handle();
	test_round_move_emitter();
	if (failures != 0) {
		std::printf("item_emitters_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("item_emitters_test passed\n");
	return 0;
}
