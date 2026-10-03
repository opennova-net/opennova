// Three witnessed net legs that had no coverage:
//
//  (A) S2C 0x50 TEAM ASSIGN — the SECOND writer of the byte_A85B48 team latch (the
//      S2C 0x04 tail is the first). Self -> re-latch + ONE C2S 0x2F re-submission at
//      slot 195 RAW; any valid handle -> the decoded row's team. The re-submission is a
//      PER-SIDE PROFILE RESELECT: the NEW team byte picks the profile side block, that
//      block's class byte is the wire class, and the page that class indexes is the kit.
//      [orig: NapiNPClientMsg_TeamAssign (0x50) @0x431910 — latch @0x4319db, entity team @0x4319ee,
//       slot team @0x431a0b, the side pick @0x431a35, the re-submit @0x431a9e]
//
//  (B) The ENTITY-REMOVAL fold. C2S 0x32 -> S2C 0x5D is the empty-slot sweep: the host
//      answers the REQUESTER ONLY with every empty pool-0 index, and the client destroys
//      whatever it still holds there. S2C 0x46 bit15 clears roster BOOKKEEPING ONLY —
//      the entity is never destroyed on that leg.
//      [orig: NapiNPServerMsg_SendEmptySlots @0x51a600 (builder @0x5160f0) ->
//       NapiNPClientMsg_DestroyEntityList @0x429730; NapiNPClientMsg_PlayerSync
//       @0x431370 @0x431411..0x43144c -> PlayerSlot_ClearAndUnlink @0x434730]
//
//  (C) The IN-MATCH SESSION-LOSS surface. Retail's transport reaps a silent peer at
//      cs_dir0.timeout_ms = 120000 and the disconnect EXITS THE MISSION with a reason.
//      [orig: CNapiNetwork_Init @0x4ca4a0 (timeout stores @0x4caa81/@0x4cab54) ->
//       CNapiNetwork_OnDisconnectedFromServer @0x4c63d0]
//
// Every assertion starts from a state where the asserted value DIFFERS from its prior
// value, so a no-op implementation cannot pass.

#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/joiner_connection.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/server_message_dispatch.h>

#include <runtime/replication/connection.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/session_transport.h>
#include <runtime/inmatch/udp_session_transport.h>

#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/protocol_message.h>
#include <net/npwire/session_keys.h>

#include <runtime/world/ai.h>
#include <runtime/world/player_spawn.h>
#include <runtime/world/world.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

const std::string kClientScrk = "CLIENT-LIFECYCLE-SCRK";
const std::string kServerScrk = "SERVER-LIFECYCLE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;

// Frame one or more inner S2C records as a 0x83 SESSION datagram the joiner accepts.
std::vector<uint8_t> frame_server_session(SessionSequencing &seq,
		const std::vector<ProtocolMessage> &messages) {
	std::vector<uint8_t> body;
	if (!frame_session_packet(
			seq, SessionCrypto{kServerScrk, {}, kClientKey}, messages, body)) {
		return {};
	}
	return nw_encode_outbound(
			SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
}

// The witnessed 24-byte S2C 0x04 slot assignment; the tail byte is the team latch.
// [orig: NetPacket_WriteSlotAssignment @0x502b30 -> byte_A85B48 @0x425499]
std::vector<uint8_t> slot_assignment(uint8_t team) {
	std::vector<uint8_t> body(24, 0);
	body[17] = 0x01; // the joiner's roster slot
	body[18] = 0x18; // host slot capacity
	body[23] = team;
	return body;
}

// One named pool-0 organic-spawn record (the load-time world stream, §5.23).
std::vector<uint8_t> organic_spawn(uint16_t handle, const std::string &name, uint8_t team) {
	OrganicSpawnBatch batch;
	OrganicSpawnRecord rec;
	rec.slot_id = handle;
	rec.def_type = 3; // ItemDefType person
	rec.item_type_id = w::kPlayerInfantryTypeId;
	rec.entity_name = name;
	rec.pos_x = w::to_fixed(10.0);
	rec.pos_y = w::to_fixed(20.0);
	rec.pos_z = w::to_fixed(1.0);
	rec.team = team;
	batch.records.push_back(rec);
	batch.entity_count = 1;
	return encode_organic_spawn_batch(batch);
}

// Decode the C2S records a joiner queued into its outbound datagrams.
std::vector<ProtocolMessage> client_records(
		const std::vector<std::vector<uint8_t>> &datagrams) {
	std::vector<ProtocolMessage> all;
	for (const std::vector<uint8_t> &dg : datagrams) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!nw_decode_inbound(dg.data(), dg.size(), opcode, body)) continue;
		if (opcode != SESSION_OPCODE_PROTOCOL_MESSAGE) continue;
		if (!decode_protocol_packet_plaintext(
				body.data(), body.size(), kClientScrk, header, messages))
			continue;
		for (ProtocolMessage &m : messages) all.push_back(std::move(m));
	}
	return all;
}

const ns::ClientEntityState *row_for(const ns::ClientState &state, uint16_t handle) {
	for (const ns::ClientEntityState &e : state.entities)
		if (e.handle == handle) return &e;
	return nullptr;
}

// ---------------------------------------------------------------------------------
// (A) S2C 0x50
// ---------------------------------------------------------------------------------

