// The vehicle screen (vehicle.mnu VEHICLE, D-NET-NEW-L5-5): the client half of
// vehicle spawning, as retail's client runs it.
//   - The screen's first open populates, arms the poll, clears the S2C 0x70
//     received flag and sends one bodiless C2S 0x42; a later open only forgets
//     the selection and refills the list [orig: VehicleMenu_Open @0x564240;
//     VehicleMenu_OnActivate @0x563AD0; UIScene_LoadAndParseContent's CRC skip @0x63C900].
//   - The poll steps a never-reset counter once per main frame: every 64th
//     frame it asks again and, with a reply in, refills; with none yet the
//     counter goes to -1, so it asks every frame until one lands
//     [orig: VehicleMenu_PollAvailability @0x563AF0].
//   - The list rows "<group>   (<avail>/<max>)", "inf" for 255 and no max with
//     an unlimited count, for each group the bay's mask offers with a reply row;
//     the kept selected index re-selects, its value the selection, 0 past the
//     rows [orig: UI_PopulateVehicleWeaponSlotList @0x5638A0; CListWnd_GetRowValue
//     @0x6445F0].
//   - ACCEPT on a vehicle bay sends C2S 0x40 [u16 bay][u8 group]
//     [orig: the VEHICLE button callback @0x563B60; NetPacket_SendWeaponSelectType40
//     @0x42DB50].
// Every value below is an input or follows from the cited arithmetic.

#include <formats/def/def.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/vehicle_menu.h>
#include <runtime/world/entity.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

const std::string kClientScrk = "CLIENT-VEHICLE-SCRK";
const std::string kServerScrk = "SERVER-VEHICLE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E115u;
constexpr uint16_t kSelf = 0x0005;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// Every C2S message of `tag` in the frame's datagrams.
std::vector<ProtocolMessage> sent(const std::vector<std::vector<uint8_t>> &datagrams, uint8_t tag) {
	std::vector<ProtocolMessage> out;
	for (const std::vector<uint8_t> &packet : datagrams) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!nw_decode_inbound(packet.data(), packet.size(), opcode, body) ||
				opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
				!decode_protocol_packet_plaintext(body.data(), body.size(), kClientScrk,
						header, messages)) continue;
		for (ProtocolMessage &message : messages)
			if (message.tag == tag) out.push_back(std::move(message));
	}
	return out;
}

// items.def: row 0 "Null", the two vehicles; four groups, the third and fourth
// naming ids no row carries (their name is row 0's). Ids are authored with
// the 100000 bias the `id` arm strips.
struct Items {
	std::vector<def::DefItemDef> rows;
	def::DefItemsFile file{};
	Items() {
		rows.resize(3);
		const char *names[] = {"Null", "Buggy", "Boat"};
		const int ids[] = {100000, 101291, 101300};
		for (size_t i = 0; i < rows.size(); ++i) {
			std::memset(&rows[i], 0, sizeof(rows[i]));
			std::snprintf(rows[i].display_name, sizeof(rows[i].display_name), "%s", names[i]);
			rows[i].id = ids[i];
		}
		file.entries = rows.data();
		file.count = rows.size();
		const int groups[] = {1291, 1300, 4242, 1310};
		for (int g = 0; g < 4; ++g) file.vehicle_spawn_ids[g] = groups[g];
		file.vehicle_spawn_id_count = 4;
	}
};

// A seeded joiner runtime and a world where L stands on a vehicle bay that
// offers groups 0, 1, 2 and 3. Both on the heap (a World is megabytes).
struct Rig {
	std::unique_ptr<inmatch::ClientRuntime> rt = std::make_unique<inmatch::ClientRuntime>("Driver");
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	Items items;
	w::EntityHandle bay;
	uint32_t now = 1;

	Rig() {
		rt->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk, 1, 0, kSelf,
				w::kPlayerInfantryTypeId);
		world->registry.configure_pool(0, 4);
		world->registry.configure_pool(1, 4);
		w::Entity bay_seed;
		bay_seed.kind = w::EntityKind::Item;
		bay_seed.has_item_def = true;
		bay_seed.item_attrib2 = def::DEF_ITEM_ATTRIB2_VEHICLEBAY;
		bay_seed.vehicle_spawn_groups = {0, 1, 2, 3};
		bay = world->registry.spawn(1, bay_seed);
		w::Entity player;
		player.ground_target = bay;
		world->cached.local_player = world->registry.spawn(0, player);
	}
	inmatch::VehicleMenu &menu() { return rt->vehicle_menu(); }
	// One main frame as the roles run it: the client net frame, then the poll.
	std::vector<std::vector<uint8_t>> frame() {
		std::vector<std::vector<uint8_t>> out = rt->Client_ProcessNetworkFrame(now++);
		rt->poll_vehicle_menu(*world, &items.file);
		return out;
	}
	void reply(const std::vector<VehicleSpawnAvailabilityRow> &rows) {
		VehicleSpawnAvailabilityList list;
		list.rows = rows;
		rt->view().apply(s2c::VEHICLE_SPAWN_AVAILABILITY, encode_vehicle_spawn_availability(list));
	}
};

