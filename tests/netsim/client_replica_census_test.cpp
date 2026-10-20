// The S2C census's client-replica consumers (2026-10-20): the tags a stock host
// sends that the reducer dropped as unknown.
//   * S2C 0x79 lands the host's quality metric (CNetQuality +0x0C), 0 for an
//     empty body [orig: NapiNPClientMsg_NetworkQuality @0x429B00];
//   * S2C 0x70 keeps the vehicle-spawn rows and raises the received flag on
//     every reply [orig: NapiNPClientMsg_HandleWeaponLoadoutList @0x429A30];
//   * S2C 0x2A adds a server-command table entry off the authority, into the
//     first slot empty or of its id [orig: NapiNPClientMsg_0x02A @0x425BA0 ->
//     Chat_AddToHistory @0x422920];
//   * the empty handlers (0x07, 0x1C, 0x5E, 0x5F, 0x65, 0x7F) and the stores no
//     client code reads (0x1B, 0x7A) change nothing and are no unknown tag.
// Bodies are built from the cited layouts, never captured.
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <runtime/replication/client_replica_pipeline.h>

using namespace opennova;
using namespace opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

// A 0x2A body: [u8 id][u8 length][8 bytes of name, each XOR 0xFE, zero-padded].
std::vector<uint8_t> command_entry(uint8_t id, const std::string &name) {
	std::vector<uint8_t> body(kServerCommandEntryBytes, 0);
	body[0] = id;
	body[1] = static_cast<uint8_t>(name.size());
	for (std::size_t i = 0; i < name.size() && i < 8; ++i)
		body[2 + i] = static_cast<uint8_t>(name[i] ^ 0xFE);
	return body;
}

bool slot_holds(const ClientServerCommandTable &table, std::size_t slot,
		const std::vector<uint8_t> &body) {
	for (std::size_t i = 0; i < kServerCommandEntryBytes; ++i)
		if (table[slot][i] != body[i]) return false;
	return true;
}

void test_network_quality_lands_the_host_metric() {
	ClientReplicaPipeline view;
	view.apply(s2c::NETWORK_QUALITY, {0xAB});
	CHECK(view.state().net.host_metric == 0xAB);
	// A longer body reads its first byte.
	view.apply(s2c::NETWORK_QUALITY, {0x07, 0x99});
	CHECK(view.state().net.host_metric == 0x07);
	// An empty body stores 0 [orig: @0x429B11].
	view.apply(s2c::NETWORK_QUALITY, {});
	CHECK(view.state().net.host_metric == 0);
	CHECK(view.unknown_tags() == 0);
}

void test_vehicle_spawn_availability_keeps_rows_and_the_flag() {
	ClientReplicaPipeline view;
	CHECK(!view.state().vehicle_spawn_availability.received);
	VehicleSpawnAvailabilityList list;
	list.rows.push_back({0x1004, 2, 4});
	list.rows.push_back({0x100D, kVehicleSpawnUnlimited, kVehicleSpawnUnlimited});
	view.apply(s2c::VEHICLE_SPAWN_AVAILABILITY, encode_vehicle_spawn_availability(list));
	const ClientVehicleSpawnAvailability &a = view.state().vehicle_spawn_availability;
	CHECK(a.received);
	CHECK(a.rows.size() == 2);
	CHECK(a.rows.size() == 2 && a.rows[0].type_id == 0x1004 && a.rows[0].available == 2 &&
			a.rows[0].max_count == 4);
	CHECK(a.rows.size() == 2 && a.rows[1].type_id == 0x100D &&
			a.rows[1].available == kVehicleSpawnUnlimited);

	// A reply cut inside a row still stores what it read (the absent bytes 0)
	// and raises the flag [orig: @0x429A7D / @0x429A8C, both exits @0x429ACE /
	// @0x429AE7].
	ClientReplicaPipeline cut;
	cut.apply(s2c::VEHICLE_SPAWN_AVAILABILITY, {3, 0x04, 0x10, 0x05});
	const ClientVehicleSpawnAvailability &c = cut.state().vehicle_spawn_availability;
	CHECK(c.received);
	CHECK(c.rows.size() == 1 && c.rows[0].type_id == 0x1004 && c.rows[0].available == 5 &&
			c.rows[0].max_count == 0);
	CHECK(cut.unknown_tags() == 0 && view.unknown_tags() == 0);
}

void test_server_command_entries_fill_the_table() {
	ClientReplicaPipeline view;
	// A stock host's six static entries, each id 0 "NULL": the first takes the
	// empty slot 0, the rest match its id and rewrite it.
	const std::vector<uint8_t> null_entry = command_entry(0, "NULL");
	for (int i = 0; i < 6; ++i) view.apply(s2c::CHAT_HISTORY, null_entry);
	const ClientServerCommandTable &t = view.state().server_commands;
	CHECK(slot_holds(t, 0, null_entry));
	CHECK(t[1][1] == 0);
	// Another id takes the next empty slot; the same id again rewrites it.
	view.apply(s2c::CHAT_HISTORY, command_entry(5, "KICK"));
	CHECK(slot_holds(t, 1, command_entry(5, "KICK")));
	view.apply(s2c::CHAT_HISTORY, command_entry(5, "BAN"));
	CHECK(slot_holds(t, 1, command_entry(5, "BAN")));
	CHECK(t[2][1] == 0);
	// A full table of other ids drops a new one [orig: the end test @0x422947].
	for (uint8_t id = 10; id < 24; ++id) view.apply(s2c::CHAT_HISTORY, command_entry(id, "X"));
	CHECK(slot_holds(t, 15, command_entry(23, "X")));
	const ClientServerCommandTable before = t;
	view.apply(s2c::CHAT_HISTORY, command_entry(40, "LATE"));
	CHECK(view.state().server_commands == before);
	// A short body adds nothing (retail would add uninitialized bytes).
	view.apply(s2c::CHAT_HISTORY, {7, 3});
	CHECK(view.state().server_commands == before);
	CHECK(view.unknown_tags() == 0);

	// The authority's own view keeps none [orig: @0x425BB9].
	ClientReplicaPipeline authority;
	authority.set_authority_recipient(true);
	authority.apply(s2c::CHAT_HISTORY, null_entry);
	CHECK(authority.state().server_commands[0][1] == 0);
}

void test_inert_tags_change_nothing() {
	ClientReplicaPipeline view;
	const auto revision = view.state().revision;
	for (uint8_t tag : {s2c::RESERVED_NOOP_07, s2c::NOOP, s2c::RESERVED_NOOP_5E,
				 s2c::RESERVED_NOOP_5F, s2c::RESERVED_NOOP_65, s2c::RESERVED_NOOP_7F})
		view.apply(tag, {1, 2, 3, 4});
	view.apply(s2c::NETWORK_DELAY, {0x10, 0, 0, 0});
	view.apply(s2c::PLAYER_NAME, {'P', 'a', 'r', 'i', 't', 'y', 0});
	CHECK(view.unknown_tags() == 0);
	CHECK(view.malformed_bodies() == 0);
	CHECK(view.state().revision == revision);
}

} // namespace

int main() {
	test_network_quality_lands_the_host_metric();
	test_vehicle_spawn_availability_keeps_rows_and_the_flag();
	test_server_command_entries_fill_the_table();
	test_inert_tags_change_nothing();
	if (failures == 0) std::printf("client_replica_census: ok\n");
	return failures == 0 ? 0 : 1;
}