// The self leg: the latch moves off the 0x04 value and ONE C2S 0x2F goes out carrying
// the NEW team, the kit class, and slot 195 raw.
bool run_team_assign_relatches_self_and_resubmits() {
	constexpr uint16_t kSelf = 0x0005;
	inmatch::JoinerConnection joiner("TeamJoiner", [] { return uint64_t(0); });
	inmatch::JoinerConnection::LoadoutKit kit;
	kit.player_class = 6;
	kit.equipped_combo = 212; // deliberately NOT 195: the re-submit must ignore it
	kit.rows.push_back(LoadoutSubmitEntry{0x18, 0x03, 0xFF, 0xFF});
	joiner.set_loadout_kit(kit);
	joiner.seed_in_match(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                     1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	// Prior state: the 0x04 tail latched team 1.
	const std::vector<uint8_t> assign_04 =
			frame_server_session(server_tx, {make_protocol_message(0x04, slot_assignment(1))});
	(void)joiner.handle_datagram(assign_04.data(), assign_04.size());
	if (!expect(joiner.assigned_team() == 1,
			"0x50-self: the 0x04 tail latched team 1 (the prior value)"))
		return false;

	// The 0x50 re-latch to a DIFFERENT team.
	TeamAssign assign;
	assign.entity_handle = kSelf;
	assign.team = 3;
	assign.net_id = 0x1234;
	assign.anim_slot = 1;
	const std::vector<uint8_t> assign_payload = encode_team_assign(assign);
	const std::vector<uint8_t> dg = frame_server_session(
			server_tx, {make_protocol_message(0x50, assign_payload)});
	const inmatch::JoinerConnection::PollResult poll =
			joiner.handle_datagram(dg.data(), dg.size());

	if (!expect(joiner.assigned_team() == 3,
			"0x50-self: the team latch moved to the assigned team"))
		return false;
	if (!expect(poll.self_team_changed && poll.self_team == 3,
			"0x50-self: the own-team change is surfaced to the binding"))
		return false;
	if (!expect(poll.entity_team_assigns.size() == 1 &&
				poll.entity_team_assigns[0].first == kSelf &&
				poll.entity_team_assigns[0].second == 3,
			"0x50-self: the decoded-view fold also names the entity"))
		return false;
	if (!expect(poll.inbound_gameplay.size() == 1 &&
				poll.inbound_gameplay[0].first == 0x50 &&
				poll.inbound_gameplay[0].second == assign_payload,
			"0x50-self: the validated raw record reaches the replica pipeline"))
		return false;

	int submits = 0;
	LoadoutSubmit submitted;
	for (const ProtocolMessage &m : poll.queued_send_messages) {
		if (m.tag != 0x2F) continue;
		++submits;
		if (!expect(decode_loadout_submit(
					m.payload.data(), m.payload.size(), submitted),
				"0x50-self: the re-submission body decodes"))
			return false;
	}
	if (!expect(submits == 1, "0x50-self: EXACTLY one C2S 0x2F re-submission")) return false;
	if (!expect(submitted.team == 3, "0x50-self: the re-submission carries the NEW team"))
		return false;
	if (!expect(submitted.player_class == 6,
			"0x50-self: the re-submission carries the applied kit's class"))
		return false;
	// [orig: @0x431a9e passes 195 RAW, not the live g_CurrentWeaponSlot (212 here)]
	if (!expect(submitted.weapon_slot_index == 195,
			"0x50-self: the re-submission uses slot 195 raw, not the equipped combo"))
		return false;
	return expect(submitted.entries.size() == 1 && submitted.entries[0].adm_index == 0x18,
			"0x50-self: the re-submission carries the applied kit rows");
}

// Pull the single C2S 0x2F this poll queued. Zero or more than one fails loudly: retail
// emits EXACTLY one re-submission per self team assign [orig: @0x431a9e].
bool one_loadout_submit(const inmatch::JoinerConnection::PollResult &poll,
		std::vector<uint8_t> &payload_out, LoadoutSubmit &decoded_out, const char *what) {
	int submits = 0;
	for (const ProtocolMessage &m : poll.queued_send_messages) {
		if (m.tag != 0x2F) continue;
		++submits;
		payload_out = m.payload;
	}
	if (!expect(submits == 1, what)) return false;
	return expect(decode_loadout_submit(
				payload_out.data(), payload_out.size(), decoded_out),
			"0x50-profile: the re-submission body decodes");
}

bool same_rows(const std::vector<LoadoutSubmitEntry> &got,
		const std::vector<LoadoutSubmitEntry> &want, const char *what) {
	if (!expect(got.size() == want.size(), what)) return false;
	for (std::size_t i = 0; i < want.size(); ++i) {
		if (!expect(got[i].adm_index == want[i].adm_index &&
					got[i].ammo_primary == want[i].ammo_primary &&
					got[i].ammo_secondary == want[i].ammo_secondary &&
					got[i].variant == want[i].variant,
				what))
			return false;
	}
	return true;
}

// The PER-SIDE PROFILE RESELECT of the S2C 0x50 self leg. Retail does not resend "the kit
// it already applied": it re-reads the profile side block the NEW team byte names, takes
// THAT block's class byte as the wire class, and submits the 2048-byte page that class
// indexes inside the same block. One integer picks both, which is why a retail client's
// kit is always legal for the class it is paired with — the host tests the .adm
// charfilter against that class byte [orig: NapiNPServerMsg_HandlePlayerLoadout
// @0x515A36] and drops every row that fails it.
// [orig: NapiNPClientMsg_TeamAssign @0x431910 — the side pick @0x431a35, profileType =
//  *profileData, the page qmemcpy, then NetPacket_SendLoadoutSubmit(team, profileType,
//  restrictionData, 195) @0x431a9e]
bool run_team_assign_reselects_the_new_sides_profile() {
	constexpr uint16_t kSelf = 0x0005;
	inmatch::JoinerConnection joiner("SideJoiner", [] { return uint64_t(0); });
	inmatch::JoinerConnection::LoadoutKit kit;
	// The single applied kit is deliberately DIFFERENT from BOTH side blocks, so an
	// implementation that keeps submitting "the one applied kit" cannot pass.
	kit.player_class = 9;
	kit.equipped_combo = 212; // and the reselect must ignore the live equipped combo
	kit.rows.push_back(LoadoutSubmitEntry{0x2A, 0x01, 0xFF, 0xFF});
	kit.blue.set = true;
	kit.blue.player_class = 6; // the BLUE block's class byte: sniper
	kit.blue.rows.push_back(LoadoutSubmitEntry{0x1A, 0xFF, 0xFF, 0xFF});
	kit.red.set = true;
	kit.red.player_class = 8; // the RED block's class byte: rifleman
	kit.red.rows.push_back(LoadoutSubmitEntry{0x18, 0xFF, 0xFF, 0xFF});
	kit.red.rows.push_back(LoadoutSubmitEntry{0x2C, 0x02, 0xFF, 0x01});
	joiner.set_loadout_kit(kit);
	joiner.seed_in_match(kSessionId, kClientKey, kClientScrk, kServerScrk,
						 1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	// Prior state: the 0x04 tail latched team 1 — a BLUE team.
	{
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx, {make_protocol_message(0x04, slot_assignment(1))});
		(void)joiner.handle_datagram(dg.data(), dg.size());
	}
	if (!expect(joiner.assigned_team() == 1,
			"0x50-profile: the 0x04 tail latched blue team 1"))
		return false;

	// WITHIN one side: teams 1 and 3 are both blue. Retail compares nothing — the
	// handler re-picks and re-submits unconditionally — so a re-submission still goes
	// out, carrying the SAME (blue) block.
	{
		TeamAssign assign;
		assign.entity_handle = kSelf;
		assign.team = 3;
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx,
				{make_protocol_message(0x50, encode_team_assign(assign))});
		const inmatch::JoinerConnection::PollResult poll =
				joiner.handle_datagram(dg.data(), dg.size());
		std::vector<uint8_t> payload;
		LoadoutSubmit got;
		if (!one_loadout_submit(poll, payload, got,
				"0x50-profile: a same-side change still re-submits exactly once"))
			return false;
		if (!expect(got.team == 3,
				"0x50-profile: the same-side re-submit carries team 3"))
			return false;
		if (!expect(got.player_class == 6,
				"0x50-profile: team 3 is BLUE, so the blue block's class byte goes out"))
			return false;
		if (!expect(got.weapon_slot_index == 195,
				"0x50-profile: slot 195 raw, never the equipped combo 212"))
			return false;
		if (!same_rows(got.entries, kit.blue.rows,
				"0x50-profile: the BLUE page's rows go out, not the applied kit's"))
			return false;
	}

	// ACROSS the line: 3 (blue) -> 2 (red). Both the class byte and the page must flip
	// to the red block. Holding the blue class while shipping a red-side kit is exactly
	// the pair a live retail host drops row by row.
	{
		TeamAssign assign;
		assign.entity_handle = kSelf;
		assign.team = 2;
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx,
				{make_protocol_message(0x50, encode_team_assign(assign))});
		const inmatch::JoinerConnection::PollResult poll =
				joiner.handle_datagram(dg.data(), dg.size());
		if (!expect(joiner.assigned_team() == 2,
				"0x50-profile: the latch crossed to the red team"))
			return false;
		std::vector<uint8_t> payload;
		LoadoutSubmit got;
		if (!one_loadout_submit(poll, payload, got,
				"0x50-profile: the cross-side change re-submits exactly once"))
			return false;
		if (!expect(got.team == 2,
				"0x50-profile: the re-submit carries the NEW team 2"))
			return false;
		if (!expect(got.player_class == 8,
				"0x50-profile: the RED block's OWN class byte replaces the blue one"))
			return false;
		if (!expect(got.weapon_slot_index == 195,
				"0x50-profile: the cross-side re-submit also uses slot 195 raw"))
			return false;
		if (!same_rows(got.entries, kit.red.rows,
				"0x50-profile: the RED page's rows replace the blue page's"))
			return false;
	}
	return true;
}

