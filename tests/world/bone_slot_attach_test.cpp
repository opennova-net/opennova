// The attach process's bone typing (D-NET-445): the requested bone is typed by
// its userpoint name, the ten-slot table serves the occupied test alone, the
// requester leaves its seat before an untyped bone refuses, and a typed bone off
// the table attaches (a sitex one with no seat handle, a ctrlx one in slot 8).
// [orig: Entity_ProcessVehicleAttach @0x435AA0; Entity_GetBoneSlotType @0x434ED0;
//  Entity_AttachToVehicleSlot @0x4946D0]
#include <runtime/world/entity.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include "../common/title_fixture.h"

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                         \
    do {                                                                                 \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

Seat seat_row(SeatType type, uint8_t bone, uint8_t retail_slot, const char *name) {
	Seat s;
	s.type = type;
	s.bone_index = bone;
	s.retail_slot = retail_slot;
	s.source_name = name;
	return s;
}

// A carrier whose model holds sitex00 (row 1, slot 0), ctrlx00 (row 2, replaced in
// slot 8 by row 4), sitex01 (row 3, slot 1), drvrx00 (row 4, slot 8) and a lamp
// (row 5, untyped); rows 2 is off the table, typed ctrlx.
struct Rig {
	std::unique_ptr<World> wp = std::make_unique<World>();
	bool wp_jo = test_titles::jo_world(*wp);
	World &w = *wp;
	EntityHandle veh_h, player_h;
	Rig() {
		w.registry.configure_pool(0, 8);
		w.registry.configure_pool(1, 8);
		w.ai.is_authority = true;
		Entity veh;
		veh.kind = EntityKind::Item;
		veh.has_item_def = true;
		veh.has_graphic_model = true;
		veh.graphic_model_userpoints = 5;
		veh.item_attrib = kItemAttribPlayerControl;
		veh.item_id = 1294;
		veh.item_type_index = 7;
		veh.position = {100.0f, 200.0f, 10.0f};
		veh.health = 2000;
		veh.health_max = 2000;
		veh.alive = true;
		veh.seats.push_back(seat_row(SeatType::Passenger, 1, 0, "sitex00"));
		veh.seats.push_back(seat_row(SeatType::Passenger, 3, 1, "sitex01"));
		veh.seats.push_back(seat_row(SeatType::Driver, 4, 8, "drvrx00"));
		veh.off_table_seats.push_back(seat_row(SeatType::Controller, 2, 0xFF, "ctrlx00"));
		veh_h = w.registry.spawn(1, veh);

		Entity pl;
		pl.kind = EntityKind::Organic;
		pl.item_type_index = 3;
		pl.item_id = 5305;
		pl.player_class = 8;
		pl.position = {102.0f, 200.0f, 10.0f};
		pl.health = 150;
		pl.health_max = 150;
		pl.alive = true;
		player_h = w.registry.spawn(0, pl);
	}
	Entity &veh() { return *w.registry.get(veh_h); }
	Entity &player() { return *w.registry.get(player_h); }
};

// A typed table bone attaches into its own slot row.
void test_a_table_bone_takes_its_row() {
	Rig r;
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 3));
	CHECK(r.player().mounted && r.player().mount_bone == 3);
	CHECK(r.player().mount_type == SeatType::Passenger);
	CHECK(r.player().mount_seat == 1);
	CHECK(r.veh().seats[1].occupant == r.player_h);
	Seat seat;
	CHECK(r.w.vehicles.rider_seat(r.player(), r.veh(), seat) && seat.bone_index == 3);
}

// An off-table ctrlx bone is typed 2 by its name and writes slot 8 (the drvrx row's
// slot) whatever the bone; the rider sits on its own bone. [orig: @0x4947D2 (+0x170),
//  @0x494804 (+0x1A0), @0x494763..0x49476F (+0x16C / +0x157 / +0x168)]
void test_an_off_table_ctrlx_bone_takes_slot_8() {
	Rig r;
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 2));
	const Entity &p = r.player();
	CHECK(p.mounted && p.mount_target == r.veh_h && p.mount_bone == 2);
	CHECK(p.mount_type == SeatType::Controller);
	CHECK(p.mount_seat == 2);
	CHECK(r.veh().seats[2].occupant == r.player_h);
	CHECK(r.veh().primary_occupant == r.player_h);
	Seat seat;
	CHECK(r.w.vehicles.rider_seat(p, r.veh(), seat));
	CHECK(seat.bone_index == 2 && seat.type == SeatType::Controller);
}