bool test_the_group_table_names_each_group() {
	const Items items;
	const std::vector<inmatch::VehicleSpawnGroup> groups = inmatch::vehicle_spawn_group_table(items.file);
	return expect(groups.size() == 4, "groups: one per pcvehicle_spawnlist id") &&
			expect(groups[0].item_id == 1291 && groups[0].name == "Buggy",
					"groups: 1291 names the row whose id word (101291 - 100000) is 1291") &&
			expect(groups[1].name == "Boat", "groups: 1300 names Boat") &&
			expect(groups[2].item_id == 4242 && groups[2].name == "Null",
					"groups: an id no row carries reads row 0's name");
}

bool test_the_first_open_asks_once_and_a_later_open_only_refills() {
	Rig r;
	(void)r.frame(); // a quiet frame, nothing armed
	if (!expect(sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).empty(),
			"before the screen opens no 0x42 leaves"))
		return false;
	// A reply from an earlier ask lands while the screen is closed.
	r.reply({{1291, 2, 5}});
	r.menu().open(*r.rt, *r.world, &r.items.file);
	// The init event's populate read the old reply, then the flag went down
	// and the activate event's refill found none.
	if (!expect(!r.rt->state().vehicle_spawn_availability.received,
			"open: the received flag is cleared"))
		return false;
	if (!expect(r.menu().rows().empty() && r.menu().selection() == -1,
			"open: the activate refill lists nothing without a reply"))
		return false;
	std::vector<ProtocolMessage> asks = sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST);
	if (!expect(asks.size() == 1 && asks[0].payload.empty(),
			"open: one bodiless C2S 0x42 leaves at the next boundary"))
		return false;
	// The screen closes (the buttons) and opens again: no second ask.
	(void)r.menu().click(*r.rt, *r.world, /*cancel=*/true);
	r.menu().open(*r.rt, *r.world, &r.items.file);
	return expect(sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).empty(),
			"a later open asks nothing (the init event ran once)");
}

bool test_the_list_reads_the_bay_mask_and_the_reply() {
	Rig r;
	r.menu().open(*r.rt, *r.world, &r.items.file);
	// Group 0 twice (the last row stands), group 1 at 0/0 (not listed), group 2
	// unlimited, group 3 none left of 3.
	r.reply({{1291, 2, 5}, {1300, 0, 0}, {4242, 0xFF, 0xFF}, {1310, 0, 3}, {1291, 1, 4}});
	r.menu().open(*r.rt, *r.world, &r.items.file);
	const std::vector<inmatch::VehicleMenuRow> &rows = r.menu().rows();
	if (!expect(rows.size() == 3, "list: three groups listed")) return false;
	if (!expect(rows[0].text == "Buggy   (1/4)" && rows[0].value == 0,
			"list: the last matching row's counts"))
		return false;
	if (!expect(rows[1].text == "Null   (inf)" && rows[1].value == 2,
			"list: an unlimited count shows no max"))
		return false;
	if (!expect(rows[2].text == "Null   (0/3)" && rows[2].value == 3,
			"list: none left of a cap is listed"))
		return false;
	if (!expect(r.menu().selected_row() == 0 && r.menu().selection() == 0,
			"list: row 0 selected, its value the selection"))
		return false;
	// A pick: row 1, group 2.
	r.menu().select_row(1);
	if (!expect(r.menu().selected_row() == 1 && r.menu().selection() == 2,
			"pick: the row's value is the selection"))
		return false;
	// The bay stops offering group 0. A refill keeps the selected INDEX, not
	// the group: row 1 is now group 3.
	r.world->registry.get(r.bay)->vehicle_spawn_groups = {2, 3};
	r.menu().open(*r.rt, *r.world, &r.items.file);
	if (!expect(r.menu().rows().size() == 2 && r.menu().selected_row() == 1 &&
					r.menu().selection() == 3,
			"refill: the kept index 1 now carries group 3"))
		return false;
	// One row left: the kept index 1 is past the rows, so nothing is
	// selected and the selection reads 0.
	r.world->registry.get(r.bay)->vehicle_spawn_groups = {2};
	r.menu().open(*r.rt, *r.world, &r.items.file);
	return expect(r.menu().rows().size() == 1 && r.menu().selected_row() == -1 &&
					r.menu().selection() == 0,
			"refill: an index past the rows selects nothing and reads value 0");
}

