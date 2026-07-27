// Three witnessed net legs that had no coverage:
//
//  (A) S2C 0x50 TEAM ASSIGN — the SECOND writer of the byte_A85B48 team latch (the
//      S2C 0x04 tail is the first). Self -> re-latch + ONE C2S 0x2F re-submission at
//      slot 195 RAW; any valid handle -> the decoded row's team. The re-submission is a
//      PER-SIDE PROFILE RESELECT: the NEW team byte picks the profile side block, that
//      block's class byte is the wire class, and the page that class indexes is the kit.
//      [orig: NapiNPClientMsg_0x050 @0x431910 — latch @0x4319db, entity team @0x4319ee,
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

#include <npruntime/client_runtime.h>
#include <npruntime/joiner_connection.h>
#include <npruntime/napi_np_connection.h>
#include <npruntime/napi_np_server_ctx.h>
#include <npruntime/server_message_dispatch.h>

#include <netsim/connection.h>
#include <netsim/loopback_channel.h>
#include <netsim/session_transport.h>
#include <netsim/udp_session_transport.h>

#include <npwire/ingame_decode.h>
#include <npwire/ingame_encode.h>
#include <npwire/nw_session_framing.h>
#include <npwire/protocol_message.h>
#include <npwire/session_keys.h>

#include <world/ai.h>
#include <world/player_spawn.h>
#include <world/world.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace np = opennova::np;
namespace ns = opennova::netsim;
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
	rec.has_body = true;
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
	np::JoinerConnection joiner("TeamJoiner", [] { return uint64_t(0); });
	np::JoinerConnection::LoadoutKit kit;
	kit.player_class = 6;
	kit.equipped_combo = 212; // deliberately NOT 195: the re-submit must ignore it
	kit.rows.push_back(LoadoutSubmitEntry{0x18, 0x03, 0xFF, 0xFF});
	joiner.set_loadout_kit(kit);
	joiner.seed_in_match(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                     1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();

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
	const std::vector<uint8_t> dg = frame_server_session(
			server_tx, {make_protocol_message(0x50, encode_team_assign(assign))});
	const np::JoinerConnection::PollResult poll =
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
	// [orig: @0x431a9e passes 195 RAW, not the live g_currentWeaponSlot (212 here)]
	if (!expect(submitted.weapon_slot_index == 195,
			"0x50-self: the re-submission uses slot 195 raw, not the equipped combo"))
		return false;
	return expect(submitted.entries.size() == 1 && submitted.entries[0].adm_index == 0x18,
			"0x50-self: the re-submission carries the applied kit rows");
}

// Pull the single C2S 0x2F this poll queued. Zero or more than one fails loudly: retail
// emits EXACTLY one re-submission per self team assign [orig: @0x431a9e].
bool one_loadout_submit(const np::JoinerConnection::PollResult &poll,
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
	np::JoinerConnection joiner("SideJoiner", [] { return uint64_t(0); });
	np::JoinerConnection::LoadoutKit kit;
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
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();

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
		const np::JoinerConnection::PollResult poll =
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
		const np::JoinerConnection::PollResult poll =
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
	np::JoinerConnection joiner("DefaultKitJoiner", [] { return uint64_t(0); });
	// deliberately NO set_loadout_kit
	joiner.seed_in_match(kSessionId, kClientKey, kClientScrk, kServerScrk,
						 1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
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
	const np::JoinerConnection::PollResult poll =
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
	np::ClientRuntime client("FoldJoiner", [] { return uint64_t(0); });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();

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
bool run_empty_slot_sweep_retires_the_row() {
	constexpr uint16_t kSelf = 0x0005;
	constexpr uint16_t kGhost = 0x0003;
	np::ClientRuntime client("SweepJoiner", [] { return uint64_t(0); });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();

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
				4u + 12u + 6u + 1u + 4u * kWorldStateScoreCount + 4u, 0);
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

// S2C 0x46 bit15: bookkeeping only. The entity row must SURVIVE.
bool run_player_sync_removal_keeps_the_entity() {
	constexpr uint16_t kSelf = 0x0005;
	constexpr uint16_t kPeer = 0x0004;
	np::ClientRuntime client("RemovalJoiner", [] { return uint64_t(0); });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, kSelf, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();
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

np::NapiNPConnection make_in_match_conn(uint32_t id, int type, ns::ISessionTransport *t,
		ns::TransportMode mode, w::EntityHandle owned) {
	np::NapiNPConnection c;
	c.connection_id = id;
	c.type = type;
	c.link.transport = t;
	c.link.mode = mode;
	c.link.owned_entity = owned;
	c.burst.spawned = true;
	c.spawned_announced = true;
	c.phase = np::ConnectionPhase::InMatch;
	c.admission_stage = np::GameAdmissionStage::Complete;
	return c;
}

// The host leg: C2S 0x32 -> ONE S2C 0x5D listing exactly the empty pool-0 indices, to
// the requester only, and nothing at all once the round is over.
bool run_host_answers_the_sweep_request() {
	w::World world;
	world.registry.configure_pool(0, 16);
	w::AiSystem ai;
	world.ai = &ai;

	w::PlayerSpawn spawn;
	spawn.position = {0, 0, 0};
	const w::EntityHandle a = w::spawn_player(world, spawn);
	const w::EntityHandle b = w::spawn_remote_player(world, spawn);
	const w::EntityHandle c = w::spawn_remote_player(world, spawn);
	if (!expect(a.valid() && b.valid() && c.valid(), "sweep-host: three pool-0 players"))
		return false;

	ns::LoopbackChannel loop;
	ns::UdpSessionTransport udp_requester(ns::UdpSessionTransport::Role::Host);
	np::NapiNPServerCtx ctx;
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
		const std::vector<ProtocolMessage> replies = np::dispatch_session_replies(
				np::GameConfig{}, roster[1], request, 100, roster, &world);
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
		const std::vector<ProtocolMessage> replies = np::dispatch_session_replies(
				np::GameConfig{}, roster[1], request, 101, roster, &world);
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
	world.round_end.ended = true;
	const std::vector<ProtocolMessage> after_round = np::dispatch_session_replies(
			np::GameConfig{}, roster[1], request, 102, roster, &world);
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
	np::ClientRuntime client("TimeoutJoiner", [&now_ms] { return now_ms; });
	client.seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                    1, 0, 0x0005, w::kPlayerInfantryTypeId);
	SessionSequencing server_tx = np::make_jo_game_session_sequencing();

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

	// Crossing it: lost, with a player-facing reason.
	now_ms += 1;
	(void)client.Client_ProcessNetworkFrame(4);
	if (!expect(client.session_lost(),
			"loss: 120000 ms of silence trips the witnessed reap window"))
		return false;
	return expect(!client.session_loss_reason().empty(),
			"loss: a lost session reports a reason for the shell to surface");
}

// A HOST-role runtime has no session to lose (there is no peer reaping it).
bool run_host_client_never_reports_loss() {
	ns::LoopbackChannel loop;
	np::ClientRuntime host_view(loop);
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
	if (!run_empty_slot_sweep_retires_the_row()) return 1;
	if (!run_player_sync_removal_keeps_the_entity()) return 1;
	if (!run_host_answers_the_sweep_request()) return 1;
	if (!run_in_match_session_loss()) return 1;
	if (!run_host_client_never_reports_loss()) return 1;
	std::printf("OK\n");
	return 0;
}