// A caller that never calls set_loadout_kit keeps the canned capture kit BYTE FOR BYTE —
// the headless ctests, nw_replay and seed_in_match all pin it. The per-side seam must not
// disturb that fallback.
bool run_team_assign_default_kit_stays_byte_identical() {
	constexpr uint16_t kSelf = 0x0005;
	inmatch::JoinerConnection joiner("DefaultKitJoiner", [] { return uint64_t(0); });
	// deliberately NO set_loadout_kit
	joiner.seed_in_match(kSessionId, kClientKey, kClientScrk, kServerScrk,
						 1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	{
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx, {make_protocol_message(0x04, slot_assignment(1))});
		(void)joiner.handle_datagram(dg.data(), dg.size());
	}
	TeamAssign assign;
	assign.entity_handle = kSelf;
	assign.team = 2;
	const std::vector<uint8_t> dg = frame_server_session(
			server_tx, {make_protocol_message(0x50, encode_team_assign(assign))});
	const inmatch::JoinerConnection::PollResult poll =
			joiner.handle_datagram(dg.data(), dg.size());

	std::vector<uint8_t> payload;
	LoadoutSubmit got;
	if (!one_loadout_submit(poll, payload, got,
			"0x50-default: the unset-kit path still re-submits exactly once"))
		return false;

	// The golden joiner's profile kit from retail-lan-host-join-session, under the NEW
	// team byte and slot 195 raw.
	LoadoutSubmit want;
	want.team = 2;
	want.player_class = 0x08;
	want.weapon_slot_index = 195;
	for (uint8_t adm : {
			uint8_t{0x18}, uint8_t{0x03}, uint8_t{0x2C}, uint8_t{0x28},
			uint8_t{0x29}, uint8_t{0x2A}, uint8_t{0x02},
	}) {
		want.entries.push_back(LoadoutSubmitEntry{adm, 0xFF, 0xFF, 0xFF});
	}
	return expect(payload == encode_loadout_submit(want),
			"0x50-default: the canned capture kit goes out BYTE-IDENTICAL");
}

// A 0x50 for a REMOTE handle folds the team onto that row and sends nothing.
bool run_team_assign_folds_remote_row() {
	constexpr uint16_t kSelf = 0x0005;
	constexpr uint16_t kRemote = 0x0009;
	inmatch::ClientRuntime client("FoldJoiner", [] { return uint64_t(0); });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	// Prior state: the remote spawned on team 1.
	{
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx,
				{make_protocol_message(0x0C, organic_spawn(kRemote, "Peer", 1))});
		client.receive(dg.data(), dg.size());
		(void)client.Client_ProcessNetworkFrame(1);
	}
	const ns::ClientEntityState *before = row_for(client.state(), kRemote);
	if (!expect(before != nullptr && before->team == 1,
			"0x50-remote: the row starts on the spawned team 1"))
		return false;

	TeamAssign assign;
	assign.entity_handle = kRemote;
	assign.team = 2;
	const std::vector<uint8_t> dg = frame_server_session(
			server_tx, {make_protocol_message(0x50, encode_team_assign(assign))});
	client.receive(dg.data(), dg.size());
	const std::vector<std::vector<uint8_t>> outs = client.Client_ProcessNetworkFrame(2);

	const ns::ClientEntityState *after = row_for(client.state(), kRemote);
	if (!expect(after != nullptr && after->team == 2,
			"0x50-remote: the decoded row's team followed the assignment"))
		return false;
	if (!expect(client.assigned_team() == 0,
			"0x50-remote: a remote assignment does not touch our own latch"))
		return false;
	if (!expect(client.self_team_revision() == 0,
			"0x50-remote: no own-team edge for a remote assignment"))
		return false;
	for (const ProtocolMessage &m : client_records(outs))
		if (!expect(m.tag != 0x2F, "0x50-remote: no loadout re-submission")) return false;
	return true;
}