bool test_the_poll_asks_every_frame_until_a_reply_then_every_64() {
	Rig r;
	r.menu().open(*r.rt, *r.world, &r.items.file);
	// The frame after the open ships the open's ask; that frame's poll is 1.
	int asks = static_cast<int>(sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).size());
	if (!expect(asks == 1, "poll: the open's ask")) return false;
	// Polls 2..63 ask nothing; poll 64 asks (shipped by the next frame).
	for (int poll = 2; poll <= 64; ++poll)
		if (!expect(sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).empty(),
				"poll: no ask before the 64th poll"))
			return false;
	// No reply: the counter went to -1, so every poll from here asks again.
	for (int k = 0; k < 5; ++k)
		if (!expect(sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).size() == 1,
				"poll: one ask per frame while no reply has landed"))
			return false;
	// A reply lands before this frame's poll, which asks once more and keeps
	// the counter; then 63 quiet polls and the 64th asks.
	r.reply({{1291, 2, 5}});
	if (!expect(sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).size() == 1,
			"poll: the frame shipping the last retry"))
		return false;
	if (!expect(sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).size() == 1,
			"poll: the ask of the poll that saw the reply"))
		return false;
	int quiet = 0;
	for (;;) {
		const size_t n = sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).size();
		if (n != 0) break;
		if (++quiet > 200) break;
	}
	if (!expect(quiet == 63, "poll: after the reply one ask every 64 frames")) return false;
	if (!expect(r.menu().rows().size() == 1, "poll: the 64th poll refilled from the reply"))
		return false;
	// Closing the screen does not stop the poll (only the scene's destroy
	// disarms it), and a re-open does not reset the counter.
	(void)r.menu().click(*r.rt, *r.world, /*cancel=*/true);
	r.menu().open(*r.rt, *r.world, &r.items.file);
	quiet = 0;
	for (;;) {
		const size_t n = sent(r.frame(), c2s::VEHICLE_SPAWN_AVAILABILITY_REQUEST).size();
		if (n != 0) break;
		if (++quiet > 200) break;
	}
	return expect(quiet == 63, "poll: closed or re-opened, the 64-frame cadence runs on");
}

bool test_accept_sends_the_bay_and_the_group() {
	Rig r;
	r.menu().open(*r.rt, *r.world, &r.items.file);
	r.reply({{1291, 2, 5}, {4242, 0xFF, 0xFF}});
	r.menu().open(*r.rt, *r.world, &r.items.file);
	(void)r.frame();
	r.menu().select_row(1); // group 2
	// CANCEL sends nothing.
	if (!expect(!r.menu().click(*r.rt, *r.world, /*cancel=*/true) &&
					sent(r.frame(), c2s::VEHICLE_SPAWN_REQUEST).empty(),
			"cancel: no 0x40"))
		return false;
	if (!expect(r.menu().click(*r.rt, *r.world, /*cancel=*/false), "accept: queued")) return false;
	const std::vector<ProtocolMessage> picks = sent(r.frame(), c2s::VEHICLE_SPAWN_REQUEST);
	const uint16_t h = r.bay.packed;
	const std::vector<uint8_t> expected = {static_cast<uint8_t>(h & 0xFF),
			static_cast<uint8_t>(h >> 8), 2};
	if (!expect(picks.size() == 1 && picks[0].payload == expected,
			"accept: C2S 0x40 [u16 bay handle][u8 group 2]"))
		return false;
	// Off a vehicle bay (its VEHICLEBAY bit clear) ACCEPT only closes.
	r.world->registry.get(r.bay)->item_attrib2 = 0;
	if (!expect(!r.menu().click(*r.rt, *r.world, false) &&
					sent(r.frame(), c2s::VEHICLE_SPAWN_REQUEST).empty(),
			"accept: no 0x40 off a vehicle bay"))
		return false;
	// With no ground at all likewise.
	r.world->registry.get(r.bay)->item_attrib2 = def::DEF_ITEM_ATTRIB2_VEHICLEBAY;
	r.world->registry.get(r.world->cached.local_player)->ground_target = w::EntityHandle{};
	return expect(!r.menu().click(*r.rt, *r.world, false) &&
					sent(r.frame(), c2s::VEHICLE_SPAWN_REQUEST).empty(),
			"accept: no 0x40 with no ground entity");
}

bool test_the_reply_and_the_poll_outlive_a_mission_reload() {
	Rig r;
	r.menu().open(*r.rt, *r.world, &r.items.file);
	r.reply({{1291, 2, 5}});
	if (!expect(r.rt->begin_mission_reload(), "reload: begins")) return false;
	if (!expect(r.rt->state().vehicle_spawn_availability.received &&
					r.rt->state().vehicle_spawn_availability.rows.size() == 1,
			"reload: the 0x70 table and its flag stay"))
		return false;
	// The reload's admission runs no main frame: the poll counts nothing.
	const int32_t before = r.menu().poll_counter();
	r.rt->poll_vehicle_menu(*r.world, &r.items.file);
	return expect(r.menu().poll_armed() && r.menu().poll_counter() == before,
			"reload: no poll while the mission start runs");
}

} // namespace

int main() {
	bool ok = true;
	ok &= test_the_group_table_names_each_group();
	ok &= test_the_first_open_asks_once_and_a_later_open_only_refills();
	ok &= test_the_list_reads_the_bay_mask_and_the_reply();
	ok &= test_the_poll_asks_every_frame_until_a_reply_then_every_64();
	ok &= test_accept_sends_the_bay_and_the_group();
	ok &= test_the_reply_and_the_poll_outlive_a_mission_reload();
	if (ok) std::printf("vehicle_menu: ok\n");
	return ok ? 0 : 1;
}
