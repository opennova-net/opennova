// The fire face (LI-s18): after case 0x95's fire request returns nonzero, at the record's
// dispatch, the local player's facial slot takes expression 4 for 15 transitions and its gaze
// straight ahead for 15 steps; the press leg's request and the release leg's throw request
// alike, and only a request the slot takes (WeaponSlot_RequestFire's return). The face lands
// ahead of the update's facial step.
// [orig: Input_HandleActionBinding_0 @0x4E0420 - the press leg @0x4E0A06..0x4E0A78, the release
//  leg @0x4E086F..0x4E08D2 (FaceAnim row +0x2C = 4, +0x30 = 15, +0x50 = 15, +0x48 / +0x4C =
//  0.0); WeaponSlot_RequestFire @0x53EFA0; FaceAnim_UpdateSlot @0x57FA50 from
//  FaceAnim_UpdateAllSlots @0x580000]
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <base/resource_index/resource_index.h>
#include <formats/def/def.h>
#include <runtime/world/facial_animation.h>
#include <runtime/world/local_player.h>
#include <runtime/world/player_view.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/world.h>
#include "common/test_paths.h"

using namespace opennova;
using namespace opennova::world;
namespace wa = opennova::world::weapon_action;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

// The local player with a facial slot (the authored fixtures/grm person face) and an
// equipped weapon whose press leg passes its gates.
struct Fixture {
	ResourceIndex index;
	World world;
	def::DefItemDef definition{};
	def::DefItemsFile definitions{};
	LocalPlayerWeapon weapon;
	PlayerViewState view;
	EntityHandle player;