// ---------------------------------------------------------------------------------
// (B) the removal fold
// ---------------------------------------------------------------------------------

// The joiner requests the sweep from its S2C 0x0F reply burst, and the 0x5D reply
// retires the decoded row (the permanent-ghost fix).
bool run_empty_slot_sweep_surfaces_raw_gameplay() {
	inmatch::JoinerConnection joiner("SweepPollJoiner", [] { return uint64_t(0); });
	joiner.seed_in_match(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                     1, 0, 0x0005, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	DestroyEntityList sweep;
	sweep.pool0_indices = {0x0003, 0x0007};
	const std::vector<uint8_t> sweep_payload = encode_destroy_entity_list(sweep);
	const std::vector<uint8_t> dg = frame_server_session(
			server_tx, {make_protocol_message(0x5D, sweep_payload)});
	const inmatch::JoinerConnection::PollResult poll =
			joiner.handle_datagram(dg.data(), dg.size());

	if (!expect(poll.destroyed_pool0_slots == sweep.pool0_indices,
			"0x5D-poll: decoded slot diagnostics remain available"))
		return false;
	return expect(poll.inbound_gameplay.size() == 1 &&
				poll.inbound_gameplay[0].first == 0x5D &&
				poll.inbound_gameplay[0].second == sweep_payload,
			"0x5D-poll: the validated raw record reaches the replica pipeline");
}

bool run_deployed_item_lifecycle_surfaces_raw_gameplay() {
	inmatch::JoinerConnection joiner("PlacedDevicePollJoiner", [] { return uint64_t(0); });
	joiner.seed_in_match(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                     1, 0, 0x0005, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	DeployedItemSpawn spawn;
	spawn.item_id = 0x0361;
	spawn.owner_handle = 0x0005;
	spawn.friendly_item_id = 0x0362;
	spawn.enemy_item_id = 0x0363;
	spawn.slot_handle = 0x1003;
	spawn.parent_handle = 0xFFFF;
	spawn.pos_x = w::to_fixed(4.0);
	spawn.pos_y = w::to_fixed(5.0);
	spawn.pos_z = w::to_fixed(1.0);
	const std::vector<uint8_t> spawn_payload =
			encode_deployed_item_spawn(spawn);
	EntityRemove removal;
	removal.entity_handle = spawn.slot_handle;
	const std::vector<uint8_t> removal_payload = encode_entity_remove(removal);
	const std::vector<uint8_t> dg = frame_server_session(server_tx, {
			make_protocol_message(s2c::DEPLOYED_ITEM, spawn_payload),
			make_protocol_message(s2c::ENTITY_REMOVE, removal_payload),
	});
	const inmatch::JoinerConnection::PollResult poll =
			joiner.handle_datagram(dg.data(), dg.size());

	if (!expect(poll.inbound_gameplay.size() == 2 &&
			poll.inbound_gameplay[0].first == s2c::DEPLOYED_ITEM &&
			poll.inbound_gameplay[0].second == spawn_payload &&
			poll.inbound_gameplay[1].first == s2c::ENTITY_REMOVE &&
			poll.inbound_gameplay[1].second == removal_payload,
			"validated 0x59/0x12 lifecycle surfaces on the diagnostic view"))
		return false;
	// ClientRuntime folds ONLY the reducer stream — a record absent there never
	// reaches ClientReplicaPipeline (the post-#498 review regression: 0x59/0x12
	// rode the diagnostic vector alone and joiners dropped every placed device).
	std::vector<std::pair<uint8_t, std::vector<uint8_t>>> lifecycle;
	for (const auto &tb : poll.inbound_reducer)
		if (tb.first == s2c::DEPLOYED_ITEM || tb.first == s2c::ENTITY_REMOVE)
			lifecycle.push_back(tb);
	return expect(lifecycle.size() == 2 &&
			lifecycle[0].first == s2c::DEPLOYED_ITEM &&
			lifecycle[0].second == spawn_payload &&
			lifecycle[1].first == s2c::ENTITY_REMOVE &&
			lifecycle[1].second == removal_payload,
			"validated 0x59/0x12 placed-device lifecycle rides the applied reducer stream in wire order");
}

// End to end: the datagrams cross ClientRuntime::run_frame and the replica row
// appears, then retires. [orig: client dispatch table @0x82ae28 — 0x59 ->
// NapiNPClientMsg_0x059 @0x4228e0 -> Entity_SpawnOrUpdateFromSlotPacket
// @0x546770; 0x12 -> NapiNPClientMsg_0x012 @0x425ee0 -> Entity_Destroy
// @0x43e810 — both applied on receipt]
bool run_deployed_item_lifecycle_folds_into_the_replica() {
	constexpr uint16_t kSelf = 0x0005;
	inmatch::ClientRuntime client("PlacedDeviceFoldJoiner", [] { return uint64_t(0); });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	DeployedItemSpawn spawn;
	spawn.item_id = 0x0361;
	spawn.owner_handle = kSelf;
	spawn.friendly_item_id = 0x0362;
	spawn.enemy_item_id = 0x0363;
	spawn.slot_handle = 0x1003;
	spawn.parent_handle = 0xFFFF;
	spawn.pos_x = w::to_fixed(4.0);
	spawn.pos_y = w::to_fixed(5.0);
	spawn.pos_z = w::to_fixed(1.0);
	{
		const std::vector<uint8_t> dg = frame_server_session(server_tx,
				{make_protocol_message(s2c::DEPLOYED_ITEM,
						encode_deployed_item_spawn(spawn))});
		client.receive(dg.data(), dg.size());
		(void)client.Client_ProcessNetworkFrame(1);
	}
	const ns::ClientEntityState *row = row_for(client.state(), spawn.slot_handle);
	if (!expect(row != nullptr && row->spawn_tag == s2c::DEPLOYED_ITEM,
			"0x59 through run_frame materializes the placed-device row"))
		return false;

	EntityRemove removal;
	removal.entity_handle = spawn.slot_handle;
	{
		const std::vector<uint8_t> dg = frame_server_session(server_tx,
				{make_protocol_message(s2c::ENTITY_REMOVE,
						encode_entity_remove(removal))});
		client.receive(dg.data(), dg.size());
		(void)client.Client_ProcessNetworkFrame(2);
	}
	return expect(row_for(client.state(), spawn.slot_handle) == nullptr,
			"0x12 through run_frame retires the placed-device row");
}

bool run_empty_slot_sweep_retires_the_row() {
	constexpr uint16_t kSelf = 0x0005;
	constexpr uint16_t kGhost = 0x0003;
	inmatch::ClientRuntime client("SweepJoiner", [] { return uint64_t(0); });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	// Prior state: the peer's row EXISTS.
	{
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx,
				{make_protocol_message(0x0C, organic_spawn(kGhost, "Leaver", 1))});
		client.receive(dg.data(), dg.size());
		(void)client.Client_ProcessNetworkFrame(1);
	}
	if (!expect(row_for(client.state(), kGhost) != nullptr,
			"0x5D: the peer's row exists before the sweep"))
		return false;

	// The S2C 0x0F world-state-load reply burst must carry the C2S 0x32 request —
	// without it a stock host never sends a sweep. [orig: @0x42e647]
	{
		// The fixed 0x0F header through gameFlags (byte 22) plus the score block —
		// only its arrival matters here; the burst reply is what is under test.
		std::vector<uint8_t> body(
				4u + 12u + 6u + 1u + 4u * kWorldStateAmmoPoolCount + 4u, 0);
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx, {make_protocol_message(0x0F, body)});
		client.receive(dg.data(), dg.size());
		const std::vector<std::vector<uint8_t>> outs = client.Client_ProcessNetworkFrame(2);
		int requests = 0;
		for (const ProtocolMessage &m : client_records(outs))
			if (m.tag == 0x32) ++requests;
		if (!expect(requests == 1,
				"0x32: the 0x0F reply burst carries exactly one sweep request"))
			return false;
	}

	// The reply: the host lists that pool-0 index as empty.
	DestroyEntityList sweep;
	sweep.pool0_indices.push_back(kGhost);
	const std::vector<uint8_t> dg = frame_server_session(
			server_tx, {make_protocol_message(0x5D, encode_destroy_entity_list(sweep))});
	client.receive(dg.data(), dg.size());
	(void)client.Client_ProcessNetworkFrame(3);

	return expect(row_for(client.state(), kGhost) == nullptr,
			"0x5D: the swept row is GONE (no ghost person proxy survives)");
}

