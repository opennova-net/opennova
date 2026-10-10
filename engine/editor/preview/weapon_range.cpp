#include <editor/preview/weapon_range.h>

#include <algorithm>
#include <cmath>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <runtime/assets/asset_store.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/world/ammo_table.h>
#include <runtime/world/ammo_table_build.h>
#include <runtime/world/collision.h>
#include <runtime/world/fire_sound.h>
#include <runtime/world/impact_scar.h>
#include <runtime/world/local_player.h>
#include <runtime/world/organic_fire.h>
#include <runtime/world/player_present.h>
#include <runtime/world/present_drains.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/weapon_table.h>
#include <runtime/world/weapon_table_build.h>
#include <runtime/world/world.h>

namespace opennova::editor {

namespace wa = world::weapon_action;

namespace {

// The pools the range's world holds: the shooter (pool 0) and the target (pool 2).
constexpr size_t kPoolCapacity = 4;
// The shooter's eye stands this high over the world's zero, which the game reads as the water plane where no water
// is authored: a scar is written only above it [orig: Scar_AddEntry @0x5CC865, `hit_z > g_EnvWaterHeightFixed`], and
// an eye below it is an eye under water [orig: RoundData_SpawnRound @0x4ec2de..0x4ec2ea]. Every place the range
// hands out is the eye's (this height taken off).
constexpr float kEyeHeight = 10.0f;
const world::Vec3 kEye{0.0f, 0.0f, kEyeHeight};

world::Vec3 from_eye(const world::Vec3 &v) { return world::Vec3{v.x - kEye.x, v.y - kEye.y, v.z - kEye.z}; }

// The target stands: its hit points beyond any burst, and the entity flag the damage gates read so nothing it takes
// ever kills it [orig: Projectile_ProcessDamageOnTarget @ 0x4E7FF6, the indestructible test].
constexpr int32_t kTargetHealth = 32000;

int32_t q16(float v) { return int32_t(std::lround(double(v) * io::kFp16OneD)); }

// The target's face: a square in the range's (x = 0) plane of the entity, `half` metres to each side, facing the
// shooter (-x), its material the byte whose row an ammo plays there; the section's face run as the collision
// builder lays a CFAC run out (Q8 vertices, a Q14 normal, the YZ projection, the face's bounds) [orig: the
// runtime CVRT/CNRM/CFAC arrays the builder @ 0x5B3BF0 hangs off each COBJ, walked by
// Physics_RaycastAgainstBoneCollision @ 0x4E4CB0].
world::CollisionModel target_model(uint8_t material, float half) {
	world::CollisionModel model;
	const int16_t h = int16_t(std::lround(half * 256.0f));
	model.face_vertices = {{0, int16_t(-h), int16_t(-h)}, {0, h, int16_t(-h)}, {0, h, h}, {0, int16_t(-h), h}};
	const auto face = [&](int a, int b, int c) {
		world::CollisionFace f;
		f.v[0] = int16_t(a);
		f.v[1] = int16_t(b);
		f.v[2] = int16_t(c);
		f.normal[0] = -16384;
		f.axis = 4; // the YZ projection
		f.plane_dist = 0;
		f.min[1] = -q16(half);
		f.max[1] = q16(half);
		f.min[2] = -q16(half);
		f.max[2] = q16(half);
		f.material = material;
		model.faces.push_back(f);
	};
	face(0, 1, 2);
	face(0, 2, 3);
	model.sections.assign(1, {});
	model.sections[0].face_start = 0;
	model.sections[0].face_count = 2;
	model.sections[0].face_vertex_start = 0;
	model.sections[0].face_vertex_count = 4;
	return model;
}

std::string upper(const std::string &text) {
	std::string out = text;
	for (char &c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
	return out;
}

std::string metres(float v) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f m", double(v));
	return text;
}

} // namespace

const char *weapon_gesture_token(WeaponGesture gesture) {
	switch (gesture) {
	case WeaponGesture::Fire: return "fire";
	case WeaponGesture::Hold: return "hold";
	case WeaponGesture::Release: return "release";
	case WeaponGesture::Reload: return "reload";
	case WeaponGesture::Scope: return "scope";
	case WeaponGesture::Switch: return "switch";
	}
	return "fire";
}

bool weapon_gesture_of(const std::string &token, WeaponGesture &out) {
	for (const WeaponGesture gesture : {WeaponGesture::Fire, WeaponGesture::Hold, WeaponGesture::Release,
	                                    WeaponGesture::Reload, WeaponGesture::Scope, WeaponGesture::Switch})
		if (strutil::iequals(token, weapon_gesture_token(gesture))) {
			out = gesture;
			return true;
		}
	return false;
}

const char *weapon_shot_view_token(WeaponShotView view) {
	switch (view) {
	case WeaponShotView::Own: return "own";
	case WeaponShotView::Soldier: return "soldier";
	case WeaponShotView::Player: return "player";
	}
	return "own";
}

const char *weapon_range_event_token(WeaponRangeEvent::Kind kind) {
	using Kind = WeaponRangeEvent::Kind;
	switch (kind) {
	case Kind::Clip: return "clip";
	case Kind::Begin: return "begin";
	case Kind::Effect: return "effect";
	case Kind::End: return "end";
	case Kind::Fired: return "fired";
	case Kind::Dry: return "dry";
	case Kind::Reload: return "reload";
	case Kind::Launch: return "launch";
	case Kind::Impact: return "impact";
	case Kind::Sound: return "sound";
	case Kind::Refused: return "refused";
	}
	return "begin";
}

// The tables as the mission load builds them, kept so each run starts from them as loaded.
struct WeaponRange::Tables {
	world::WeaponTable weapons;
	world::AmmoTable ammo;
};

WeaponRange::WeaponRange() = default;
WeaponRange::~WeaponRange() = default;

bool WeaponRange::configure(const WeaponRangeSetup &setup) {
	const bool same_files = setup.files == setup_.files && setup.files && !reads_.moved(*setup.files);
	const bool same = same_files && strutil::iequals(setup.catalog, setup_.catalog) &&
	                  strutil::iequals(setup.weapon, setup_.weapon) && strutil::iequals(setup.ammo, setup_.ammo) && read_;
	if (same && setup.view == setup_.view && setup.enemy == setup_.enemy && setup.target == setup_.target) return false;
	setup_ = setup;
	if (!same) {
		ready_ = false;
		why_.clear();
		tables_.reset();
		weapon_index_ = -1;
		ammo_index_ = -1;
		ammo_.clear();
		reads_.clear();
		read_ = false;
		if (!setup.files) {
			why_ = "No project is open.";
		} else {
			// The project's files as the game's mission load reads them (the engine's own reads,
			// mission::read_weapon_defs and read_ammo_table): weapon.def through the game's parser, a SIGHTS row
			// whose texture the mount lacks no row, its table built with the clips its actions read from their
			// maps [orig: Anim_InitActions @0x541FA0], then ammo.def and the round types linked.
			auto stamped = std::make_shared<StampedFiles>(setup.files);
			ResourceIndex index;
			index.mount_source(stamped);
			assets::AssetStore store(&index);
			mission::BootFileSource boot;
			boot.has_file = [&index](const std::string &name) { return index.has_file(name); };
			boot.read_file = [&stamped](const std::string &name, std::vector<uint8_t> &out) {
				return stamped->read(name, out);
			};
			def::DefWeaponsFile weapons{};
			// A soldier's shots alone (no weapon in hand, DI-24): the tables read as the load reads them all the same,
			// whatever weapon.def is (an NPC's round spawn looks its byte up in the weapon table [orig:
			// RoundData_SpawnRound @ 0x4EC0D0]).
			const bool soldier = setup.weapon.empty();
			const mission::DefTableRead weapons_read = mission::read_weapon_defs(boot, setup.catalog, weapons);
			if (weapons_read == mission::DefTableRead::Missing) {
				if (!soldier) why_ = "The project has no " + setup.catalog + ".";
			} else if (weapons_read == mission::DefTableRead::Unreadable) {
				if (!soldier) why_ = setup.catalog + " does not read as the game's weapon table.";
			}
			if (why_.empty()) {
				tables_ = std::make_unique<Tables>();
				if (weapons_read == mission::DefTableRead::Read)
					tables_->weapons = world::build_weapon_table(weapons, &store);
				mission::read_ammo_table(boot, "ammo.def", tables_->ammo);
				world::resolve_weapon_round_types(tables_->weapons, tables_->ammo);
				if (soldier) {
					ready_ = !tables_->ammo.entries.empty();
					if (!ready_) why_ = "The project has no ammo.def the game reads: no ammo fires.";
					// An ammo fired alone (DI-23): the record by name as the game's lookup finds it [orig:
					// AmmoDef_LookupByName @ 0x409870].
					if (ready_ && !setup.ammo.empty()) {
						ammo_index_ = tables_->ammo.index_of(setup.ammo.c_str());
						ready_ = ammo_index_ >= 0 && ammo_index_ < 256;
						if (!ready_) why_ = setup.ammo + " is not in ammo.def.";
						else ammo_ = tables_->ammo.entries[size_t(ammo_index_)].name;
					}
				} else {
					weapon_index_ = tables_->weapons.index_of(setup.weapon.c_str());
					// The weapon's row as the game's mount reads it: the last block of the name
					// (def::def_weapon_index_by_name).
					const int row_index = def::def_weapon_index_by_name(weapons.entries, weapons.count, setup.weapon.c_str());
					const def::DefWeaponDef *row = row_index >= 0 ? &weapons.entries[row_index] : nullptr;
					if (weapon_index_ < 0 || !row) {
						why_ = setup.weapon + " is not in " + setup.catalog + ".";
					} else {
						install_ = world::weapon_install_data_from_def(*row);
						// A mount by name runs the descriptors the table's load baked [orig: Player_MountWeaponSlot
						// @ 0x4DFA40 binds the table Anim_InitActions baked].
						install_.table_baked = true;
						const world::WeaponTableEntry *entry = tables_->weapons.by_index(uint8_t(weapon_index_));
						const world::AmmoTableEntry *round =
								entry ? tables_->ammo.by_index(entry->ammo_index) : nullptr;
						ammo_ = round ? round->name : std::string();
						ready_ = true;
					}
				}
			}
			def::def_free_weapons(&weapons);
			reads_ = stamped->stamps();
			read_ = true;
		}
	}
	reset_();
	return true;
}

void WeaponRange::set_gestures(std::vector<WeaponGestureAt> gestures) {
	std::stable_sort(gestures.begin(), gestures.end(),
	                 [](const WeaponGestureAt &a, const WeaponGestureAt &b) { return a.tick < b.tick; });
	if (gestures == gestures_) return;
	// The first tick the two lists differ on: a run that reached it runs again.
	int32_t first = INT32_MAX;
	const size_t common = std::min(gestures.size(), gestures_.size());
	size_t i = 0;
	while (i < common && gestures[i] == gestures_[i]) ++i;
	if (i < gestures.size()) first = std::min(first, gestures[i].tick);
	if (i < gestures_.size()) first = std::min(first, gestures_[i].tick);
	gestures_ = std::move(gestures);
	if (tick_ > first) reset_();
}

void WeaponRange::set_shots(std::vector<WeaponRangeShot> shots) {
	std::stable_sort(shots.begin(), shots.end(),
	                 [](const WeaponRangeShot &a, const WeaponRangeShot &b) { return a.tick < b.tick; });
	if (shots == shots_list_) return;
	int32_t first = INT32_MAX;
	const size_t common = std::min(shots.size(), shots_list_.size());
	size_t i = 0;
	while (i < common && shots[i] == shots_list_[i]) ++i;
	if (i < shots.size()) first = std::min(first, shots[i].tick);
	if (i < shots_list_.size()) first = std::min(first, shots_list_[i].tick);
	shots_list_ = std::move(shots);
	// A step fires the shots of its own tick: a run past the first that changed runs again.
	if (tick_ > first) reset_();
}

const world::AmmoTable *WeaponRange::ammo_table() const {
	return tables_ ? &tables_->ammo : nullptr;
}

bool WeaponRange::scoped() const {
	return local_ && world::player_view_scope_settled(local_->view);
}

void WeaponRange::reset_() {
	++serial_;
	++runs_;
	tick_ = 0;
	holding_ = false;
	switch_timer_ = 0;
	draw_again_ = false;
	card_ = false;
	frame_ = world::LocalPlayerViewFrame();
	shots_ = 0;
	events_.clear();
	view_ = world::LocalPlayerWeaponView();
	soldier_ = world::EntityHandle();
	local_.reset();
	world_.reset();
	collision_.reset();
	if (!ready_ || !tables_) return;
	world_ = std::make_unique<world::World>();
	world::World &world = *world_;
	world.tables.weapons = tables_->weapons;
	world.tables.ammo = tables_->ammo;
	world.registry.configure_pool(0, kPoolCapacity);
	world.registry.configure_pool(1, kPoolCapacity);
	world.registry.configure_pool(2, kPoolCapacity);
	if (setup_.weapon.empty()) {
		// A soldier's shots (DI-24): the soldier, a person of no player standing at the range's origin (the eye's place)
		// facing +x, its body the game's NPC body; no local player. Its shots leave it from where it says.
		world::Entity soldier;
		soldier.kind = world::EntityKind::Organic;
		soldier.item_type = 3;
		soldier.has_item_def = true;
		soldier.alive = true;
		soldier.health = 100;
		soldier.health_max = 100;
		soldier.team = 1;
		soldier.position = kEye;
		soldier_ = world.registry.spawn(0, soldier);
		world.ai.attach(soldier_);
		if (world::AiEntity *body = world.ai.for_handle(soldier_)) {
			body->inf.active = true;
			body->pos[2] = q16(kEyeHeight);
		}
	}
	// The shooter: the local player, a person of the player class standing at the range's eye, its eye there (no
	// CameraOffset), facing +x, the weapon in hand.
	world::Entity seed;
	seed.kind = world::EntityKind::Organic;
	seed.item_type = 3;
	seed.has_item_def = true;
	seed.alive = true;
	seed.health = 100;
	seed.health_max = 100;
	seed.flags = world::kEntityFlagPlayer;
	seed.team = 1;
	seed.equipped_adm_index = uint8_t(weapon_index_);
	seed.position = kEye;
	if (!setup_.weapon.empty()) {
		const world::EntityHandle shooter = world.registry.spawn(0, seed);
		local_ = std::make_unique<world::LocalPlayer>(world);
		world.cached.local_player = shooter;
		world.local_player_state = local_.get();
		world.ai.attach(shooter);
		if (world::AiEntity *body = world.ai.for_handle(shooter)) {
			body->inf.active = true;
			body->inf.is_local_player = true;
			body->pos[2] = q16(kEyeHeight);
		}
	}
	// The target: a building standing `range` metres down the line of fire, its face the surface picked.
	collision_ = std::make_unique<world::CollisionWorld>();
	if (setup_.target.shown) {
		world::Entity wall;
		wall.kind = world::EntityKind::Building;
		wall.item_type = 5;
		wall.has_item_def = true;
		wall.alive = true;
		wall.health = kTargetHealth;
		wall.health_max = kTargetHealth;
		wall.engine_flags = world::kEntityFlagIndestructible;
		wall.position = world::Vec3{setup_.target.range, 0.0f, kEyeHeight};
		const world::EntityHandle target = world.registry.spawn(2, wall);
		const int tag = std::clamp(setup_.target.tag, kWeaponRangeFirstTag, world::kImpactEffectTagCount - 1);
		const int32_t model = collision_->add_model(target_model(uint8_t(tag - 4), kWeaponRangeTargetHalf));
		collision_->assign_entity(target, model);
		const int32_t at[3] = {q16(setup_.target.range), 0, q16(kEyeHeight)};
		collision_->publish_entity_section_matrices(target, {world::collision_matrix_from_heading(0, at)});
	}
	collision_->build_tick_tables(world);
	world.collision = collision_.get();
	// The listener at the shooter: the distance gate counts from it (the editor hears at its camera besides).
	world.out.fire_sounds.set_listener(kEye);
	// The weapon mounted as the game mounts one by name: a fresh slot, a full clip and the def's reserve.
	if (!local_) return;
	world::local_weapon_install(world, local_->weapon, install_, false, false, nullptr, local_->view);
	view_ = world::local_player_weapon_view(world, local_->weapon, local_->inventory);
}

void WeaponRange::run_to(int32_t tick) {
	tick = std::clamp(tick, 0, kWeaponRangeMostTicks);
	if (tick < tick_) reset_();
	if (!world_) {
		tick_ = tick;
		return;
	}
	const int32_t from = tick_;
	while (tick_ < tick) step_();
	if (tick_ != from) ++serial_;
	// The frame the run stands at, observed (nothing advances): whether the card replaces the view model.
	frame_ = local_ ? local_->view_frame() : world::LocalPlayerViewFrame();
	card_ = frame_.scope_card_active;
}

void WeaponRange::fire_shots_(int32_t tick) {
	for (const WeaponRangeShot &shot : shots_list_)
		if (shot.tick == tick) fire_shot_(shot);
	if (ammo_alone()) fire_alone_(tick);
}

void WeaponRange::fire_shot_(const WeaponRangeShot &shot) {
	world::World &world = *world_;
	const int32_t tick = shot.tick;
	world::Entity *soldier = world.registry.get(soldier_);
	if (!soldier) return;
	// The fire block's shoot (world::organic_fire_shot): the firing byte rides the soldier while its ammo fires,
	// then returns to zero, and a shot fired marks the soldier a priority target; a zero byte fires nothing
	// [orig: Entity_UpdateInfantryAI @0x4BF345..0x4BF4AD]. The game's NPC entry spawns the round and presents its
	// launch (the ammo's ai_launch through the distance gate, its fire record) [orig:
	// WeaponSlot_FireAndSpawnEffects @0x53F440].
	WeaponRangeEvent fired;
	fired.tick = tick;
	fired.kind = WeaponRangeEvent::Kind::Fired;
	fired.at = shot.at;
	fired.round = shot.shot;
	fired.words = shot.words;
	if (shot.ammo != 0 && world.tables.ammo.by_index(shot.ammo)) {
		const world::Vec3 at{shot.at.x + kEye.x, shot.at.y + kEye.y, shot.at.z + kEye.z};
		const size_t before = world.round_sim.fired.size();
		world::organic_fire_shot(world, soldier_, world::FixedVec3{q16(at.x), q16(at.y), q16(at.z)}, shot.yaw_bam,
		                         shot.pitch_bam, shot.ammo);
		++shots_;
		const double bearing = double(shot.yaw_bam) * io::kRadiansPerBam;
		const double pitch = double(shot.pitch_bam) * io::kRadiansPerBam;
		fired.direction = world::Vec3{float(std::cos(bearing) * std::cos(pitch)),
		                              float(std::sin(bearing) * std::cos(pitch)), float(std::sin(pitch))};
		for (const world::LiveRound &round : world.round_sim.rounds)
			if (round.active && round.age_ticks == 0 && round.owner == soldier_) fired.tracer = fired.tracer || round.tracer;
		if (world.round_sim.fired.size() > before) soldier_fires_.push_back(shot.shot);
	} else {
		fired.kind = WeaponRangeEvent::Kind::Refused;
		fired.words = shot.words + (shot.ammo == 0 ? " Its ammo byte is zero: nothing fires."
		                                           : " Its ammo byte names no row of ammo.def: nothing fires.");
	}
	events_.push_back(fired);
}

void WeaponRange::apply_gestures_(int32_t tick) {
	world::World &world = *world_;
	world::LocalPlayerWeapon &weapon = local_->weapon;
	bool pressed = false;
	bool reload = false;
	for (const WeaponGestureAt &at : gestures_) {
		if (at.tick != tick) continue;
		WeaponRangeEvent refused;
		refused.tick = tick;
		refused.kind = WeaponRangeEvent::Kind::Refused;
		switch (at.gesture) {
		case WeaponGesture::Fire: pressed = true; break;
		case WeaponGesture::Hold:
			pressed = !holding_;
			holding_ = true;
			break;
		case WeaponGesture::Release: holding_ = false; break;
		case WeaponGesture::Reload: {
			// The dispatcher's gate ahead of the request: a full magazine or an empty reserve refuses it [orig:
			// input case 0xD3 @ 0x4E0420].
			world::WeaponSlotState *slot = world::active_local_weapon_slot(world, weapon);
			if (slot && world::weapon_fsm_reload_allowed(weapon.def, *slot)) {
				reload = true;
			} else {
				refused.action = "reload";
				refused.words = slot && slot->clip >= weapon.def.clip_capacity
				                        ? "The reload is refused: the clip is full."
				                        : "The reload is refused: the reserve is empty (or the weapon has no clip).";
				events_.push_back(refused);
			}
			break;
		}
		case WeaponGesture::Scope: {
			world::WeaponSlotState *slot = world::active_local_weapon_slot(world, weapon);
			if (!slot || !world::local_player_scope_toggle(world, weapon, local_->view, *slot)) {
				refused.action = "scope";
				refused.words = (weapon.def.flags & (world::weapon_flag::kScoped | world::weapon_flag::kSighted)) == 0
				                        ? "The scope is refused: the weapon is neither scoped nor sighted."
				                        : "The scope is refused now (a reload or a holster runs, or the last toggle's "
				                          "ease has not landed).";
				events_.push_back(refused);
			}
			break;
		}
		case WeaponGesture::Switch: {
			// The holster [orig: WeaponSlot_ForceQueueSwitchFrom @ 0x53F170]; its completion draws the weapon again
			// (step_, the swap's queue of SWITCHTO on the slot it swaps in [orig: WeaponAction_SwitchFrom
			// @ 0x543475..0x5434A3]).
			world::WeaponSlotState *slot = world::active_local_weapon_slot(world, weapon);
			if (slot) world::weapon_fsm_queue_switch_from(*slot);
			break;
		}
		}
	}
	local_->set_weapon_input(holding_, pressed, reload);
}

void WeaponRange::step_() {
	world::World &world = *world_;
	const int32_t tick = tick_;
	// The frame's head: the pending fire sounds count down [orig: Game_ProcessMainFrame @ 0x5263F0 ->
	// Sound_TickPendingSlots @ 0x529310, the call @ 0x526697].
	world.logic_tick = uint32_t(tick);
	world.out.fire_sounds.tick();
	// The presenting client's identity the spawn's tracer style selects against, stamped at the tick's head as the
	// world's tick stamps it [orig: g_LocalPlayerEntity->Team read @ 0x4EC740]: the shooter itself, or, seen by the
	// other side, a client of the other team.
	const bool other_side = setup_.enemy && setup_.view != WeaponShotView::Own;
	world.round_sim.local_player = other_side ? world::EntityHandle() : world.cached.local_player;
	world.round_sim.local_team = other_side ? uint8_t(2) : uint8_t(1);
	// The entity update: a soldier's shots in its walk (DI-24), then the rounds in flight [orig:
	// Entity_UpdateAllEntities -> Weapon_UpdateAllProjectiles @ 0x4EC020, the call @ 0x4C223A]. The kill-zone queue
	// the impacts push is not processed: nothing in the range takes the blast.
	soldier_fires_.clear();
	fire_shots_(tick);
	world.round_sim.tick(world, nullptr, collision_.get());
	// Its tail steps the tick the weapon walk reads [orig: current_tick @ 0x24C1968].
	world.logic_tick = uint32_t(tick + 1);
	if (!local_) {
		// No weapon in hand: what the soldier's shots handed the presenter.
		drain_();
		++tick_;
		return;
	}
	// The holster that finished on the last tick draws the weapon again.
	world::LocalPlayerWeapon &weapon = local_->weapon;
	if (draw_again_) {
		draw_again_ = false;
		if (world::WeaponSlotState *slot = world::active_local_weapon_slot(world, weapon))
			world::weapon_fsm_try_queue_switch_to(*slot);
	}
	apply_gestures_(tick);
	// The view tick, then the weapon walk [orig: Game_ProcessMainFrame: Camera_ComputeThirdPersonView @ 0x526781,
	// WeaponAction_ProcessAllEntities @ 0x526786].
	local_->run_local_view_tick();
	const world::WeaponSlotState *slot = world::active_local_weapon_slot(world, weapon);
	switch_timer_ = slot ? slot->switch_timer : 0;
	const uint32_t fired_before = weapon.fired_serial;
	const uint32_t dry_before = weapon.dry_serial;
	const uint32_t reload_before = weapon.reload_serial;
	local_->pump_local_weapon();
	// SWITCHFROM's timer runs to past -900 and the handler's last call resets it: the holster finished [orig:
	// WeaponAction_SwitchFrom @ 0x54345A].
	if (slot && slot->current == wa::kSwitchFrom && switch_timer_ < -900 && slot->switch_timer == 0) draw_again_ = true;
	view_ = world::local_player_weapon_view(world, weapon, local_->inventory);
	if (weapon.fired_serial != fired_before) ++shots_;
	if (weapon.dry_serial != dry_before) {
		WeaponRangeEvent dry;
		dry.tick = tick;
		dry.kind = WeaponRangeEvent::Kind::Dry;
		dry.action = "empty";
		dry.words = "The fire key on a spent clip: the empty click.";
		events_.push_back(dry);
	}
	if (weapon.reload_serial != reload_before) {
		WeaponRangeEvent reload;
		reload.tick = tick;
		reload.kind = WeaponRangeEvent::Kind::Reload;
		reload.action = "reload";
		reload.words = "The reload asked for: the clip refills as its action runs.";
		events_.push_back(reload);
	}
	drain_();
	++tick_;
}

void WeaponRange::fire_alone_(int32_t tick) {
	for (const WeaponGestureAt &at : gestures_) {
		if (at.tick != tick) continue;
		if (at.gesture == WeaponGesture::Fire) {
			// A soldier's shot of the ammo from the eye along the line of fire (heading 0, level), as the game fires an
			// ammo for a soldier (DI-23).
			WeaponRangeShot shot;
			shot.tick = tick;
			shot.ammo = uint8_t(ammo_index_);
			shot.shot = shots_ + 1;
			shot.words = "Fire: a soldier's shot of " + ammo_ + ".";
			fire_shot_(shot);
			continue;
		}
		WeaponRangeEvent refused;
		refused.tick = tick;
		refused.kind = WeaponRangeEvent::Kind::Refused;
		refused.action = weapon_gesture_token(at.gesture);
		refused.words = std::string("An ammo fired alone has no weapon to ") +
		                (at.gesture == WeaponGesture::Reload ? "reload"
		                 : at.gesture == WeaponGesture::Scope ? "scope"
		                 : at.gesture == WeaponGesture::Switch ? "switch"
		                                                       : "hold") +
		                ": only Fire fires it.";
		events_.push_back(refused);
	}
}

void WeaponRange::drain_() {
	world::World &world = *world_;
	const int32_t tick = tick_;
	static const std::vector<world::WeaponPresentationEvent> kNoRecords;
	const std::vector<world::WeaponPresentationEvent> &records = local_ ? local_->weapon.events : kNoRecords;
	const auto channel = [&](WeaponRangeEvent &event) {
		event.clip = view_.anim_key;
		event.clip_variant = view_.anim_variant;
		event.clip_ticks = view_.anim_advance_ticks;
	};
	using Kind = WeaponRangeEvent::Kind;
	// The impacts the flight resolved, each the ammo's row for its tag (the sound leg rides the fire-sound queue,
	// played where the impact is produced).
	const std::vector<world::RoundImpact> raw = world.round_sim.impacts;
	std::vector<world::RoundImpactPresentation> impacts;
	world::drain_round_impact_rows(world, impacts);
	for (const world::RoundImpact &impact : raw) {
		WeaponRangeEvent event;
		event.tick = tick;
		event.kind = Kind::Impact;
		event.at = from_eye(impact.position);
		event.direction = impact.direction;
		event.tag = impact.effect_tag;
		for (const world::RoundImpactPresentation &row : impacts)
			if (row.source_order == impact.source_order) {
				event.effect = row.effect;
				event.set = row.sound;
			}
		const char *row = impact.effect_tag >= 0 && impact.effect_tag < world::kImpactEffectTagCount
		                          ? world::kImpactEffectTagNames[impact.effect_tag]
		                          : "?";
		// Where the row came from (DI-23): the ammo's own, or ammo def 0's bank at the tag's place.
		const world::ImpactRowPick pick = world::round_impact_row(world.tables.ammo, impact);
		const std::string from =
				pick.from == world::ImpactRowFrom::Own
						? std::string("the ammo's ") + row + " row"
						: std::string("ammo def 0's bank at place ") + std::to_string(pick.tag) +
								  (pick.bank_tag > 0 && pick.bank_tag < world::kImpactEffectTagCount
								           ? std::string(" (its ") + world::kImpactEffectTagNames[pick.bank_tag] + " row)"
								           : std::string(" (no row)")) +
								  ", the ammo authoring no " + row + " row";
		event.words = "The round stops: " + from + " (" + (event.effect.empty() ? "no effect" : event.effect) + ", " +
		              (event.set.empty() ? "no sound" : event.set) + ").";
		events_.push_back(event);
	}
	// The weapon's presentation records, in the order the game's presenter takes them: a clip, the begin leg, the
	// direct effect, the end leg [orig: the batch order, world::weapon_batch_plan].
	for (const world::WeaponPresentationEvent &record : records) {
		if (!record.anim_key.empty()) {
			WeaponRangeEvent event;
			event.tick = tick;
			event.kind = Kind::Clip;
			channel(event);
			event.clip = record.anim_key;
			event.clip_variant = record.anim_variant;
			event.words = record.anim_key + " starts on the first-person channel.";
			events_.push_back(event);
		}
		if (record.action_started >= 0 && record.action_started < wa::kCount) {
			WeaponRangeEvent event;
			event.tick = tick;
			event.kind = Kind::Begin;
			channel(event);
			event.action = world::kWeaponActionSuffixes[record.action_started];
			event.set = record.action_soundset;
			event.effect = record.action_particle;
			event.point = record.action_particle_userpoint;
			// For the local player only the FIRE action takes the with-effect shim, and not at a settled scope in
			// first person [orig: ActionSlot_ExecuteActionTick @0x541a70].
			event.admitted = world::local_fire_effect_admitted(record.action_started, wa::kFire, !event.effect.empty(),
			                                                   record.scope_settled, record.third_person,
			                                                   record.vehicle_attack_context);
			event.words = upper(event.action) + " begins" +
			              (event.set.empty() ? std::string() : ": its soundset " + event.set) +
			              (event.effect.empty() ? std::string()
			               : event.admitted ? ", its particle " + event.effect + " at " + event.point
			                                : ", its particle " + event.effect + " not shown (the local player's begin "
			                                                                    "leg shows only FIRE's, and not scoped)") +
			              ".";
			events_.push_back(event);
		}
		if (record.action_effect >= 0 && record.action_effect < wa::kCount) {
			WeaponRangeEvent event;
			event.tick = tick;
			event.kind = Kind::Effect;
			channel(event);
			event.action = world::kWeaponActionSuffixes[record.action_effect];
			event.effect = record.effect_particle;
			event.point = record.effect_particle_userpoint;
			event.words = upper(event.action) + "'s direct effect " + event.effect + " at " + event.point + ".";
			events_.push_back(event);
		}
		if (record.action_finished >= 0 && record.action_finished < wa::kCount) {
			WeaponRangeEvent event;
			event.tick = tick;
			event.kind = Kind::End;
			channel(event);
			event.action = world::kWeaponActionSuffixes[record.action_finished];
			event.set = record.action_end_soundset;
			event.words = upper(event.action) + " finishes" +
			              (event.set.empty() ? std::string(".") : ": its soundsetend " + event.set + ".");
			events_.push_back(event);
		}
	}
	if (local_) local_->weapon.events.clear();
	// The rounds the pump spawned: the fire record, as the presenting host drains it.
	std::vector<world::FirePresentationRow> fires;
	world::drain_fire_presentation_rows(world, fires);
	size_t soldier_row = 0;
	for (world::FirePresentationRow &row : fires) {
		if (!row.is_local_player) {
			// A soldier's shot (DI-24): its sound played as it spawned (the ammo's ai_launch through the distance gate,
			// the NPC entry's own leg), its effect leg by the presenter's plan, the ammo arm's (zero wire flags):
			// the ai_launcheffect at the fire origin along the aim [orig: WeaponSlot_FireAndSpawnEffects @ 0x53F440].
			const int shot = soldier_row < soldier_fires_.size() ? soldier_fires_[soldier_row] : 0;
			++soldier_row;
			const world::FireEffectPlan plan = world::fire_effect_plan(row);
			if (!plan.spawn) continue;
			WeaponRangeEvent launch;
			launch.tick = tick;
			launch.kind = Kind::Launch;
			launch.effect = plan.effect;
			launch.at = from_eye(row.origin);
			launch.direction = row.forward;
			launch.round = shot;
			launch.words = "A soldier's shot: the ammo's ai_launcheffect " + plan.effect + " at its launch point.";
			events_.push_back(launch);
			continue;
		}
		WeaponRangeEvent fired;
		fired.tick = tick;
		fired.kind = Kind::Fired;
		fired.at = from_eye(row.origin);
		fired.direction = row.forward;
		fired.round = shots_;
		for (const world::LiveRound &round : world.round_sim.rounds)
			if (round.active && round.age_ticks == 0) fired.tracer = fired.tracer || round.tracer;
		fired.words = "Round " + std::to_string(shots_) + " leaves the shooter's eye along its aim" +
		              (fired.tracer ? ", a tracer." : ".");
		events_.push_back(fired);
		if (setup_.view == WeaponShotView::Own) continue;
		// The shot as another sees it: the round event's arm the editor picked, its sound legs through the game's
		// own dispatch (another shooter's, so they play) [orig: NetPacket_DeserializeRoundEvent @ 0x42F270] and
		// its effect leg by the presenter's plan.
		world::RoundSpawnParams params;
		params.origin = row.origin;
		params.shooter_pos = row.origin;
		params.shooter_pos_valid = true;
		params.ammo_index = row.ammo_index;
		params.adm_index = uint8_t(row.adm_index);
		params.wire_round_flags = setup_.view == WeaponShotView::Player ? world::round_event_flag::kAdmIndexed : 0;
		world::fire_sound_on_spawn(world, params);
		row.is_local_player = false;
		row.adm_arm = setup_.view == WeaponShotView::Player;
		const world::FireEffectPlan plan = world::fire_effect_plan(row);
		if (plan.spawn) {
			WeaponRangeEvent launch;
			launch.tick = tick;
			launch.kind = Kind::Launch;
			launch.effect = plan.effect;
			launch.point = plan.spawn_at_muzzle ? plan.userpoint : std::string();
			launch.at = from_eye(row.origin);
			launch.direction = row.forward;
			launch.round = shots_;
			launch.words = row.adm_arm ? "Another player's shot: FIRE's " + plan.effect + " at " + plan.userpoint + "."
			                           : "A soldier's shot: the ammo's ai_launcheffect " + plan.effect + ".";
			events_.push_back(launch);
		}
	}
	// The sounds the fire-sound queue readied this tick, each at its place.
	for (const world::ReadyFireSound &ready : world.out.fire_sounds.drain()) {
		WeaponRangeEvent sound;
		sound.tick = tick;
		sound.kind = Kind::Sound;
		sound.set = ready.set_name;
		sound.at = from_eye(ready.pos);
		const float distance = std::sqrt(sound.at.x * sound.at.x + sound.at.y * sound.at.y + sound.at.z * sound.at.z);
		sound.words = ready.set_name + " heard " + metres(distance) + " from the shooter.";
		events_.push_back(sound);
	}
}

std::vector<WeaponRangeTrail> WeaponRange::trails() const {
	std::vector<WeaponRangeTrail> out;
	if (!world_) return out;
	for (const world::TracerTrailChannel &channel : world_->round_sim.trails.channels) {
		if (!channel.active || channel.count <= 0) continue;
		WeaponRangeTrail trail;
		trail.style = channel.style_id;
		trail.age = channel.age;
		for (int i = 0; i < channel.count && i < world::TracerTrailChannel::kMaxPoints; ++i) {
			trail.points.push_back(from_eye(channel.pts[size_t(i)].pos));
			trail.widths.push_back(channel.pts[size_t(i)].w);
		}
		out.push_back(std::move(trail));
	}
	return out;
}

std::vector<WeaponRangeRound> WeaponRange::rounds() const {
	std::vector<WeaponRangeRound> out;
	if (!world_) return out;
	for (const world::LiveRound &round : world_->round_sim.rounds)
		if (round.active) out.push_back({from_eye(round.pos), round.tracer, world::round_visible_item_id(round)});
	return out;
}

renderer::ScarDrawList WeaponRange::scars() const {
	renderer::ScarDrawList out;
	if (!world_) return out;
	// Every slot drawn: the fog box as wide as the range, every owner visible (the range has no occlusion).
	renderer::ScarViewContext context;
	context.fog_distance = kWeaponRangeFarthest * 4.0f;
	context.cam_x = kEye.x;
	context.cam_y = kEye.y;
	renderer::compile_scar_draws(world_->out.scars, context, out);
	for (renderer::ScarDrawBatch &batch : out.batches) {
		if (batch.entity_local) continue;
		for (uint32_t i = batch.first_vertex; i < batch.first_vertex + batch.vertex_count && i < out.vertices.size(); ++i) {
			const world::Vec3 at = from_eye(world::Vec3{out.vertices[i].x, out.vertices[i].y, out.vertices[i].z});
			out.vertices[i].x = at.x;
			out.vertices[i].y = at.y;
			out.vertices[i].z = at.z;
		}
	}
	return out;
}

int WeaponRange::scar_count() const {
	if (!world_) return 0;
	int count = 0;
	for (const world::ScarSlot &slot : world_->out.scars.world_ring().slots) count += slot.live ? 1 : 0;
	for (const world::ScarRing &ring : world_->out.scars.entity_rings())
		if (ring.in_use)
			for (const world::ScarSlot &slot : ring.slots) count += slot.live ? 1 : 0;
	return count;
}

bool WeaponRange::target_corners(world::Vec3 out[4]) const {
	if (!setup_.target.shown) return false;
	const float x = setup_.target.range;
	const float h = kWeaponRangeTargetHalf;
	out[0] = world::Vec3{x, -h, -h};
	out[1] = world::Vec3{x, h, -h};
	out[2] = world::Vec3{x, h, h};
	out[3] = world::Vec3{x, -h, h};
	return true;
}

} // namespace opennova::editor