	Fixture() {
		CHECK(index.scan(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/grm"));
		world.registry.configure_pool(0, 8);
		definition.id = 100011;
		definition.type = def::DEF_ITEM_TYPE_PERSON;
		std::strcpy(definition.graphic, "folder\\PeRsOn.3di");
		definitions.entries = &definition;
		definitions.count = 1;
		world.facials.configure(world, &index, definitions);
		Entity entity;
		entity.kind = EntityKind::Organic;
		entity.alive = true;
		entity.health = 100;
		entity.item_id = 11;
		entity.item_type = 3;
		entity.has_item_def = true;
		entity.equipped_adm_index = 1;
		player = world.registry.spawn(0, entity);
		world.facials.initialize(world);
		world.cached.local_player = player;
		world.cached.humans = 1;
		world.tables.weapons.entries.resize(2);
		auto &row = world.tables.weapons.entries[1];
		row.valid = true;
		row.category = 3;
		row.name = "WPN_FACETEST";
		weapon.def_name = row.name;
		weapon.active = true;
		weapon_fsm_bake(nullptr, 0, nullptr, nullptr, nullptr, weapon.def);
		weapon.def.clip_capacity = 30;
		weapon.slot.clip = 30;
	}

	FacialSlot *face() { return world.facials.for_entity(*world.registry.get(player)); }
	bool press() { return local_weapon_press_fire(world, weapon, view); }
};

bool fire_face(const FacialSlot &face) {
	return face.expression_override == 4 && face.override_timer == 15 &&
			face.directed_timer == 15 && face.directed_eyes.x == 0.0f &&
			face.directed_eyes.y == 0.0f;
}

void test_a_taken_press_sets_the_fire_face_at_its_record() {
	auto f = std::make_unique<Fixture>();
	FacialSlot *face = f->face();
	CHECK(face != nullptr);
	if (face == nullptr) return;
	CHECK(face->expression_override == -1);
	face->directed_eyes = {0.5f, -0.25f};
	CHECK(f->press());
	CHECK(fire_face(*face)); // at the dispatch, before any pump
	// The update's facial step follows the input frame: it runs the face's first step, the
	// gaze directed. [orig: Game_ProcessMainFrame @0x5263F0 - Input_ProcessFrame @0x52661D
	//  ahead of Entity_UpdateAllEntities @0x52674B; FaceAnim_UpdateSlot @0x57FB25..0x57FB35]
	f->world.facials.tick(f->world);
	CHECK(face->directed_timer == 14);
	CHECK(face->expression_override == 4);
}

// The request's return gates the face: a semi def takes it in IDLE and EMPTYIDLE, an
// automatic def in IDLE and mid-FIRE (the deferral) but not in EMPTYIDLE.
void test_the_requests_return_gates_the_face() {
	struct Row {
		bool auto_fire;
		int32_t current;
		bool face;
	};
	const Row rows[] = {
		{false, wa::kIdle, true},
		{false, wa::kEmptyIdle, true},
		{false, wa::kRecoil, false},
		{true, wa::kFire, true},
		{true, wa::kEmptyIdle, false},
		{true, wa::kReload, false},
	};
	for (const Row &row : rows) {
		auto f = std::make_unique<Fixture>();
		f->weapon.def.auto_fire = row.auto_fire;
		f->weapon.slot.current = row.current;
		CHECK(f->press()); // the leg's gates pass: the press latches whatever the request does
		FacialSlot *face = f->face();
		CHECK(face != nullptr && fire_face(*face) == row.face);
		CHECK(face != nullptr && (face->expression_override == -1) == !row.face);
	}
}

void test_a_refused_press_sets_none() {
	auto f = std::make_unique<Fixture>();
	// OnlyFireScoped outside the optical view: the leg returns ahead of the request.
	f->world.tables.weapons.entries[1].flags = weapon_flag::kOnlyFireScoped;
	CHECK(!f->press());
	FacialSlot *face = f->face();
	CHECK(face != nullptr && face->expression_override == -1 && face->directed_timer == 0);
}

void test_the_release_sets_the_fire_face() {
	auto f = std::make_unique<Fixture>();
	f->weapon.def.flags = weapon_flag::kPowerThrow;
	CHECK(!local_weapon_release_fire(f->world, f->weapon)); // no windup: no request
	FacialSlot *face = f->face();
	CHECK(face != nullptr && face->expression_override == -1);
	f->world.logic_tick = 40;
	f->weapon.power_throw_start_tick = 10;
	CHECK(local_weapon_release_fire(f->world, f->weapon));
	CHECK(face != nullptr && fire_face(*face));
}

void test_no_facial_slot_takes_no_face() {
	auto f = std::make_unique<Fixture>();
	f->world.registry.get(f->player)->facial_slot = 0; // entity+0x1B8 == 0 @0x4E0A33
	CHECK(f->press());
	CHECK(f->face() == nullptr);
}

// The damage face (D-NET-NEW-LI-21): damage to the local player sets its facial row's
// expression override 6 (DISGUST) for 10 transitions, and no gaze. A local player with no
// facial slot writes no row: retail's unguarded write lands in row 0, another character's face,
// which ours leaves alone (PERMANENT, Q22-1 (a)).
// [orig: Player_OnDamageReceived @0x4DD880 - the row @0x4DD8F3..0x4DD907, +0x2C = 6 @0x4DD90F,
//  +0x30 = 10 @0x4DD916; FaceAnim_UpdateSlot @0x57FA50 (@0x57FACB / @0x57FAD2)]
void test_the_damage_face() {
	auto f = std::make_unique<Fixture>();
	auto local = std::make_unique<LocalPlayer>(f->world);
	f->world.local_player_state = local.get();
	const int32_t at[3] = {0, 0, 0};
	player_on_damage_received(f->world, RadarSource{}, at);
	FacialSlot *face = f->face();
	CHECK(face != nullptr && face->expression_override == 6 && face->override_timer == 10);
	CHECK(face != nullptr && face->directed_timer == 0); // no gaze write
	f->world.local_player_state = nullptr;
}

void test_no_facial_slot_writes_no_row() {
	auto f = std::make_unique<Fixture>();
	auto local = std::make_unique<LocalPlayer>(f->world);
	f->world.local_player_state = local.get();
	// The fixture's person holds row 0 (the first allocated face); the local player is a
	// second body with no facial slot.
	FacialSlot *row0 = f->face();
	CHECK(row0 != nullptr && (f->world.registry.get(f->player)->facial_slot & 0x7FFF) == 0);
	Entity body;
	body.kind = EntityKind::Organic;
	body.alive = true;
	body.health = 100;
	f->world.cached.local_player = f->world.registry.spawn(0, body);
	CHECK(f->world.registry.get(f->world.cached.local_player)->facial_slot == 0);
	const int32_t at[3] = {0, 0, 0};
	player_on_damage_received(f->world, RadarSource{}, at);
	CHECK(row0 != nullptr && row0->expression_override == -1 && row0->override_timer == 0);
	f->world.local_player_state = nullptr;
}

} // namespace

int main() {
	test_a_taken_press_sets_the_fire_face_at_its_record();
	test_the_requests_return_gates_the_face();
	test_a_refused_press_sets_none();
	test_the_release_sets_the_fire_face();
	test_no_facial_slot_takes_no_face();
	test_the_damage_face();
	test_no_facial_slot_writes_no_row();
	if (failures != 0) {
		std::printf("fire_face: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("fire_face: ok\n");
	return 0;
}