// ---------------------------------------------------------------------------------
// (D) the S2C 0x13 / 0x26 entity-death folds on a destructible row
// ---------------------------------------------------------------------------------

// Only the organic death transaction sends S2C 0x13 [u16 handle][i16 deathAnim];
// a destructible's live death channel is the 0x26 kill-sync its class callback
// sends. The client's 0x13 handler takes any valid handle, so both tags are
// pinned here on one streamed static: the connection surfaces the validated
// body, the replica fold zeroes the row's health, and drain_effect_commands hands
// the record to the embedding sim exactly once, so it can run the class death
// callback (reason 4 — the husk/explosion chain) on the world twin. Regression:
// both tags used to be dropped at the connection's tag chain.
// [orig: senders Entity_CheckAndProcessDeath @0x51b550 (msg 19, mask 0x90,
//  called only for organics) and GameEvent_PlayerDeath @0x516E8E; handler
//  NapiNPClientMsg_EntityDeath @0x42EB50 — Health = 0 @0x42ebd6,
//  deathCallback(entity, 4, 0) @0x42ebf5]
bool run_entity_death_notify_reaches_the_sim() {
	constexpr uint16_t kSelf = 0x0005;
	// A pool-2 static in the 0x10 tail beyond the former 1024 clamp: the death
	// fold only works if the load fold accepted the full retail pool range.
	constexpr uint16_t kBarrel = 0x2000u | 1130u;
	inmatch::ClientRuntime client("DeathJoiner", [] { return uint64_t(0); });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	// Prior state: the streamed static row exists with no witnessed health.
	{
		StaticEntityBatch batch;
		batch.start_index = 1130;
		StaticEntityRecord rec;
		rec.item_type_id = 0x10EB;
		rec.pos_x = 753 * 65536;
		rec.pos_y = 762 * 65536;
		rec.pos_z = 42 * 65536;
		batch.records.push_back(rec);
		batch.entity_count = 1;
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx, {make_protocol_message(
						0x10, encode_static_entity_batch(batch))});
		client.receive(dg.data(), dg.size());
		(void)client.Client_ProcessNetworkFrame(1);
	}
	const ns::ClientEntityState *before = row_for(client.state(), kBarrel);
	if (!expect(before != nullptr && !before->health_known,
			"0x13: the streamed static row exists, health unwitnessed"))
		return false;
	if (!expect(client.view().drain_effect_commands().empty(),
			"0x13: no death surfaced before the notify"))
		return false;

	// The 4-byte notify [orig: NetPacket_BuildDeathNotifyPayload @0x5036e0].
	std::vector<uint8_t> body13;
	body13.push_back(static_cast<uint8_t>(kBarrel & 0xFFu));
	body13.push_back(static_cast<uint8_t>(kBarrel >> 8));
	body13.push_back(0x03); // deathAnimStateId (the victim's +0x2C0 slot)
	body13.push_back(0x00);
	const std::vector<uint8_t> dg = frame_server_session(
			server_tx, {make_protocol_message(0x13, body13)});
	client.receive(dg.data(), dg.size());
	(void)client.Client_ProcessNetworkFrame(2);

	const ns::ClientEntityState *after = row_for(client.state(), kBarrel);
	if (!expect(after != nullptr && after->health_known && after->health_word == 0,
			"0x13: the fold zeroes the row's health (retail Health = 0)"))
		return false;
	const auto deaths = client.view().drain_effect_commands();
	if (!expect(deaths.size() == 1 && std::get<ns::EntityDeathEvent>(deaths[0]).entity_handle == kBarrel &&
				std::get<ns::EntityDeathEvent>(deaths[0]).death_anim_state_id == 3,
			"0x13: exactly one death record reaches the sim drain"))
		return false;
	if (!expect(client.view().drain_effect_commands().empty(),
			"0x13: the drain is consume-once"))
		return false;

	// The SECOND death route: S2C 0x26 kill-sync (the destructible callback's
	// own authority resend) folds through the same surface.
	// [orig: NapiNPClientMsg_0x026 @0x42EC30 → Entity_KillBySlotId @0x42BCE0]
	std::vector<uint8_t> body26;
	body26.push_back(static_cast<uint8_t>(kBarrel & 0xFFu));
	body26.push_back(static_cast<uint8_t>(kBarrel >> 8));
	body26.push_back(0xFF); // section -1

	body26.push_back(0xFF);
	const std::vector<uint8_t> dg26 = frame_server_session(
			server_tx, {make_protocol_message(0x26, body26)});
	client.receive(dg26.data(), dg26.size());
	(void)client.Client_ProcessNetworkFrame(3);
	const auto kills = client.view().drain_effect_commands();
	return expect(kills.size() == 1 && std::get<ns::EntityDeathEvent>(kills[0]).entity_handle == kBarrel &&
				std::get<ns::EntityDeathEvent>(kills[0]).item_state && std::get<ns::EntityDeathEvent>(kills[0]).hit_section == -1 &&
                std::get<ns::EntityDeathEvent>(kills[0]).death_anim_state_id == 0,
			"0x26: the signed hit section reaches the native class-state route");
}