// An off-table sitex bone parents the rider with no seat handle.
// [orig: @0x4946F3..0x494706 -> @0x494752]
void test_an_off_table_sitex_bone_holds_no_slot() {
	Rig r;
	r.veh().off_table_seats.push_back(seat_row(SeatType::Passenger, 6, 0xFF, "sitex08"));
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 6));
	const Entity &p = r.player();
	CHECK(p.mounted && p.mount_bone == 6 && p.mount_type == SeatType::Passenger);
	CHECK(p.mount_seat == -1);
	for (const Seat &s : r.veh().seats) CHECK(!s.occupant.valid());
	CHECK(!r.veh().primary_occupant.valid());
	Seat seat;
	CHECK(r.w.vehicles.rider_seat(p, r.veh(), seat) && seat.bone_index == 6);
	CHECK(r.w.vehicles.detach(r.player_h));
	CHECK(!r.player().mounted);
}

// An untyped bone refuses only after the requester left its seat, so the
// relation changed. [orig: @0x435BC2..0x435BCE, then @0x435CC7..0x435CCC]
void test_an_untyped_bone_refuses_after_the_detach() {
	Rig r;
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
	CHECK(r.player().mounted);
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 5));
	CHECK(!r.player().mounted);
	CHECK(!r.veh().seats[0].occupant.valid());
	// A bone past the userpoint rows, and bone 0, name no row: untyped too.
	CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 9));
	CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 0));
	CHECK(!r.player().mounted);
}

// The authority refuses a held slot, its holder the requester itself included,
// writing nothing. [orig: @0x435B74..0x435BB1]
void test_the_authority_refuses_the_requesters_own_slot() {
	Rig r;
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
	CHECK(!r.w.vehicles.process_attach(r.player_h, r.veh_h, 1));
	CHECK(r.player().mounted && r.player().mount_bone == 1);
	CHECK(r.veh().seats[0].occupant == r.player_h);
}

// A client applying the authority's relation detaches the slot's holder first.
// [orig: @0x435BBA]
void test_a_client_detaches_the_holder() {
	Rig r;
	r.w.ai.is_authority = false;
	Entity other = r.player();
	other.position = {98.0f, 200.0f, 10.0f};
	const EntityHandle other_h = r.w.registry.spawn(0, other);
	CHECK(r.w.vehicles.apply_confirmed_mount(other_h, r.veh_h, 1));
	CHECK(r.w.vehicles.apply_confirmed_mount(r.player_h, r.veh_h, 1));
	CHECK(r.player().mounted && r.veh().seats[0].occupant == r.player_h);
	CHECK(!r.w.registry.get(other_h)->mounted);
}

// A ninth sitex: the def walk stores it in slot 8 (a table row), the sitex search covers
// slots 0..7, so its rider holds no slot; retail still poses the rider by its bone, so the
// rider's seat is the table row holding that bone.
// [orig: the def walk's ninth sitex @0x43A4F0, the scan end @0x43A5AF;
//  Entity_AttachToVehicleSlot @0x4946D0 (@0x4946F3..0x494706 -> @0x494752, the bone store
//  @0x494769); Entity_UpdateInfantryAI @0x4B9910 (@0x4BED58 -> @0x4BED72)]
void test_a_ninth_sitex_rider_is_posed_on_its_row() {
	Rig r;
	Entity &veh = r.veh();
	veh.seats.clear();
	veh.off_table_seats.clear();
	static const char *const names[9] = {"sitex00", "sitex01", "sitex02", "sitex03", "sitex04",
			"sitex05", "sitex06", "sitex07", "sitex08"};
	for (uint8_t i = 0; i < 9; ++i)
		veh.seats.push_back(seat_row(SeatType::Passenger, uint8_t(i + 1), i, names[i]));
	veh.graphic_model_userpoints = 9;
	CHECK(r.w.vehicles.process_attach(r.player_h, r.veh_h, 9));
	const Entity &p = r.player();
	CHECK(p.mounted && p.mount_bone == 9 && p.mount_type == SeatType::Passenger);
	CHECK(p.mount_seat == -1);
	for (const Seat &s : r.veh().seats) CHECK(!s.occupant.valid());
	Seat seat;
	CHECK(r.w.vehicles.rider_seat(p, r.veh(), seat));
	CHECK(seat.bone_index == 9 && seat.retail_slot == 8);
}

} // namespace

int main() {
	test_a_table_bone_takes_its_row();
	test_an_off_table_ctrlx_bone_takes_slot_8();
	test_an_off_table_sitex_bone_holds_no_slot();
	test_an_untyped_bone_refuses_after_the_detach();
	test_the_authority_refuses_the_requesters_own_slot();
	test_a_client_detaches_the_holder();
	test_a_ninth_sitex_rider_is_posed_on_its_row();
	if (failures == 0) std::printf("PASS bone_slot_attach\n");
	return failures == 0 ? 0 : 1;
}