// S2C 0x46 bit15: bookkeeping only. The entity row must SURVIVE.
bool run_player_sync_removal_keeps_the_entity() {
	constexpr uint16_t kSelf = 0x0005;
	constexpr uint16_t kPeer = 0x0004;
	inmatch::ClientRuntime client("RemovalJoiner", [] { return uint64_t(0); });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	{
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx, {make_protocol_message(0x0C, organic_spawn(kPeer, "Peer", 1))});
		client.receive(dg.data(), dg.size());
		(void)client.Client_ProcessNetworkFrame(1);
	}
	if (!expect(row_for(client.state(), kPeer) != nullptr,
			"0x46-bit15: the peer's row exists before the removal"))
		return false;
	if (!expect(client.drain_cleared_player_slots().empty(),
			"0x46-bit15: no roster clear recorded before the removal"))
		return false;

	const std::vector<uint8_t> dg = frame_server_session(
			server_tx,
			{make_protocol_message(0x46, encode_player_sync_removal(4, /*with_ack=*/false))});
	client.receive(dg.data(), dg.size());
	(void)client.Client_ProcessNetworkFrame(2);

	if (!expect(client.drain_cleared_player_slots() == std::vector<uint8_t>{4},
			"0x46-bit15: the roster slot's bookkeeping clear is surfaced"))
		return false;
	// The witnessed entity-field wipes @0x431437 are dead code on this leg.
	return expect(row_for(client.state(), kPeer) != nullptr,
			"0x46-bit15: the ENTITY row survives (only 0x5D destroys)");
}

inmatch::NapiNPConnection make_in_match_conn(uint32_t id, int type, ns::ISessionTransport *t,
		ns::TransportMode mode, w::EntityHandle owned) {
	inmatch::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = true;
	c.spawned_announced = true;
	c.phase = inmatch::ConnectionPhase::InMatch;
	c.admission_stage = inmatch::GameAdmissionStage::Complete;
	return c;
}

// The host leg: C2S 0x32 -> ONE S2C 0x5D listing exactly the empty pool-0 indices, to
// the requester only, and nothing at all once the round is over.
// C2S 0x0F is answered only for a requester whose player the host has
// added: before Server_PlayerAdd binds the session player's slot the query
// is dropped; after it the host replies S2C 0x18 to the requester.
// [orig: NapiNPServerMsg_HandlePlayerInfoRequest @0x514191..0x5141A8 (the
//  conn+0x160 -> +0xC0 tests); Server_PlayerAdd @0x51CD51 (the +0xC0 bind)]
bool run_host_answers_entity_query_only_after_player_add() {
	w::World world;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(1, 16);
	w::PlayerSpawn spawn;
	spawn.position = {0, 0, 0};
	const w::EntityHandle a = w::spawn_player(world, spawn);
	if (!expect(a.valid(), "entity-query: a pool-0 player")) return false;

	ns::UdpSessionTransport udp_requester(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_in_match_conn(3, 1, &udp_requester, ns::TransportMode::Client,
			w::EntityHandle{}));
	roster[0].phase = inmatch::ConnectionPhase::PendingSpawn;
	roster[0].burst.spawned = false;
	roster[0].spawned_announced = false;
	const std::vector<ProtocolMessage> request{make_protocol_message(0x0F,
			{static_cast<uint8_t>(a.packed & 0xFF), static_cast<uint8_t>(a.packed >> 8)})};
	const auto answers = [&](uint32_t tick) {
		int n = 0;
		for (const ProtocolMessage &m : inmatch::dispatch_session_replies(
					inmatch::GameConfig{}, roster[0], request, tick, roster, &world))
			if (m.tag == 0x18) ++n;
		return n;
	};
	if (!expect(answers(100) == 0,
			"entity-query: a requester not yet added gets no 0x18"))
		return false;
	roster[0].phase = inmatch::ConnectionPhase::PlayerAdded;
	return expect(answers(101) == 1,
			"entity-query: an added player's query is answered with one 0x18");
}

bool run_host_answers_the_sweep_request() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem &ai = world.ai;

	w::PlayerSpawn spawn;
	spawn.position = {0, 0, 0};
	const w::EntityHandle a = w::spawn_player(world, spawn);
	const w::EntityHandle b = w::spawn_remote_player(world, spawn);
	const w::EntityHandle c = w::spawn_remote_player(world, spawn);
	if (!expect(a.valid() && b.valid() && c.valid(), "sweep-host: three pool-0 players"))
		return false;

	ns::LoopbackChannel loop;
	ns::UdpSessionTransport udp_requester(ns::UdpSessionTransport::Role::Host);
	inmatch::NapiNPServerCtx ctx;
	ctx.world = &world;
	ctx.is_authority = 1;
	ctx.is_in_session = 1;
	auto &roster = ctx.np_protocol.connection_list;
	roster.push_back(make_in_match_conn(1, 2, &loop, ns::TransportMode::Loopback, a));
	roster.push_back(
			make_in_match_conn(3, 1, &udp_requester, ns::TransportMode::Client, b));

	const std::vector<ProtocolMessage> request{make_protocol_message(0x32, {})};

	// Prior state: pool 0 is fully occupied below the high-water mark, so the sweep
	// is EMPTY. (An empty 0x5D is still the witnessed answer.)
	{
		const std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(
				inmatch::GameConfig{}, roster[1], request, 100, roster, &world);
		int sweeps = 0;
		for (const ProtocolMessage &m : replies) {
			if (m.tag != 0x5D) continue;
			++sweeps;
			DestroyEntityList decoded;
			if (!expect(decode_destroy_entity_list(
						m.payload.data(), m.payload.size(), decoded),
					"sweep-host: the reply body decodes"))
				return false;
			if (!expect(decoded.pool0_indices.empty(),
					"sweep-host: a fully occupied pool 0 sweeps nothing"))
				return false;
		}
		if (!expect(sweeps == 1, "sweep-host: exactly one 0x5D reply")) return false;
	}

	// Free the MIDDLE slot: only that index may be listed (never the trailing
	// unused capacity above the high-water mark).
	world.registry.despawn(b);
	{
		const std::vector<ProtocolMessage> replies = inmatch::dispatch_session_replies(
				inmatch::GameConfig{}, roster[1], request, 101, roster, &world);
		std::vector<uint16_t> listed;
		for (const ProtocolMessage &m : replies) {
			if (m.tag != 0x5D) continue;
			DestroyEntityList decoded;
			if (!expect(decode_destroy_entity_list(
						m.payload.data(), m.payload.size(), decoded),
					"sweep-host: the reply body decodes after the despawn"))
				return false;
			listed = decoded.pool0_indices;
		}
		if (!expect(listed == std::vector<uint16_t>{static_cast<uint16_t>(b.slot())},
				"sweep-host: the reply lists exactly the freed pool-0 index"))
			return false;
	}

	// REQUESTER ONLY: the reply rides the direct reply list, so no other in-match
	// connection's transport was staged. [orig: send_mask 32, target = requester slot]
	{
		ns::Datagram staged;
		if (!expect(!loop.client_recv(staged),
				"sweep-host: the host loopback peer receives no sweep"))
			return false;
		std::vector<uint8_t> raw;
		if (!expect(!udp_requester.pop_outbound(raw),
				"sweep-host: the sweep is a direct reply, never a broadcast"))
			return false;
	}

	// Round over -> the handler is skipped entirely.
	world.process_round_end(0);
	const std::vector<ProtocolMessage> after_round = inmatch::dispatch_session_replies(
			inmatch::GameConfig{}, roster[1], request, 102, roster, &world);
	for (const ProtocolMessage &m : after_round)
		if (!expect(m.tag != 0x5D, "sweep-host: no sweep once the round has ended"))
			return false;
	return true;
}

// ---------------------------------------------------------------------------------
// (C) in-match session loss
// ---------------------------------------------------------------------------------

bool run_in_match_session_loss() {
	uint64_t now_ms = 1000;
	inmatch::ClientRuntime client("TimeoutJoiner", [&now_ms] { return now_ms; });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0005, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();

	if (!expect(!client.session_lost() && client.session_loss_reason().empty(),
			"loss: a fresh in-match session is healthy"))
		return false;

	// One millisecond short of the witnessed reap window: still healthy.
	now_ms += 119999;
	(void)client.Client_ProcessNetworkFrame(1);
	if (!expect(!client.session_lost(),
			"loss: 119999 ms of silence is not yet a loss"))
		return false;

	// A datagram mid-way RESETS the clock.
	{
		const std::vector<uint8_t> dg = frame_server_session(
				server_tx, {make_protocol_message(0x03, {0, 0, 0, 0})});
		client.receive(dg.data(), dg.size());
		(void)client.Client_ProcessNetworkFrame(2);
	}
	now_ms += 119999;
	(void)client.Client_ProcessNetworkFrame(3);
	if (!expect(!client.session_lost(),
			"loss: inbound traffic re-armed the reap window"))
		return false;

	// Retail compares elapsed strictly greater-than the configured timeout.
	now_ms += 1;
	(void)client.Client_ProcessNetworkFrame(4);
	if (!expect(!client.session_lost(),
			"loss: exactly 120000 ms of silence remains inside the reap window"))
		return false;
	now_ms += 1;
	const std::vector<std::vector<uint8_t>> timeout_frame =
			client.Client_ProcessNetworkFrame(5);
	if (!expect(timeout_frame.empty() && client.session_lost(),
			"loss: 120001 ms of silence trips the reap without new traffic"))
		return false;
	if (!expect(client.phase() == inmatch::JoinerConnection::Phase::Error &&
				!client.in_match(),
			"loss: the silence reap enters the terminal connection phase"))
		return false;
	ClientFiredRound fire;
	fire.shooter_handle = 0x0005;
	if (!expect(!client.queue_fired_round(fire),
			"loss: a timed-out runtime rejects newly queued gameplay"))
		return false;
	// Stay beyond a complete 0x34 interval: terminal silence must not be a
	// one-frame queue clear followed by a freshly produced keepalive.
	for (uint32_t tick = 6; tick < 29770; ++tick) {
		if (!expect(client.Client_ProcessNetworkFrame(tick).empty(),
				"loss: the timed-out runtime remains silent indefinitely"))
			return false;
	}
	return expect(!client.session_loss_reason().empty(),
			"loss: a lost session reports a reason for the shell to surface");
}

// Death temporarily returns an already-established joiner to Phase::Driving
// while the deploy screen owns the next pick/release exchange. That phase reuse
// must not disable the transport's 120-second silence reap: a host crash while
// the player is dead still exits the session instead of stranding the screen.
bool run_redeployment_session_loss() {
	uint64_t now_ms = 1000;
	inmatch::JoinerConnection joiner(
			"RedeployTimeoutJoiner", [&now_ms] { return now_ms; });
	joiner.seed_in_match(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0005, w::kPlayerInfantryTypeId);
	if (!expect(joiner.begin_redeployment() &&
	                    joiner.phase() == inmatch::JoinerConnection::Phase::Driving,
			"loss: death enters the established redeployment drive"))
		return false;

	now_ms += inmatch::JO_GAME_SESSION_TIMEOUT_MS;
	if (!expect(!joiner.session_lost(),
			"loss: redeployment remains healthy exactly at the timeout boundary"))
		return false;
	++now_ms;
	(void)joiner.pump(0); // the send pump runs the reap (PumpStateMachine case 5)
	if (!expect(joiner.session_lost() &&
	                      !joiner.session_loss_reason().empty(),
			"loss: host silence reaps an established joiner after the redeploy timeout"))
		return false;
	const uint64_t terminal_silence = joiner.milliseconds_since_last_receive();
	if (!expect(joiner.frame_inner(0x34, {0, 0, 0, 0}).empty() &&
				joiner.phase() == inmatch::JoinerConnection::Phase::Error,
			"loss: direct session framing latches timeout before producing traffic"))
		return false;
	if (!expect(joiner.pump(0).empty(),
			"loss: the direct joiner pump remains silent after the timeout"))
		return false;

	SessionSequencing late_server_tx = inmatch::make_jo_game_session_sequencing();
	const std::vector<uint8_t> late = frame_server_session(
			late_server_tx, {make_protocol_message(0x03, {0, 0, 0, 0})});
	const inmatch::JoinerConnection::PollResult late_result =
			joiner.handle_datagram(late.data(), late.size());
	return expect(late_result.outbound.empty() && joiner.session_lost() &&
				joiner.phase() == inmatch::JoinerConnection::Phase::Error &&
				joiner.milliseconds_since_last_receive() == terminal_silence,
			"loss: a late valid datagram cannot refresh or revive a timed-out joiner");
}

// The client reap is PumpStateMachine's case 5, which runs only inside the
// send pump (PumpFlags 0x40), and the in-match frame calls that pump only when
// the holdoff countdown is out. Under a NovaWorld host's 12-tick holdoff a
// silence past the window is therefore reaped at the next open boundary, and a
// datagram that lands before that boundary refreshes the clock and saves the
// connection.
// [orig: CNapiNPConnection_PumpStateMachine case 5 @0x6295a2..0x62961c ahead of
//  PumpSendIntervals @0x629628; CNapiNPConnection_PumpFlags 0x40 @0x6297c8;
//  PumpClientProtocolSend (flags 738) behind `cmp [conn+648h],0` @0x42c3dd]
bool run_in_match_reap_waits_for_the_send_boundary() {
	const ProtocolMessage holdoff_12 = make_protocol_message(
			0x00, {0x01, 0x08, 0x00, 0x00, 0x00, 12, 0x00, 0x00, 0x00}, 0xA0);
	for (const bool rescued : {false, true}) {
		uint64_t now_ms = 1000;
		inmatch::ClientRuntime client("GatedReapJoiner", [&now_ms] { return now_ms; });
		client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                    1, 0, 0x0005, w::kPlayerInfantryTypeId);
		SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
		const std::vector<uint8_t> dictation = frame_server_session(server_tx, {holdoff_12});
		client.receive(dictation.data(), dictation.size());
		(void)client.Client_ProcessNetworkFrame(1);
		if (!expect(client.send_holdoff_countdown() == 12,
				"gated reap: the open boundary arms the dictated twelve-tick period"))
			return false;

		now_ms += inmatch::JO_GAME_SESSION_TIMEOUT_MS + 1;
		uint32_t tick = 2;
		for (; tick <= 12; ++tick) {
			if (rescued && tick == 6) {
				const std::vector<uint8_t> dg = frame_server_session(
						server_tx, {make_protocol_message(0x03, {0, 0, 0, 0})});
				client.receive(dg.data(), dg.size());
			}
			(void)client.Client_ProcessNetworkFrame(tick);
			if (!expect(!client.session_lost(),
					"gated reap: a held frame past the window does not reap"))
				return false;
		}
		(void)client.Client_ProcessNetworkFrame(tick);
		if (!expect(client.session_lost() == !rescued,
				rescued ? "gated reap: a datagram before the boundary saves the connection"
				        : "gated reap: the next open boundary reaps the silent connection"))
			return false;
	}
	return true;
}

// A HOST-role runtime has no session to lose (there is no peer reaping it).
bool run_host_client_never_reports_loss() {
	ns::LoopbackChannel loop;
	inmatch::ClientRuntime host_view(loop);
	(void)host_view.Client_ProcessNetworkFrame(1);
	return expect(!host_view.session_lost() && host_view.session_loss_reason().empty(),
			"loss: the host-as-client role never reports a session loss");
}

} // namespace

int main() {
	if (!run_team_assign_relatches_self_and_resubmits()) return 1;
	if (!run_team_assign_folds_remote_row()) return 1;
	if (!run_team_assign_reselects_the_new_sides_profile()) return 1;
	if (!run_team_assign_default_kit_stays_byte_identical()) return 1;
	if (!run_empty_slot_sweep_surfaces_raw_gameplay()) return 1;
	if (!run_deployed_item_lifecycle_surfaces_raw_gameplay()) return 1;
	if (!run_deployed_item_lifecycle_folds_into_the_replica()) return 1;
	if (!run_empty_slot_sweep_retires_the_row()) return 1;
	if (!run_entity_death_notify_reaches_the_sim()) return 1;
	if (!run_player_sync_removal_keeps_the_entity()) return 1;
	if (!run_host_answers_the_sweep_request()) return 1;
	if (!run_host_answers_entity_query_only_after_player_add()) return 1;
	if (!run_in_match_session_loss()) return 1;
	if (!run_redeployment_session_loss()) return 1;
	if (!run_in_match_reap_waits_for_the_send_boundary()) return 1;
	if (!run_host_client_never_reports_loss()) return 1;
	std::printf("OK\n");
	return 0;
}
