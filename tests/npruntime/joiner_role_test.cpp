// The joiner role's frame (ADR 0043 d3, slice E8b; ex the S10a joiner world
// bridge). The frame's phase SEQUENCE is the witnessed retail client frame
// [orig: Game_ProcessMainFrame @0x5263f0; Client_ProcessNetworkFrame
// @0x42c180] — this pins the portable contract through its effects: the
// ClientHello one-shot over the socket seam, the per-frame clock, the in-match
// spawn edge for L (H latched, the join-wait latches cleared, the weapon pump
// gated open the same frame), the mid-frame loadout-grant stamp, and the
// per-session latch reset. Confirmed vehicle occupancy, prediction, mount
// requests and the matching seat overlays run through the production frame.

#include <net/npwire/ingame_decode.h>
#include <base/gameprofile/gameprofile.h>
#include <net/title/title_identity.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/nw_session_framing.h>
#include <net/npwire/session_hello.h>
#include <net/npwire/session_keys.h>
#include <runtime/inmatch/charattr_table.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/client_replica_present.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/mission_exit.h>
#include <runtime/replication/connection_fan.h>
#include <formats/def/def.h>
#include <formats/mission/mission.h>
#include <runtime/mission/item_traits.h>
#include <runtime/mission/mission_kernel.h>

#include <runtime/world/vehicle_motor.h>
#include <runtime/world/collision.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/vehicle_panel_feed.h>
#include <runtime/world/angle.h>
#include <runtime/world/mount_controls.h> // emplaced_gun_frame_heading
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/infantry.h> // infantry_anim_flags (the crouched state)
#include <runtime/world/player_spawn.h>
#include <runtime/world/player_loadout.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/throwables.h>
#include <runtime/world/weapon_inventory.h>
#include <runtime/world/wire_person_hit.h> // WirePersonBody (the person pass's row view)
#include <net/npwire/entity_class.h> // class_from_tag (the row's class)
#include <runtime/world/world.h>

#include <base/io/bam.h>

#include "../common/hull_brain.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// JO's title identity (net/title/title_identity.h), the identity these tests speak.
static const opennova::TitleIdentity &kJo = *opennova::title_identity(opennova::gameprofile::GAME_JO);

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;

namespace {
// The USE/label seat scan gates on an aim cone from the eye (standing
// 0x3FFFFFC0, seated 5 deg) [orig: Entity_FindNearestSeatOrArmory
// @0x43608f..0x436123], so a rig must face the seat point it expects to
// be offered, from a step away (the co-located case is atan2(0, 0)).
void look_at(w::World &world, w::EntityHandle player_h, float tx, float ty, float tz) {
	w::Entity *player = world.registry.get(player_h);
	if (player == nullptr) return;
	const double dx = double(tx) - player->position.x - player->eye_offset_x / 65536.0;
	const double dy = double(ty) - player->position.y - player->eye_offset_y / 65536.0;
	const double dz = double(tz) + 0.1875 - player->position.z - player->eye_offset_z / 65536.0;
	const double heading = std::atan2(dy, dx);
	const double pitch = std::atan2(dz, std::hypot(dx, dy));
	player->heading = w::placed_heading_bam(int16_t(std::lround(90.0 - heading * 180.0 / 3.14159265358979323846)));
	player->pitch_bam = int32_t(pitch * opennova::io::kBamPerRadian);
	if (w::AiEntity *body = world.ai.for_handle(player_h)) {
		body->heading = int32_t(heading * opennova::io::kBamPerRadian);
		body->pitch = int32_t(pitch * opennova::io::kBamPerRadian);
	}
}
} // namespace

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

const std::string kClientScrk = "CLIENT-BRIDGE-SCRK";
const std::string kServerScrk = "SERVER-BRIDGE-SCRK";
constexpr uint32_t kClientKey = 0x0000BEEFu;
constexpr uint32_t kSessionId = 0x0FE0E112u;

// The socket seam: counts what the role ships, receives nothing.
class CountingSocket final : public opennova::IDatagramSocket {
public:
	int sends = 0;
	std::vector<std::vector<uint8_t>> datagrams;
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *data, std::size_t size) override {
		++sends;
		datagrams.emplace_back(data, data + size);
	}
	bool last_message(uint8_t tag, ProtocolMessage &out) const {
		for (auto it = datagrams.rbegin(); it != datagrams.rend(); ++it) {
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (!nw_decode_inbound(it->data(), it->size(), opcode, body) ||
					opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
					!decode_protocol_packet_plaintext(body.data(), body.size(),
							kClientScrk, header, messages)) continue;
			for (auto msg = messages.rbegin(); msg != messages.rend(); ++msg) {
				if (msg->tag == tag) { out = *msg; return true; }
			}
		}
		return false;
	}
};

// The kernel lives on the heap: a World-carrying frame is megabytes.
struct Harness {
	std::unique_ptr<mission::MissionKernel> kernel = std::make_unique<mission::MissionKernel>();
	inmatch::JoinerRole role;
	CountingSocket socket;
	inmatch::TickInput input;
	std::vector<std::string> seams;

	Harness() {
		kernel->world.registry.configure_pool(0, 16);
		role.bind(*kernel);
		role.set_socket(&socket, PeerAddr{});
		role.set_title(&kJo);
		role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "", "");
		role.kit_seams.apply_authoritative = [this] { seams.push_back("loadout"); };
		role.kit_seams.reseed_on_side_change = [this] {
			seams.push_back("reseed");
			return false;
		};
		role.kit_seams.push = [this] { seams.push_back("push_kit"); };
	}
	w::LocalPlayerWeapon &weapon() { return kernel->local.weapon; }
	w::WeaponInventory &inventory() { return kernel->local.inventory; }
};

// A pre-match frame ships the one-shot ClientHello, runs the loadout seams in
// the witnessed order, advances the clock, and never spawns L.
bool run_pre_match_frame() {
	Harness h;
	h.role.run_tick(h.input);
	const std::vector<std::string> expected = {"loadout", "reseed"};
	if (!expect(h.seams == expected, "pre-match frame: the loadout seams in order")) {
		for (const std::string &c : h.seams) std::fprintf(stderr, "  %s\n", c.c_str());
		return false;
	}
	if (!expect(h.socket.sends == 1, "pre-match frame: the ClientHello shipped once")) return false;
	if (!expect(h.role.started(), "pre-match frame: the hello latch armed")) return false;
	if (!expect(!h.role.local_spawned(), "pre-match frame: no L")) return false;
	if (!expect(!h.kernel->world.cached.local_player.valid(), "pre-match frame: no local player")) return false;
	if (!expect(h.role.now_tick() == 1, "pre-match frame: the clock advanced")) return false;
	if (!expect(h.role.take_diagnostic_sample(), "pre-match frame: the tripwire sampled on tick 0")) return false;

	// Frame 2: no second hello, no tripwire sample (1 % 62 != 0), still no L.
	h.seams.clear();
	h.role.run_tick(h.input);
	if (!expect(h.seams == expected, "frame 2: the seams again")) return false;
	if (!expect(h.socket.sends == 1, "frame 2: hello is a one-shot")) return false;
	if (!expect(!h.role.take_diagnostic_sample(), "frame 2: no tripwire sample")) return false;
	return expect(h.role.now_tick() == 2, "frame 2: the clock advanced again");
}

// The preload frame (no world to tick) shares the hello latch and the clock.
bool run_preload_frame() {
	Harness h;
	h.role.poll_preload();
	if (!expect(h.socket.sends == 1 && h.role.started(), "preload: the hello shipped once")) return false;
	if (!expect(h.role.now_tick() == 1, "preload: the clock advanced")) return false;
	h.role.poll_preload();
	if (!expect(h.socket.sends == 1, "preload: hello is a one-shot")) return false;
	return expect(h.role.now_tick() == 2, "preload: the clock advanced again");
}

// A seeded in-match runtime spawns L on the first frame (the in-match edge),
// latches H, clears the join-wait latches, and seeds the look heading.
bool run_in_match_spawn_edge() {
	Harness h;
	// Production order: the pre-load preload frame ships the ClientHello (the
	// latch arms there), the session establishes later. runtime.start()
	// RESETS a session, so the latch must already be armed when the in-match
	// frame runs — exactly what poll_preload guarantees.
	h.role.poll_preload();
	if (!expect(h.role.started() && h.socket.sends == 1,
			"in-match: the preload hello armed the latch")) return false;
	// An Attack & Defend session (g_GameType 0x10002) whose one TARGET-attrib
	// item belongs to team 2, against L's seeded team 0.
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId,
	                             /*game_type=*/0x10002u);
	if (!expect(h.role.runtime->in_match(), "in-match: seeded runtime is InMatch")) return false;
	h.kernel->world.registry.configure_pool(1, 4);
	w::Entity target;
	target.has_item_def = true;
	target.item_attrib = 0x8000u;
	target.team = 2;
	h.kernel->world.registry.spawn(1, target);
	h.weapon().fire_held = true;
	h.weapon().fire_pressed = true;
	h.weapon().reload_pressed = true;

	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "in-match frame: L spawned on the edge")) return false;
	if (!expect(h.socket.sends == 1, "in-match frame: no second hello on the seeded session")) return false;
	if (!expect(h.kernel->world.cached.local_player.valid(),
			"in-match frame: cached.local_player published")) return false;
	if (!expect(h.role.self_wire_handle() == 0x0005,
			"in-match frame: H latched from the runtime")) return false;
	// L is client-built: commandGroup 0, not the authority's 1
	// [orig: NapiNPClientMsg_0x00C @0x42E7F1].
	if (const w::Entity *body = h.kernel->world.registry.get(
				h.kernel->world.cached.local_player)) {
		if (!expect(body->group_id == 0, "in-match frame: L's commandGroup is 0"))
			return false;
	}
	// The join-wait fire latches died at the spawn edge (the weapon pump ran
	// gated open the same frame over the cleared latches).
	if (!expect(!h.weapon().fire_held && !h.weapon().fire_pressed &&
					!h.weapon().reload_pressed,
			"in-match frame: join-wait weapon latches cleared")) return false;
	// The mission start's A&D latch ran with no L; the spawn edge latches
	// against L's team: another team's target attacks (2) [orig: Game_LatchAttackDefendRole,
	// the joiner's call @0x5260C1 after Player_InitPlayer @0x525BBC].
	if (!expect(h.kernel->local.attack_defend_role == 2,
			"in-match frame: the A&D role latched at the spawn edge")) return false;

	// Frame 2: the edge is a one-shot.
	const w::EntityHandle L = h.kernel->world.cached.local_player;
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "frame 2: still spawned")) return false;
	return expect(h.kernel->world.cached.local_player == L, "frame 2: no re-spawn");
}

// The spawn edge's A&D latch reads L's team from its self record: a team-2
// L against a team-2 target defends (1); the same target against the seeded
// team-0 L of run_in_match_spawn_edge attacks (2) [orig: Game_LatchAttackDefendRole --
// `cmp al, [edx+162h]; setnz; add ecx, 1` @0x5241A3..0x5241AC].
bool run_spawn_edge_latch_reads_self_team() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId,
	                             /*game_type=*/0x10002u, /*tick_seed=*/0, /*replay_mode=*/true,
	                             /*self_team=*/2);
	h.kernel->world.registry.configure_pool(1, 4);
	w::Entity target;
	target.has_item_def = true;
	target.item_attrib = 0x8000u;
	target.team = 2;
	h.kernel->world.registry.spawn(1, target);
	h.role.run_tick(h.input);
	const w::Entity *L = h.kernel->local.player();
	if (!expect(h.role.local_spawned() && L != nullptr && L->team == 2,
			"own team: L spawned with its self record's team")) return false;
	return expect(h.kernel->local.attack_defend_role == 1,
			"own team: the latch defends against its own team's target");
}

// A TARGET whose team an S2C 0x50 moves in the spawn frame: the latch reads
// the team the frame's mirror applied, as retail's mission start latches after
// its wait loop pumped every message before it [orig: Game_StartMission --
// NapiClient_WaitForGameStart (the call @0x524E29; its receive pump @0x42CC88),
// then the Game_LatchAttackDefendRole call @0x5260C1; NapiNPClientMsg_TeamAssign's store
// @0x4319EE].
bool run_spawn_edge_latch_reads_the_frames_target_team() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId,
	                             /*game_type=*/0x10002u);
	h.kernel->world.registry.configure_pool(1, 4);
	w::Entity target;
	target.has_item_def = true;
	target.item_attrib = 0x8000u;
	target.team = 2;
	target.spawn_origin = 0; // a placed row the mirror follows
	const w::EntityHandle target_h = h.kernel->world.registry.spawn(1, target);
	TeamAssign assign;
	assign.entity_handle = target_h.packed;
	assign.team = 0; // L's own seeded team
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::TEAM_ASSIGN, encode_team_assign(assign))}, packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input); // the spawn frame folds the 0x50 too
	const w::Entity *t = h.kernel->world.registry.get(target_h);
	if (!expect(h.role.local_spawned() && t != nullptr && t->team == 0,
			"frame team: L spawned and the frame's mirror moved the target to team 0"))
		return false;
	return expect(h.kernel->local.attack_defend_role == 1,
			"frame team: the latch reads the target's team after the frame's 0x50");
}

// A self S2C 0x50 folded before the spawn edge: retail's handler stores the team
// on the joiner's own dcb whether or not its mission start has made it the local
// entity, so Player_InitPlayer and the A&D latch read the new team; L spawns on
// it (seeded team 0, moved to 2) and defends a team-2 target (1, where the seeded
// team attacks it, 2) [orig: NapiNPClientMsg_TeamAssign @0x431910 - the Team
// store @0x4319EE; Player_InitPlayer @0x4E15F0 - @0x4E1809; Game_StartMission -
// the Game_LatchAttackDefendRole call @0x5260C1].
bool run_spawn_edge_takes_a_self_team_assign() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId,
	                             /*game_type=*/0x10002u);
	h.kernel->world.registry.configure_pool(1, 4);
	w::Entity target;
	target.has_item_def = true;
	target.item_attrib = 0x8000u;
	target.team = 2;
	h.kernel->world.registry.spawn(1, target);
	TeamAssign assign;
	assign.entity_handle = 0x0005; // the joiner's own H
	assign.team = 2;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::TEAM_ASSIGN, encode_team_assign(assign))}, packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input); // the net frame folds the 0x50 ahead of the spawn edge
	const w::Entity *L = h.kernel->local.player();
	if (!expect(h.role.local_spawned() && L != nullptr && L->team == 2,
			"self team: L spawns on the team a pre-spawn self 0x50 assigned"))
		return false;
	if (!expect(h.kernel->world.round_sim.local_team == 2,
			"self team: the round sim's team agrees"))
		return false;
	return expect(h.kernel->local.attack_defend_role == 1,
			"self team: the spawn edge's latch reads the assigned team");
}

// D-NET-570: a self S2C 0x50 on the local entity in match re-runs Player_InitPlayer(1):
// L takes the class word of the side its Team names (the session start's, or a 0x5A
// grant's for that side) and its hidden bit clears; then the handler stores the
// message's animSlot and NetId, so L ends with the 0x50's identity pair.
// [orig: NapiNPClientMsg_TeamAssign @0x431910 - @0x431B0A, Player_InitPlayer(1)
//  @0x431B14, animSlot @0x431B3A, NetId @0x431B46; Player_InitPlayer @0x4E15F0 - the
//  class by Team @0x4E1809..0x4E1832, the hidden clear @0x4E19C2]
bool run_a_self_team_assign_reinits_l() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId,
	                             /*game_type=*/0x10002u);
	inmatch::CharacterJoinVars vars;
	vars.char_class[0] = 6; // team 1 / 3
	vars.char_class[1] = 7; // team 0 / 2
	h.role.runtime->set_character_join_vars(vars);
	h.role.run_tick(h.input); // the spawn frame
	auto &row = h.role.runtime->state().upsert(0x0005);
	row.cls = EntityClass::Player;
	row.type_id = w::kPlayerInfantryTypeId;
	w::Entity *L = h.kernel->local.player();
	if (!expect(h.role.local_spawned() && L != nullptr, "self 0x50: L spawned")) return false;
	L->player_class = 8;
	L->hidden = true;
	TeamAssign assign;
	assign.entity_handle = 0x0005;
	assign.team = 1;
	assign.net_id = 0x0213;
	assign.anim_slot = 0x2C;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::TEAM_ASSIGN, encode_team_assign(assign))}, packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input);
	L = h.kernel->local.player();
	return expect(L != nullptr && L->team == 1 && L->player_class == 6 && !L->hidden &&
					L->anim_slot == 0x2C && L->minimap_net_id == 0x0213,
			"self 0x50: L takes team 1's class, unhides, and keeps the 0x50's identity pair");
}

// D-NET-570's pre-spawn half: L's spawn takes the mission start's Player_InitPlayer identity.
// The init restamps animSlot from the avatar byte of the side L's team names and never writes
// the NetId, so L keeps the self row's NetId (a pre-start 0x50's over the record's) and the
// side's avatar byte (not the record's animSlot); a self 0x50 whose local arm ran after the
// mission start bound the local entity, ahead of L's spawn, re-ran the init and stored its own
// pair, so L takes that 0x50's animSlot.
// [orig: Player_InitPlayer @0x4E15F0 - the Team test @0x4E1809..0x4E1817, g_AvatarTeam2
//  @0x4E1825 / g_AvatarTeam1 @0x4E1838, the store @0x4E1843; Game_StartMission @0x524360 -
//  Player_InitPlayer @0x525BBC; NapiNPClientMsg_TeamAssign @0x431910 - @0x431B0A,
//  Player_InitPlayer(1) @0x431B14, animSlot @0x431B3A, NetId @0x431B46]
bool run_the_spawn_takes_the_mission_starts_identity() {
	inmatch::CharacterJoinVars vars;
	vars.avatar[0] = 0x11; // team 1 / 3
	vars.avatar[1] = 0x22; // team 0 / 2
	bool ok = true;
	{
		Harness h;
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId,
		                             /*game_type=*/0x10002u, 0, true, /*self_team=*/2);
		h.role.runtime->set_character_join_vars(vars);
		// The self row as a pre-start 0x50 left it (the pipeline's identity fold).
		auto &row = h.role.runtime->state().upsert(0x0005);
		row.cls = EntityClass::Player;
		row.type_id = w::kPlayerInfantryTypeId;
		row.net_id = 0x0213;
		row.spawn_anim_slot = 0x2C;
		h.role.run_tick(h.input); // the spawn frame
		const w::Entity *L = h.kernel->local.player();
		ok &= expect(h.role.local_spawned() && L != nullptr && L->team == 2 &&
						L->anim_slot == 0x22 && L->minimap_net_id == 0x0213,
				"mission start identity: the side's avatar byte and the self row's NetId");
	}
	{
		Harness h;
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId,
		                             /*game_type=*/0x10002u);
		h.role.runtime->set_character_join_vars(vars);
		auto &row = h.role.runtime->state().upsert(0x0005);
		row.cls = EntityClass::Player;
		row.type_id = w::kPlayerInfantryTypeId;
		row.net_id = 0x0100;
		row.spawn_anim_slot = 0x05;
		TeamAssign assign;
		assign.entity_handle = 0x0005;
		assign.team = 1;
		assign.net_id = 0x0213;
		assign.anim_slot = 0x2C;
		SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::TEAM_ASSIGN, encode_team_assign(assign))}, packet);
		auto datagram =
				nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input); // the 0x50's local arm, then the spawn
		const w::Entity *L = h.kernel->local.player();
		ok &= expect(h.role.local_spawned() && L != nullptr && L->team == 1 &&
						L->anim_slot == 0x2C && L->minimap_net_id == 0x0213,
				"mission start identity: a post-start self 0x50 leaves L its own pair");
	}
	return ok;
}

// L's spawn takes the mission start's Player_InitPlayer class word: the class byte of the side
// its Team names (team 1 or 3 the first side), stored with no range test, whatever class the
// shell's pre-spawn apply latched (here 5); a joiner whose shell installed no join vars holds
// the fresh profile's 8 on both sides.
// [orig: Player_InitPlayer @0x4E15F0 - the Team test @0x4E1809..0x4E1817, g_CharClassTeam2
//  @0x4E1819..0x4E181F, g_CharClassTeam1 @0x4E182C..0x4E1832; Game_StartMission @0x525BBC;
//  PlayerProfile_InitDefaults - the two class bytes 8 @0x54BBE0 / @0x54BBE3]
bool run_the_spawn_takes_its_sides_class_word() {
	struct Case {
		uint8_t self_team;
		bool vars;
		uint8_t expected;
	};
	bool ok = true;
	for (const Case c : {Case{2, true, 7}, Case{1, true, 4}, Case{2, false, 8}}) {
		Harness h;
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId,
		                             /*game_type=*/0x10002u, 0, true, c.self_team);
		if (c.vars) {
			inmatch::CharacterJoinVars vars;
			vars.char_class[0] = 4; // team 1 / 3: a byte outside 5..9, stored as it is
			vars.char_class[1] = 7; // team 0 / 2
			h.role.runtime->set_character_join_vars(vars);
		}
		h.kernel->local.loadout.pending_player_class = 5; // the shell's pre-spawn latch
		auto &row = h.role.runtime->state().upsert(0x0005);
		row.cls = EntityClass::Player;
		row.type_id = w::kPlayerInfantryTypeId;
		h.role.run_tick(h.input); // the spawn frame
		const w::Entity *L = h.kernel->local.player();
		ok &= expect(h.role.local_spawned() && L != nullptr && L->team == c.self_team &&
						L->player_class == c.expected,
				"spawn class: the side word of L's team, unranged, never the shell's latch");
	}
	return ok;
}

// L spawns on the in-match edge, after the boot's definition sweeps: the edge
// binds its items.def row like the host's boot did, so L's body sounds resolve
// through its own profile instead of the slotless "default" (silent footsteps
// on every joiner). [orig: Entity_InitFromItemDef @0x49e550; the def+0x268
// profile binding @0x49fb0f..0x49fb64]
bool run_in_match_spawn_binds_local_player_definition() {
	Harness h;
	static const char kProfiles[] =
			"begin \"default\"\r\n"
			"end\r\n"
			"begin \"SP_JoinerSelf\"\r\n"
			"     SSLFootGND     T_DIRT_L\r\n"
			"end\r\n";
	if (!expect(h.kernel->world.tables.sound_profiles.parse(kProfiles, sizeof(kProfiles) - 1) == 2,
			"profiles parsed")) return false;
	std::vector<opennova::def::DefItemDef> rows(1);
	rows[0].id = static_cast<int>(w::kPlayerInfantryTypeId) +
			static_cast<int>(opennova::mission::kItemIdOffset);
	rows[0].hp = 100;
	std::snprintf(rows[0].sound_profile, sizeof(rows[0].sound_profile), "SP_JoinerSelf");
	opennova::def::DefItemsFile items{};
	items.entries = rows.data();
	items.count = rows.size();
	h.kernel->set_items_table(&items);

	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	const w::EntityHandle L = h.kernel->world.cached.local_player;
	if (!expect(h.role.local_spawned() && L.valid(), "L spawned on the edge")) return false;
	const w::AiEntity *body = h.kernel->world.ai.for_handle(L);
	const w::Entity *e = h.kernel->world.registry.get(L);
	bool ok = expect(body != nullptr && e != nullptr, "L has its body");
	if (!ok) return false;
	ok &= expect(body->profile.sound_profile == 1,
	             "L's sound profile is its items.def row's, not the slotless default");
	ok &= expect(e->has_item_def, "L carries its items.def traits");
	h.kernel->set_items_table(nullptr);
	return ok;
}

// The runtime owns H across removal and repair; every weapon and presenter
// consumer must follow it after the first local spawn instead of caching H.
bool run_self_handle_lifecycle() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, 5, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	const w::EntityHandle local = h.kernel->world.cached.local_player;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> body;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::ENTITY_REMOVE, {5, 0})}, body);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input);
	if (!expect(!h.role.runtime->has_self_handle() && !h.role.has_self_wire_handle() &&
					h.role.self_wire_handle() == w::EntityHandle::kInvalid,
			"role stops using the retired runtime identity (kInvalid, never handle 0)")) return false;
	// Slot 0 is a LIVE pool-0 handle (the listen host's own player). While the
	// identity is retired, a remote row there must keep flowing to every
	// consumer that excludes self by handle; a 0 sentinel silently dropped it.
	// The minefield actor fold is the cheapest such consumer on the frame.
	auto &host_row = h.role.runtime->state().upsert(0x0000);
	host_row.cls = EntityClass::Player;
	host_row.type_id = w::kPlayerInfantryTypeId;
	h.role.run_tick(h.input);
	bool host_row_seen = false;
	for (const auto &actor : h.kernel->world.minefields.remote_actors)
		if (actor.handle == 0x0000) host_row_seen = true;
	if (!expect(host_row_seen,
			"the slot-0 remote row is not mistaken for self while the identity is retired")) return false;
	// The replay/session seed is also a public identity replacement seam.
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, 7, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	return expect(h.role.self_wire_handle() == 7 &&
			h.kernel->world.cached.local_player == local,
			"role follows replacement identity without duplicating its local motor entity");
}

// reset_for_join re-arms the per-session latches for a fresh dial.
bool run_reset_for_join() {
	Harness h;
	h.role.run_tick(h.input);
	if (!expect(h.role.started(), "reset: latch armed before")) return false;
	h.role.reset_for_join();
	if (!expect(!h.role.started() && !h.role.local_spawned() &&
					h.role.self_wire_handle() == w::EntityHandle::kInvalid &&
					h.role.flat_seconds() == 0 &&
					!h.role.freeze_suspected(),
			"reset: per-session latches cleared")) return false;
	// The clock deliberately survives (retail's per-frame tick is process-scoped).
	return expect(h.role.now_tick() == 1, "reset: the clock is not a session latch");
}

// The D-NET-194 wire-header joiner: no pre-load kit, so inventory_valid is
// false at frame entry. The first S2C 0x5A grant folds and the
// apply_authoritative seam flips the live flag + arms the equipped slot
// INSIDE run_client_net_frame — i.e. BEFORE spawn_and_arm the SAME frame. The
// spawn edge must see that live flip and stamp L's equipped_adm_index;
// otherwise the entity carries the default adm on the C2S 0x0C uplink until
// the next respawn.
bool run_spawn_stamps_equipped_adm_from_midframe_grant() {
	Harness h;
	// Model retail's mid-frame apply: flip the live inventory-valid flag and
	// arm the equipped combo, exactly as apply_authoritative_loadout ->
	// local_loadout_rebuild does before the spawn block runs.
	constexpr int16_t kGrantedAdm = 16; // WPN_M16BURST-shaped grant
	h.role.kit_seams.apply_authoritative = [&h, kGrantedAdm] {
		h.seams.push_back("loadout");
		h.kernel->local.inventory_valid = true;
		h.inventory().equipped_combo = 0;
		h.inventory().slots[0].adm_index = kGrantedAdm;
	};
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	if (!expect(h.role.runtime->in_match(), "midframe grant: seeded runtime InMatch"))
		return false;

	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "midframe grant: L spawned")) return false;
	const w::Entity *L = h.kernel->world.registry.get(h.kernel->world.cached.local_player);
	if (!expect(L != nullptr, "midframe grant: L resolvable")) return false;
	return expect(L->equipped_adm_index == kGrantedAdm,
			"midframe grant: the same-frame grant stamped L's equipped adm "
			"(regression: a by-value inventory_valid froze it at frame entry)");
}


// A full-BMS join's promote spawns each carrier's addeweap child with no spawn
// origin, riding its carrier's attachment row; it is the decoded slot's own
// entity on both peers, so the replica rule takes it and a confirmed gunner
// mount on it attaches the local body (D-NET-620).
// [orig: NapiNPClientMsg_EntityDeath @0x42EBC6; Entity_SpawnAddEWeapChildren: the
//  Pool_AllocEntry call @0x40F393]
bool run_full_bms_child_gun_mount() {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	world.load_systems();
	h.role.poll_preload();
	constexpr uint16_t self_handle = 0x0005;
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, self_handle, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr && !h.kernel->wire_header_world,
			"child gun: a full-BMS joiner's local body")) return false;
	w::Entity carrier;
	carrier.kind = w::EntityKind::Item;
	carrier.item_id = 1889;
	carrier.has_item_def = true;
	carrier.item_type = 1;
	carrier.spawn_origin = (1u << 24) | 4u;
	carrier.position = local->position;
	carrier.position.x += 1.0f;
	carrier.health = carrier.health_max = 1000;
	carrier.team = local->team;
	const w::EntityHandle ch = world.registry.spawn(1, carrier);
	w::Entity gun;
	gun.kind = w::EntityKind::Item;
	gun.item_id = 1887;
	gun.has_item_def = true;
	gun.item_type = 1;
	gun.spawn_origin = w::kSpawnOriginNone;
	gun.position = world.registry.get(ch)->position;
	gun.health = gun.health_max = 100;
	gun.team = local->team;
	gun.emplacement_parent = ch;
	gun.emplacement_parent_spawn_id = world.registry.get(ch)->registry_spawn_id;
	gun.emplacement_pose_metadata_resolved = true;
	w::Seat gunner;
	gunner.type = w::SeatType::Gunner;
	gunner.bone_index = 1;
	gunner.source_name = "gunrx00";
	gun.seats.push_back(gunner);
	const w::EntityHandle gh = world.registry.spawn(1, gun);
	for (const w::EntityHandle row_handle : {ch, gh}) {
		auto &row = h.role.runtime->state().upsert(row_handle.packed);
		row.type_id = static_cast<uint16_t>(world.registry.get(row_handle)->item_id);
		row.cls = EntityClass::NoNetworkCallback;
	}
	auto &self = h.role.runtime->state().upsert(self_handle);
	self.cls = EntityClass::Player;
	self.type_id = w::kPlayerInfantryTypeId;
	self.carrier_handle = gh.packed;
	self.mount_bone = 1;
	self.state_flags = w::kEntityFlagMounted;
	self.state_flags_known = true;
	h.role.run_tick(h.input);
	local = h.kernel->local.player();
	return expect(local != nullptr && local->mounted && local->mount_target == gh,
			"a confirmed gunner mount on a full-BMS addeweap child attaches the local body");
}

// A decoded pool-1 mover's collision proxy names the decoded row's own native
// row as its registry twin by the replica rule (D-NET-676): a header-only
// world's owned lifetime, never a same-type newcomer spawned at the handle
// after it; on a full-BMS join the promoted (spawn-origin) row, never the
// joiner's own unattached spawn of the type. Retail keeps one entity per slot,
// the wire handle naming the slot's entity.
// [orig: NapiNPClientMsg_EntityDeath @0x42EBC6, the handle's slot address]
bool run_wire_proxy_twin_takes_the_owned_lifetime(bool wire_header_world) {
	constexpr uint16_t kType = 1291;
	Harness h;
	w::World &world = h.kernel->world;
	h.kernel->wire_header_world = wire_header_world;
	world.registry.configure_pool(1, 16);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, 0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	// A cveh vehicle def (type 1) naming no graphic: its proxy is candidate-
	// source eligible and carries no model.
	def::DefItemDef defs[2]{};
	defs[0].id = w::kPlayerInfantryTypeId + mission::kItemIdOffset;
	defs[0].type = 3;
	defs[0].hp = 100;
	defs[1].id = kType + mission::kItemIdOffset;
	defs[1].type = 1;
	std::strcpy(defs[1].ai_function, "cveh");
	def::DefItemsFile items{defs, 2};
	h.kernel->set_items_table(&items);
	h.kernel->resolve_collision_instances();
	const w::EntityHandle own{0x1003};
	const w::EntityHandle other{0x1004};
	bool ok = true;
	if (wire_header_world) {
		PoolSpawnBatch batch;
		PoolSpawnRecord rec;
		rec.slot_id = own.packed;
		rec.item_type_id = kType;
		rec.pos_x = 10 << 16;
		rec.pos_y = -20 << 16;
		rec.pos_z = 2 << 16;
		batch.records.push_back(rec);
		h.role.runtime->view().apply(s2c::POOL_SPAWN, encode_pool_spawn_batch(batch));
		h.role.run_tick(h.input);
		const w::Entity *mine = world.registry.get(own);
		const w::WireDynamicCollisionProxy *proxy = h.kernel->collision.wire_dynamic_proxy(own.packed);
		ok &= expect(mine != nullptr && h.role.materializer().owned(world, own) == mine &&
						proxy != nullptr && proxy->registry_twin == own,
				"wire proxy: a header-only world's owned row is the proxy's twin");
		world.registry.despawn(own);
		w::Entity newcomer;
		newcomer.kind = w::EntityKind::Item;
		newcomer.item_id = kType;
		newcomer.alive = true;
		newcomer.position = {10.0f, -20.0f, 2.0f};
		if (!expect(world.registry.spawn_at(own, newcomer) == own &&
						h.role.materializer().owned(world, own) == nullptr,
				"wire proxy: a same-type newcomer at the handle is not the owned lifetime"))
			return false;
		h.role.run_tick(h.input);
		proxy = h.kernel->collision.wire_dynamic_proxy(own.packed);
		ok &= expect(proxy != nullptr && !proxy->registry_twin.valid(),
				"wire proxy: the slot's same-type newcomer is no twin of the decoded row");
		return ok;
	}
	// A full-BMS join: the promoted row (its spawn origin set) and the joiner's
	// own spawn of the same type (no origin, no attachment).
	w::Entity promoted;
	promoted.kind = w::EntityKind::Item;
	promoted.item_id = kType;
	promoted.alive = true;
	promoted.spawn_origin = (1u << 24) | 4u;
	w::Entity local_spawn = promoted;
	local_spawn.spawn_origin = w::kSpawnOriginNone;
	if (!expect(world.registry.spawn_at(own, promoted) == own &&
					world.registry.spawn_at(other, local_spawn) == other,
			"wire proxy: the full-BMS rows spawn"))
		return false;
	for (const w::EntityHandle row_handle : {own, other}) {
		auto &row = h.role.runtime->state().upsert(row_handle.packed);
		row.type_id = kType;
		row.cls = EntityClass::Vehicle;
	}
	h.role.run_tick(h.input);
	const w::WireDynamicCollisionProxy *promoted_proxy =
			h.kernel->collision.wire_dynamic_proxy(own.packed);
	const w::WireDynamicCollisionProxy *local_proxy =
			h.kernel->collision.wire_dynamic_proxy(other.packed);
	ok &= expect(promoted_proxy != nullptr && promoted_proxy->registry_twin == own,
			"wire proxy: a full-BMS join's promoted row is its decoded row's twin");
	ok &= expect(local_proxy != nullptr && !local_proxy->registry_twin.valid(),
			"wire proxy: the joiner's own same-type spawn is no twin of the decoded row");
	return ok;
}

// The proximity lists the joiner's collision reads are rebuilt every game-loop frame, the
// local player alive or dead: a dead joiner (back in the deploy pick) keeps publishing the
// decoded rows' wire proxies.
// [orig: Entity_UpdateAllEntities @0x4c240a -> Entity_BuildProximityLists_Pool01 @0x4B9340,
//  no local-death test]
bool run_dead_joiner_keeps_its_wire_proxies() {
	constexpr uint16_t kType = 1291;
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, 0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	def::DefItemDef defs[2]{};
	defs[0].id = w::kPlayerInfantryTypeId + mission::kItemIdOffset;
	defs[0].type = 3;
	defs[0].hp = 100;
	defs[1].id = kType + mission::kItemIdOffset;
	defs[1].type = 1;
	std::strcpy(defs[1].ai_function, "cveh");
	def::DefItemsFile items{defs, 2};
	h.kernel->set_items_table(&items);
	h.kernel->resolve_collision_instances();
	const w::EntityHandle remote{0x1004};
	auto &row = h.role.runtime->state().upsert(remote.packed);
	row.type_id = kType;
	row.cls = EntityClass::Vehicle;
	h.role.run_tick(h.input);
	if (!expect(h.kernel->collision.wire_dynamic_proxy(remote.packed) != nullptr,
			"dead proxies: a living joiner publishes the decoded row's proxy"))
		return false;
	FrameUpdate death;
	death.carried_handle = 0xFFFF;
	death.health = 0;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(death))}, packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input);
	h.role.run_tick(h.input);
	if (!expect(!h.role.runtime->in_match() && h.role.runtime->in_game_loop(),
			"dead proxies: the death leaves the match phase, not the game loop"))
		return false;
	return expect(h.kernel->collision.wire_dynamic_proxy(remote.packed) != nullptr,
			"dead proxies: a dead joiner still publishes the decoded row's proxy");
}

// Retail drives the carrier from the mounted person's C2S 0x0C. Exercise
// the joiner frame's confirmed mount, input and client prediction together.
// [orig: Client_ProcessNetworkFrame @0x42c180;
// Entity_UpdateVehiclePhysics @0x48af00; net-re section 5.13]
bool run_confirmed_vehicle_drive(int occupancy, bool server_feedback = false,
		bool internet_conditions = false) {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	world.load_systems();
	h.role.poll_preload();
	constexpr uint16_t self_handle = 0x0005;
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, self_handle, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local && local->handle.packed != self_handle,
			"vehicle joiner: local body and wire identity differ")) return false;
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 1291;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.item_type = 1;
	vehicle.item_attrib = 0x40u;
	vehicle.net_class_code = static_cast<uint8_t>(EntityClass::Vehicle);
	vehicle.spawn_origin = (1u << 24) | 3u;
	vehicle.position = local->position;
	vehicle.position.x += 1.0f; // a step away: the scan aims from the eye at the seat point
	vehicle.health = vehicle.health_max = 3000;
	vehicle.team = local->team;
	w::Seat controller;
	controller.type = w::SeatType::Controller;
	controller.bone_index = 1;
	controller.retail_slot = 8;
	controller.source_name = "ctrlx00";
	if (occupancy == 1) controller.occupant = w::EntityHandle{self_handle};
	vehicle.seats.push_back(controller);
	if (occupancy == 4) {
		w::Seat passenger;
		passenger.type = w::SeatType::Passenger;
		passenger.bone_index = 4;
		passenger.retail_slot = 0;
		passenger.source_name = "sitex00";
		// Array order differs from the numbered HUD list: driver is key 1.
		vehicle.seats.insert(vehicle.seats.begin(), passenger);
	}
	const auto vh = world.registry.spawn(1, vehicle);
	// The buggy row's class init brains the hull: its client mover runs past its head.
	test_common::brain_hull(world, vh);
	w::EntityHandle previous_driver;
	if (occupancy == 2) {
		w::Entity peer;
		peer.kind = w::EntityKind::Organic;
		peer.team = local->team;
		peer.health = peer.health_max = 100;
		previous_driver = world.registry.spawn(0, peer);
		if (!expect(world.vehicles.process_attach(previous_driver, vh, 1),
				"fixture: previous driver occupies the confirmed seat")) return false;
		world.registry.get(previous_driver)->team = local->team == 1 ? 2 : 1;
	}
	w::VehicleTraits traits;
	traits.physics = 1;
	traits.player_control = true;
	traits.player_speed = 94 * 293;
	traits.acceleration = 15 * 4;
	traits.deceleration = 70 * 4;
	traits.turn_rate = 65 * 192426;
	traits.turn_rate2 = 41 * 192426;
	world.vehicles.traits.set(vehicle.item_id, traits);
	auto &row = h.role.runtime->state().upsert(vh.packed);
	row.type_id = static_cast<uint16_t>(vehicle.item_id);
	row.cls = EntityClass::Vehicle;
	row.x = w::to_fixed(vehicle.position.x);
	row.y = w::to_fixed(vehicle.position.y);
	row.z = w::to_fixed(vehicle.position.z);
	row.heading_known = true;
	row.heading_bam = vehicle.heading;
	row.net_has_compact = true;
	row.compact_revision = 1;
	auto &self = h.role.runtime->state().upsert(self_handle);
	self.cls = EntityClass::Player;
	self.type_id = w::kPlayerInfantryTypeId; // the spawn stream's identity: a compact never types a row
	self.carrier_handle = vh.packed;
	self.mount_bone = occupancy == 4 ? 4 : 1;
	self.state_flags = w::kEntityFlagMounted;
	self.state_flags_known = true;
	h.role.run_tick(h.input);
	local = h.kernel->local.player();
	if (occupancy == 4) {
		if (!expect(local->mount_type == w::SeatType::Passenger,
				"numbered-seat fixture starts in a confirmed passenger seat")) return false;
		h.socket.datagrams.clear();
		if (!expect(h.role.queue_numbered_seat(0),
				"seat key 1 queues the available driver seat")) return false;
		h.role.run_tick(h.input);
		ProtocolMessage selected;
		if (!expect(h.socket.last_message(0x26, selected) &&
				selected.payload == std::vector<uint8_t>({self_handle, 0,
					static_cast<uint8_t>(vh.packed), static_cast<uint8_t>(vh.packed >> 8), 1, 0}) &&
				h.kernel->local.player()->mount_bone == 4,
				"numbered seat emits the driver bone with H and waits for the host echo")) return false;
		h.role.runtime->state().find(self_handle)->mount_bone = 1;
		h.role.run_tick(h.input);
		local = h.kernel->local.player();
	}
	std::printf("vehicle joiner occupied=%d: mounted=%d local=%04x wire=%04x\n",
			occupancy, local->mounted, local->handle.packed, self_handle);
	if (!expect(local->mounted && local->mount_target == vh &&
			local->mount_type == w::SeatType::Controller,
			"retail-confirmed controller seat attaches the local joiner body")) return false;
	if (previous_driver.valid() &&
			!expect(!world.registry.get(previous_driver)->mounted,
					"confirmed seat assignment detaches the previous occupant")) return false;
	if (occupancy == 3) {
		world.registry.get(vh)->primary_occupant = {};
		world.registry.get(vh)->seats[0].occupant = {};
		h.role.run_tick(h.input);
	}
	if (!expect(world.registry.get(vh)->primary_occupant == local->handle &&
			world.registry.get(vh)->seats[occupancy == 4 ? 1 : 0].occupant == local->handle,
			"confirmed controller relation restores its control link")) return false;
	// Keep an independent authority alive: a prediction-only test can pass
	// while the host stays parked and every received compact pulls us back.
	auto authority = std::make_unique<w::World>();
	replication::LoopbackChannel channel;
	replication::Connection connection{
			&channel, replication::TransportMode::Client, w::EntityHandle{self_handle}, 0};
	if (server_feedback) {
		h.role.runtime->view().set_item_class_resolver([](uint16_t type) -> std::optional<EntityClass> {
			if (type == 1291) return EntityClass::Vehicle;
			if (type == w::kPlayerInfantryTypeId) return EntityClass::Player;
			return std::nullopt;
		});
		authority->registry.configure_pool(0, 16);
		authority->registry.configure_pool(1, 16);
		authority->load_systems();
		w::Entity peer = *local;
		// A seated eye above the zero water plane (the body's CameraOffset): the
		// driver's submerged test compares the raw words, no dry sentinel
		// (D-NET-NEW-L4-8) [orig: Entity_UpdateVehiclePhysics @0x48B9A0..0x48B9AC].
		peer.eye_offset_z = 1 << 16;
		peer.mounted = false;
		peer.mount_target = {};
		peer.mount_seat = -1;
		peer.flags &= ~w::kEntityFlagMounted;
		authority->registry.spawn_from(0, self_handle, peer);
		authority->ai.attach(w::EntityHandle{self_handle});
		w::Entity carrier = vehicle;
		for (auto &seat : carrier.seats) seat.occupant = {};
		authority->registry.spawn_from(1, vh.slot(), carrier);
		test_common::brain_hull(*authority, vh);
		authority->vehicles.traits.set(vehicle.item_id, traits);
		if (!expect(authority->vehicles.process_attach(
				w::EntityHandle{self_handle}, vh, 1), "authority confirms driver")) return false;
	}
	// NovaWorld dictates a 12-tick uplink period. Add six ticks each way
	// (a 192 ms round trip: 12 ticks of 16 ms); only newly emitted controls reach the authority.
	// Re-sending the last observed packet every tick would hide a pacing bug.
	const int period = internet_conditions ? 12 : 4;
	const int delay = internet_conditions ? 6 : 0;
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	auto frame_server = [&](const std::vector<ProtocolMessage> &messages) {
		std::vector<uint8_t> body;
		if (!frame_session_packet(server_tx, SessionCrypto{kServerScrk, {}, kClientKey},
				messages, body)) return std::vector<uint8_t>{};
		return nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
	};
	if (internet_conditions) {
		const auto settings = frame_server({make_protocol_message(0x00,
				{0x01, 0x08, 0x00, 0x00, 0x00, 12, 0x00, 0x00, 0x00}, 0xA0)});
		h.role.runtime->receive(settings.data(), settings.size());
	}
	std::deque<std::pair<int, ProtocolMessage>> toward_host;
	std::deque<std::pair<int, std::vector<uint8_t>>> toward_client;
	std::size_t sent_cursor = h.socket.datagrams.size();
	int control_packets = 0;
	h.kernel->local.set_movement_keys(true, false, false, false, false, false, false);
	const int drive_ticks = internet_conditions ? 186 : 62;
	for (int tick = 0; tick < drive_ticks; ++tick) {
		while (!toward_client.empty() && toward_client.front().first <= tick) {
			const auto &packet = toward_client.front().second;
			h.role.runtime->receive(packet.data(), packet.size());
			toward_client.pop_front();
		}
		h.role.run_tick(h.input);
		if (!server_feedback) continue;
		for (; sent_cursor < h.socket.datagrams.size(); ++sent_cursor) {
			const auto &packet = h.socket.datagrams[sent_cursor];
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			ProtocolPacketHeader header;
			std::vector<ProtocolMessage> messages;
			if (!nw_decode_inbound(packet.data(), packet.size(), opcode, body) ||
					opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
					!decode_protocol_packet_plaintext(body.data(), body.size(), kClientScrk,
						header, messages)) continue;
			for (auto &message : messages) {
				if (message.tag != 0x0C) continue;
				++control_packets;
				toward_host.emplace_back(tick + delay, std::move(message));
			}
		}
		while (!toward_host.empty() && toward_host.front().first <= tick) {
			const auto &message = toward_host.front().second;
			channel.client_send(message.tag, message.payload);
			toward_host.pop_front();
		}
		replication::drain_connection_c2s(*authority, connection, true);
		authority->run_logic_tick(true);
		if ((tick + 1) % period == 0) {
			if (!expect(replication::emit_connection_s2c(*authority, connection,
					replication::snapshot_world(*authority)), "authority emits vehicle update"))
				return false;
			replication::Datagram update;
			while (channel.client_recv(update)) {
				auto packet = frame_server({make_protocol_message(update.tag, update.body)});
				if (!expect(!packet.empty(), "authority frames a real S2C update")) return false;
				toward_client.emplace_back(tick + delay, std::move(packet));
			}
		}
	}
	if (internet_conditions &&
			!expect(h.role.runtime->send_holdoff_ticks() == 12 &&
					control_packets >= 15 && control_packets <= 16,
					"NovaWorld vehicle controls are emitted once every twelve ticks")) return false;
	if (server_feedback) {
		if (!expect(h.role.runtime->view().malformed_bodies() == 0 &&
				h.role.runtime->state().find(vh.packed)->compact_revision > 1,
				"continuous server vehicle records were decoded and applied")) return false;
		const auto *host_vehicle = authority->registry.get(vh);
		if (!expect(host_vehicle->veh.speed > 0 &&
				std::hypot(host_vehicle->position.x - vehicle.position.x,
						host_vehicle->position.y - vehicle.position.y) > 0.5f,
				"the authority drives the vehicle from real joiner packets")) return false;
	}
	const auto *driven = world.registry.get(vh);
	ProtocolMessage movement;
	EntityPacketSubHeader sub;
	PlayerExtendedUplink uplink;
	size_t header_bytes = 0, body_bytes = 0;
	if (!expect(h.socket.last_message(0x0C, movement) &&
			decode_entity_packet_sub_header(movement.payload.data(), movement.payload.size(),
					sub, header_bytes) &&
			decode_player_extended_uplink(movement.payload.data() + header_bytes,
					movement.payload.size() - header_bytes, uplink, body_bytes) &&
			sub.handle == self_handle && header_bytes + body_bytes == movement.payload.size(),
			"the real outgoing C2S movement packet identifies the joiner by its host handle"))
		return false;
	const float dx = driven->position.x - vehicle.position.x;
	const float dy = driven->position.y - vehicle.position.y;
	std::printf("vehicle joiner: move=%02x carrier=%04x speed=%d distance_squared=%.4f\n",
			uplink.move_input_byte, uplink.carrier_handle, driven->veh.speed, dx * dx + dy * dy);
	if (!expect(uplink.carrier_handle == vh.packed &&
			(uplink.move_input_byte & w::Entity::kMoveOrderMoving) != 0,
			"mounted joiner uplinks its vehicle and held forward input") ||
			!expect(driven->veh.speed > 0 && dx * dx + dy * dy > 0.25f,
					"holding forward predicts motion of the confirmed driver vehicle")) return false;

	def::DefVehicleHudBlock hud_block{};
	if (occupancy == 4) hud_block.seat_count = 1;
	std::vector<hud::HudVehicleSeat> panel;
	w::fill_vehicle_panel_seats(world, vh, local->handle, hud_block, panel, &h.role);
	if (!expect(panel.size() == (occupancy == 4 ? 2u : 1u) && panel[0].occupied && panel[0].own_seat &&
			panel[0].health == local->health,
			"vehicle overlay highlights the confirmed local seat and its health")) return false;

	if (occupancy == 4 || server_feedback) return true;

	// The use-item scan can select another nearby carrier while mounted.
	// It must queue attach, keep the current seat until the echo, then detach
	// only when no alternative seat is available.
	w::Entity adjacent = vehicle;
	adjacent.position = h.kernel->local.player_position();
	adjacent.position.x += 1.0f;
	adjacent.seats[0].occupant = {};
	const auto adjacent_h = world.registry.spawn(1, adjacent);
	// Seated, the scan admits only what the rider looks at (5 deg cone).
	look_at(world, local->handle, adjacent.position.x, adjacent.position.y, adjacent.position.z);
	std::vector<w::AttachLabel> labels;
	h.kernel->local.collect_attach_labels(labels, &h.role);
	bool adjacent_highlighted = false;
	for (const auto &label : labels) {
		if (label.nearest && label.entity == adjacent_h && label.seat_index == 0)
			adjacent_highlighted = true;
	}
	if (!expect(adjacent_highlighted,
			"seat overlay highlights the same nearby seat the mounted action selects")) return false;
	h.socket.datagrams.clear();
	if (!expect(h.role.queue_mount_toggle(), "mounted use-item queues a request")) return false;
	h.role.run_tick(h.input);
	ProtocolMessage request;
	if (!expect(h.socket.last_message(0x26, request) &&
			request.payload == std::vector<uint8_t>({
					static_cast<uint8_t>(self_handle), 0,
					static_cast<uint8_t>(adjacent_h.packed),
					static_cast<uint8_t>(adjacent_h.packed >> 8), 1, 0}) &&
			h.kernel->local.player()->mount_target == vh,
			"mounted use-item requests the nearby seat and waits for host confirmation"))
		return false;
	h.role.runtime->state().find(self_handle)->carrier_handle = adjacent_h.packed;
	h.role.run_tick(h.input);
	if (!expect(h.kernel->local.player()->mount_target == adjacent_h,
			"seat swap applies on the host's relationship echo")) return false;
	world.registry.get(vh)->health = 0;
	h.socket.datagrams.clear();
	if (!expect(h.role.queue_mount_toggle(), "use-item without another seat queues detach"))
		return false;
	h.role.run_tick(h.input);
	if (!expect(h.socket.last_message(0x27, request) &&
			h.kernel->local.player()->mount_target == adjacent_h,
			"dismount also waits for the host's confirmation")) return false;
	auto *detached_self = h.role.runtime->state().find(self_handle);
	detached_self->carrier_handle = w::EntityHandle::kInvalid;
	detached_self->mount_bone = 0;
	h.role.run_tick(h.input);
	return expect(!h.kernel->local.player()->mounted &&
			!world.registry.get(adjacent_h)->seats[0].occupant.valid() &&
			!world.registry.get(adjacent_h)->primary_occupant.valid(),
			"confirmed dismount releases the local seat and controller");
}


// A joiner's own request turns L to the seat before it queues the C2S 0x26, as
// the retail client's request does; the host's echo then seats L and turns
// nothing (D-NET-421). [orig: Entity_RequestVehicleAttach @0x43655F..0x4365CE,
// the queue @0x436602; the client's receive arm Entity_TryAttachOrDetach
// @0x4366E8 -> Entity_ProcessVehicleAttach, which writes no yaw]
bool run_request_turns_l_before_the_0x26() {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	world.load_systems();
	h.role.poll_preload();
	constexpr uint16_t self_handle = 0x0005;
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, self_handle, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	w::AiEntity *body = local != nullptr ? world.ai.for_handle(local->handle) : nullptr;
	if (!expect(body != nullptr, "request fixture has L and its body")) return false;
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.position = local->position;
	vehicle.position.x += 1.0f; // a step away: the scan aims from the eye at the seat point
	vehicle.health = vehicle.health_max = 100;
	vehicle.team = local->team;
	w::Seat seat;
	seat.type = w::SeatType::Passenger;
	seat.bone_index = 2;
	seat.retail_slot = 0;
	seat.source_name = "sitex00";
	seat.yaw_offset = 30;
	vehicle.seats.push_back(seat);
	const auto vh = world.registry.spawn(1, vehicle);
	look_at(world, local->handle, vehicle.position.x, vehicle.position.y, vehicle.position.z);
	body->inf.target_heading = body->heading;
	h.kernel->local.input.look_heading = body->heading;
	const int32_t turned = opennova::io::bam_sub(
			w::emplaced_gun_frame_heading(*world.registry.get(vh)), w::bam_from_degrees_wrapped(30));
	if (!expect(body->heading != turned, "fixture: L faces away from the seat's yaw")) return false;
	h.socket.datagrams.clear();
	if (!expect(h.role.queue_mount_toggle(), "Use queues the seat request")) return false;
	if (!expect(body->heading == turned && body->inf.target_heading == turned &&
			!h.kernel->local.player()->mounted,
			"the request turns L to the seat before the host answers")) return false;
	h.role.run_tick(h.input);
	ProtocolMessage request;
	if (!expect(h.socket.last_message(0x26, request) &&
			h.kernel->local.input.look_heading == turned,
			"the 0x26 leaves and the frame folds the turned look into L's input")) return false;
	// The echo seats L and keeps a look the player moved since. Its success
	// copies L's Yaw into the look (D-NET-447), but the frame's input carried
	// the moved look into the Yaw before the receive, as the look arms write
	// both, so the copy is the identity here. [orig: Input_HandleActionBinding_0
	//  cases 166 / 167 @0x4E10A7..0x4E10AA / @0x4E110C..0x4E110F, queued by
	//  Input_ProcessMouseAxisBindings @0x499680 and run in Input_ProcessFrame (the
	//  call @0x52661D), ahead of Client_ProcessNetworkFrame @0x526692 ->
	//  CNapiNetwork_PumpClientProtocolRecv @0x42C228; Entity_ProcessVehicleAttach
	//  @0x435c5e..0x435c61]
	constexpr int32_t moved = 0x0F000000;
	h.kernel->local.input.look_heading = moved;
	// A sitex attach runs no camera reset, so the echo keeps raised binoculars;
	// neither does the detach (D-WPN-49). [orig: Entity_AttachToVehicleSlot
	//  type 1 @0x4946e3..0x494746 (the resets @0x4947ae / @0x4948b4 are types 2 /
	//  5); Entity_DetachFromVehicle @0x4355F0 writes no g_BinocularsToggle]
	h.kernel->local.view.binoculars_requested = true;
	auto &self = h.role.runtime->state().upsert(self_handle);
	self.cls = EntityClass::Player;
	self.type_id = w::kPlayerInfantryTypeId;
	self.carrier_handle = vh.packed;
	self.mount_bone = 2;
	self.state_flags = w::kEntityFlagMounted;
	self.state_flags_known = true;
	h.role.run_tick(h.input);
	if (!expect(h.kernel->local.player()->mount_target == vh &&
			body->heading == moved && h.kernel->local.input.look_heading == moved &&
			h.kernel->local.view.binoculars_requested,
			"the host's echo seats L, writes no yaw and keeps the binoculars"))
		return false;
	auto &detached = h.role.runtime->state().upsert(self_handle);
	detached.carrier_handle = w::EntityHandle::kInvalid;
	detached.mount_bone = 0;
	detached.state_flags = 0;
	h.role.run_tick(h.input);
	return expect(!h.kernel->local.player()->mounted &&
			h.kernel->local.view.binoculars_requested,
			"the host's detach echo keeps the binoculars");
}

// A remote player has only a decoded pool-0 row on a joiner. Its current
// carrier/bone must still occupy the seat for both Use and the health panel.
bool run_remote_vehicle_occupancy() {
	Harness h;
	auto &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, 0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	const auto *local = h.kernel->local.player();
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.item_id = 1291;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.item_type = 1;
	vehicle.item_attrib = 0x40u;
	vehicle.position = local->position;
	vehicle.health = vehicle.health_max = 3000;
	vehicle.team = local->team;
	w::Seat seat;
	seat.type = w::SeatType::Controller;
	seat.bone_index = 1;
	seat.retail_slot = 8;
	vehicle.seats.push_back(seat);
	const auto vh = world.registry.spawn(1, vehicle);
	auto &peer = h.role.runtime->state().upsert(0x0006);
	peer.cls = EntityClass::Player;
	peer.type_id = w::kPlayerInfantryTypeId;
	peer.carrier_handle = vh.packed;
	peer.mount_bone = 1;
	peer.net_has_compact = true;
	peer.health_class_byte = 0x18;
	peer.team = static_cast<uint8_t>(local->team);
	peer.team_known = true;
	def::DefItemDef person_def{};
	person_def.id = w::kPlayerInfantryTypeId + mission::kItemIdOffset;
	person_def.hp = 100;
	def::DefItemsFile items{&person_def, 1};
	h.kernel->set_items_table(&items);
	h.role.run_tick(h.input);
	look_at(world, local->handle, vehicle.position.x, vehicle.position.y, vehicle.position.z);
	std::vector<w::AttachLabel> labels;
	h.kernel->local.collect_attach_labels(labels, &h.role);
	bool ok = expect(labels.empty(), "remote occupied seat has no free-seat label");
	ok &= expect(!h.role.queue_mount_toggle(), "Use cannot request a remote occupied seat");
	def::DefVehicleHudBlock block{};
	std::vector<hud::HudVehicleSeat> panel;
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(panel.size() == 1 && panel[0].occupied && !panel[0].own_seat,
	             "remote rider appears as occupied in the vehicle panel");
	ok &= expect(panel[0].health == 59 && panel[0].max_health == 100,
	             "remote rider health uses the retail compact-tier midpoint");
	// A rider the 0x13 killed holds Health 0 ahead of its next record, as the
	// powerup picker reads it [orig: Entity_KillBySlotId Health = 0 @0x42BD33].
	h.role.runtime->state().find(0x0006)->net_health_zero = true;
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(panel.size() == 1 && panel[0].health == 0 && panel[0].max_health == 100,
	             "a rider the 0x13 killed shows Health 0");
	h.role.runtime->state().find(0x0006)->net_health_zero = false;

	// Another wire player can occupy the numeric handle of native L.
	auto old_peer = *h.role.runtime->state().find(0x0006);
	old_peer.handle = local->handle.packed;
	h.role.runtime->state().find(0x0006)->mount_bone = 0;
	h.role.runtime->state().upsert(local->handle.packed) = old_peer;
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(panel[0].occupied && !panel[0].own_seat,
	             "a remote wire handle equal to L never highlights an own seat");

	// Same-team passengers can use the remaining seat; an enemy in the
	// controller seat blocks the whole vehicle, for labels and Use alike.
	w::Seat passenger = seat;
	passenger.type = w::SeatType::Passenger;
	passenger.bone_index = 2;
	passenger.retail_slot = 0;
	world.registry.get(vh)->seats.push_back(passenger);
	h.kernel->local.collect_attach_labels(labels, &h.role);
	ok &= expect(labels.size() == 1 && labels[0].seat_index == 1,
	             "same-team remote rider leaves other seats available");
	auto *remote = h.role.runtime->state().find(local->handle.packed);
	remote->team = local->team == 1 ? 2 : 1;
	h.kernel->local.collect_attach_labels(labels, &h.role);
	ok &= expect(labels.empty() && !h.role.queue_mount_toggle(),
	             "a remote enemy blocks the entire vehicle for labels and Use");

	// Compact dismount supersedes even a retained spawn mountHandles entry.
	world.registry.get(vh)->seats[0].occupant = local->handle;
	remote->mount_bone = 0;
	remote->carrier_handle = w::EntityHandle::kInvalid;
	h.kernel->local.collect_attach_labels(labels, &h.role);
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(labels.size() == 2 && !panel[0].occupied,
	             "remote dismount frees selection and panel despite stale spawn occupancy");
	world.registry.get(vh)->seats[0].occupant = w::EntityHandle{0x5000};
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(!panel[0].occupied && panel[0].label.empty(),
	             "an unresolved raw control handle draws neither a rider marker nor a digit");

	// The retained spawn slot can still name our wire H before the first self
	// compact reconciles it to L: that seat is the local player's own.
	auto &self_row = h.role.runtime->state().upsert(0x0005);
	self_row.cls = EntityClass::Player;
	world.registry.get(vh)->seats[0].occupant = w::EntityHandle{0x0005};
	w::fill_vehicle_panel_seats(world, vh, local->handle, block, panel, &h.role);
	ok &= expect(panel[0].occupied && panel[0].own_seat && panel[0].health == local->health,
	             "a retained H occupant reads as the local player's own seat");
	h.kernel->local.collect_attach_labels(labels, &h.role);
	ok &= expect(labels.size() == 1 && labels[0].seat_index == 1,
	             "the retained H seat is not offered to the local player again");
	return ok;
}

// A real local weapon pump must carry its selected lock through C2S 0x06.
// [orig: WeaponAction_Fire target read @0x542C00..0x542C15;
//  NetPacket_WriteEntityPositionUpdate target store @0x42A759]
bool run_guided_fire_preserves_selected_target() {
    Harness h;
    auto &world = h.kernel->world;
    world.tables.weapons.entries.resize(2);
    auto &def = world.tables.weapons.entries[1];
    def.valid = true; def.name = "WPN_GUIDED"; def.ammo_index = 1;
    world.tables.ammo.entries.resize(2);
    world.tables.ammo.entries[1].valid = true;
    world.tables.ammo.entries[1].max_age_ticks = 100;
    world.tables.ammo.entries[1].velocity = 248;
    world.tables.ammo.entries[1].tracer_item_friendly = 10;
    world.tables.ammo.entries[1].tracer_item_enemy = 10;
    world.throwables.classes.set({10,w::ThrowClass::kNone,w::ThrowClass::kJavelin});
    h.role.kit_seams.apply_authoritative = [&h] {
        h.kernel->local.inventory_valid = true;
        h.inventory().equipped_combo = 0;
        h.inventory().slots[0].adm_index = 1;
    };
    h.role.poll_preload();
    h.role.runtime->seed_session(kSessionId,kClientKey,kClientScrk,kServerScrk,
        1,0,0x0005,w::kPlayerInfantryTypeId);
    h.role.run_tick(h.input);
    w::WeaponInstallData data;
    data.name = "WPN_GUIDED"; data.clipsize = 4; data.rows.resize(3);
    std::snprintf(data.rows[0].name,sizeof(data.rows[0].name),"idle");
    std::snprintf(data.rows[1].name,sizeof(data.rows[1].name),"fire");
    data.rows[1].delayend = 6;
    std::snprintf(data.rows[2].name,sizeof(data.rows[2].name),"recoil");
    w::local_weapon_install(world,h.weapon(),data,false,false,nullptr,h.kernel->local.view);
    h.kernel->local.inventory_valid = false;
    constexpr uint16_t target = 0x1003;
    auto *body = world.ai.for_handle(world.cached.local_player);
    if (!expect(body != nullptr,"guided: local body spawned")) return false;
    body->slot.f[3] = int(target)+1;
    // The latest player lock must win over an older self replica at launch.
    h.role.runtime->state().upsert(0x0005).fire_target_handle = 0x1002;
    auto &target_row = h.role.runtime->state().upsert(target);
    target_row.cls = EntityClass::Vehicle;
    target_row.x = 200*65536; target_row.y = 30*65536; target_row.z = 4*65536;
    h.weapon().fire_pressed = h.weapon().fire_held = true;
    h.role.run_tick(h.input);
    h.role.run_tick(h.input);
    ProtocolMessage message;
    if (!expect(h.socket.last_message(0x06,message),"guided: C2S fire sent")) return false;
    ClientFiredRound fire;
    size_t consumed = 0;
    if (!expect(decode_client_fired_round(message.payload.data(),message.payload.size(),fire,consumed)
        && fire.target_handle == target,"guided: C2S fire retains selected target")) return false;
    for (const auto &round : world.round_sim.rounds) {
        if (round.guided_family == w::GuidedFamily::Javelin)
            return expect(round.active && round.guided.target == target,
                "guided: predicted launch retains current lock despite stale replica");
    }
    return expect(false,"guided: local fire creates a predicted Javelin");
}

// A pilot's countermeasure rides the same C2S 0x06 as the handheld, and the
// descriptor's side bytes are the PILOT's, not the flare's. Retail: the flare
// fires with the vehicle's occupantEntity as the shooter and
// (aiRuntime[3], targetId 1, ammoDefIndex, weaponSlot 0)
// [orig: Weapon_FireProcess @0x53f6d6 -> @0x53f70a]; the writer then stores
// targetId at off6 (@0x42a68a), the ammo-def index at off7 (@0x42a69b),
// *(WORD*)(shooter+352) — the pilot's handheld ammo-def index — at off32
// (@0x42c052 -> @0x42a7da), fire_flags at off33 (@0x42a7ed) and weaponSlot at
// off34 (@0x42a800). fire_flags = Weapon_GetScopeZoomLevel(can_fire, 12) |
// (can_fire ? 0x80 : 0) [orig: Entity_FireWeaponAndSendPacket @0x42bdd6..
// 0x42bdf9] with can_fire = Player_IsOpticalViewVisible() = 0 for a seated pilot
// (parentSlot 2/5 @0x5cf7a8..0x5cf7b6), and weaponActive 0 returns the default
// 12 [orig: @0x422fd1/@0x422fd5]. Expected bytes derived by hand from those
// legs: off6 = 1, off7 = the flare index, off32 = the handheld's ammo-def index
// (the retail capture's 0x03 beside adm 69, net-re 5.66 seeds the row),
// off33 = 12, off34 = 0, off28 = 0xFFFF (no AI runtime target). The yaw/pitch
// words are (bam + 0x8000) >> 16 [orig: @0x42a6e1/@0x42a6fb].
bool run_flare_descriptor_carries_the_pilot_handheld() {
	Harness h;
	constexpr int16_t kHandheldAdm = 69; // WPN_M4AUTO-shaped row
	constexpr int32_t kHandheldAmmo = 3; // its ammo-def index (the captured off32)
	constexpr uint8_t kFlareAmmo = 41;   // the FLARE ammo-def index; carried verbatim
	h.role.kit_seams.apply_authoritative = [&h, kHandheldAdm] {
		h.seams.push_back("loadout");
		h.kernel->local.inventory_valid = true;
		h.inventory().equipped_combo = 0;
		h.inventory().slots[0].adm_index = kHandheldAdm;
	};
	w::World &world = h.kernel->world;
	world.tables.weapons.entries.resize(static_cast<size_t>(kHandheldAdm) + 1);
	w::WeaponTableEntry &handheld = world.tables.weapons.entries[static_cast<size_t>(kHandheldAdm)];
	handheld.valid = true;
	handheld.name = "WPN_M4AUTO";
	handheld.ammo_index = kHandheldAmmo;

	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "flare: L spawned")) return false;
	const w::Entity *L = world.registry.get(world.cached.local_player);
	if (!expect(L != nullptr && L->equipped_adm_index == static_cast<uint8_t>(kHandheldAdm),
			"flare: L carries the handheld adm")) return false;
	if (!expect(world.ai.for_handle(L->handle) != nullptr, "flare: L carries its AI body"))
		return false;

	// What VehicleSystem::release_flares -> RoundSim::fire_source queues on a
	// joiner whose local player pilots the source (the launch already presented).
	w::RoundSpawnParams flare;
	flare.launch_presented = true;
	flare.owner = L->handle;
	flare.shooter_handle = L->handle.packed;
	flare.origin = w::Vec3{12.0f, -7.0f, 30.0f};
	flare.dir_yaw_bam = 0x20000000;
	flare.dir_pitch_bam = 0x10000000;
	flare.ammo_index = kFlareAmmo;
	flare.adm_index = kFlareAmmo;
	world.out.source_fires.push_back(flare);

	// The descriptor queues in the frame's weapon phase and ships with a send.
	h.role.run_tick(h.input);
	h.role.run_tick(h.input);
	ProtocolMessage msg;
	if (!expect(h.socket.last_message(0x06, msg), "flare: a C2S 0x06 shipped")) return false;
	ClientFiredRound r;
	size_t consumed = 0;
	if (!expect(decode_client_fired_round(msg.payload.data(), msg.payload.size(), r, consumed) &&
					consumed == 45,
			"flare: the fixed 45 B body decodes")) return false;
	bool ok = true;
	ok &= expect(r.shooter_handle == 0x0005, "flare: the shooter is the pilot's wire handle");
	ok &= expect(r.fire_flags == 1, "flare: off6 = targetId 1 (the ammo-def arm)");
	ok &= expect(r.adm_index == kFlareAmmo, "flare: off7 = the flare's ammo-def index");
	ok &= expect(r.extra_byte1 == kHandheldAmmo,
			"flare: off32 = the pilot's handheld ammo-def index (entity+0x160), not the flare's");
	ok &= expect(r.extra_byte2 == 12,
			"flare: off33 = Weapon_GetScopeZoomLevel(0, 12) = 12 for a seated pilot");
	ok &= expect(r.misc_byte == 0, "flare: off34 = weaponSlot 0");
	ok &= expect(r.target_handle == 0xFFFF, "flare: off28 = 0xFFFF without an AI target");
	ok &= expect((r.hit_part & 0x1FF) == 1 &&
					(r.hit_part >> 9) == h.role.runtime->local_player_slot(),
			"flare: hit_part packs (roster slot << 9) | shot seq 1");
	ok &= expect(r.pos_x == 12 * 65536 && r.pos_y == -7 * 65536 && r.pos_z == 30 * 65536,
			"flare: the launch origin rides off8..off19 in full");
	ok &= expect(r.dir_x == 0x2000 && r.dir_y == 0x1000,
			"flare: the launch yaw/pitch ride as (bam + 0x8000) >> 16");
	ok &= expect(world.out.source_fires.empty(), "flare: the source-fire queue drained");
	return ok;
}

// An S2C 0x49 naming a non-Person EWeap entity reloads that entity's own MountSlot:
// the remaining clip is refunded through the slot's re-split, then a clip is drawn
// from its reserve [orig: NapiNPClientMsg_WeaponReload_0x049 @0x42C0A0 - the
//  non-Person WeaponSlot_ReloadAmmo call @0x42C116; WeaponSlot_ReloadAmmo's EWeap
//  slot @0x54172D, the re-split @0x5462A0, sub_546200 / Entity_SpendMountSlotReserve, the clip store
//  @0x541850].
bool run_received_reload_refills_an_eweap_entity() {
    Harness h;
    h.role.poll_preload();
    h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
            1, 0, 5, w::kPlayerInfantryTypeId);
    w::World &world = h.kernel->world;
    world.registry.configure_pool(1, 8);
    world.tables.weapons.entries.resize(2);
    w::WeaponTableEntry &gun = world.tables.weapons.entries[1];
    gun.name = "WPN_TANKGUN";
    gun.valid = true;
    gun.clipsize = 4;
    gun.startrounds = 12;
    gun.ammo_class_count = 1;
    w::Entity tank;
    tank.kind = w::EntityKind::Item;
    tank.has_item_def = true;
    tank.item_type = 1;
    tank.item_attrib = w::kItemAttribEweap;
    tank.primary_weapon = gun.name;
    const w::EntityHandle tank_h = world.registry.spawn(1, tank);
    w::Entity *live = world.registry.get(tank_h);
    if (!expect(live != nullptr && world.vehicles.prepare_weapon_slot(*live),
            "reload: the tank's slot resolves")) return false;
    live->primary_weapon_slot.clip = 1;
    live->primary_weapon_slot.reserve = 8;
    SessionSequencing tx = inmatch::make_jo_game_session_sequencing();
    WeaponReload reload;
    reload.entity_handle = tank_h.packed;
    reload.reload_param = 0xFFFF;
    std::vector<uint8_t> body;
    if (!frame_session_packet(tx, SessionCrypto{kServerScrk, {}, kClientKey},
            {make_protocol_message(s2c::WEAPON_RELOAD, encode_weapon_reload(reload))}, body))
        return false;
    const auto datagram = nw_encode_outbound(
            SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
    h.role.runtime->receive(datagram.data(), datagram.size());
    h.role.run_tick(h.input);
    // 1 + 8 + 1 capped at 12: 4 / 6, then the draw of 4: 4 / 2.
    return expect(live->primary_weapon_slot.clip == 4 && live->primary_weapon_slot.reserve == 2,
            "reload: the EWeap entity refills its own slot from its reserve");
}

bool run_received_loadout_policy_and_sounds() {
    Harness h;
    h.role.poll_preload();
    h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
            1, 0, 5, w::kPlayerInfantryTypeId);
    SessionSequencing tx = inmatch::make_jo_game_session_sequencing();
    auto deliver = [&](const std::vector<ProtocolMessage> &messages) {
        std::vector<uint8_t> body;
        if (!frame_session_packet(tx, SessionCrypto{kServerScrk, {}, kClientKey},
                messages, body)) return false;
        const auto datagram = nw_encode_outbound(
                SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(body));
        h.role.runtime->receive(datagram.data(), datagram.size());
        h.role.run_tick(h.input);
        return true;
    };
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS,
            {5, 5, 0, 7, 2, 9, 1, 10, 3, 255, 0})})) return false;
    auto &availability = h.kernel->local.loadout.availability;
    if (!expect(availability.value_for(5) == 0 && availability.value_for(7) == 2,
            "host weapon bans and armory-only rules reach the joiner's real loadout table")) return false;
    if (!expect(availability.value_for(9) == 1 && availability.value_for(10) == 1,
            "retail ignores restriction values other than 0 and 2")) return false;
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS, {0})})) return false;
    if (!expect(availability.value_for(5) == 1 && availability.value_for(7) == 1,
            "an empty restriction list restores the all-allowed table")) return false;
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS, {1, 254, 2})})) return false;
    if (!expect(availability.value_for(254) == 2,
            "the final valid weapon index accepts an armory restriction")) return false;
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS, {1, 9})})) return false;
    if (!expect(availability.value_for(9) == 0 && availability.value_for(254) == 1,
            "a short restriction pair zero-fills its missing value after resetting the table")) return false;
    if (!deliver({make_protocol_message(s2c::WEAPON_RESTRICTIONS, {})})) return false;
    if (!expect(availability.value_for(9) == 1,
            "an empty body still resets retail's availability table")) return false;
    PlaySoundCommand positioned;
    positioned.flag = 1;
    positioned.has_pos = true;
    positioned.sound_name = "MEDIC_REQUEST";
    positioned.pos_x = -12;
    positioned.pos_y = 23;
    positioned.pos_z = 4;
    PlaySoundCommand listener;
    listener.sound_name = "MP_COMMAND1";
    const std::size_t before = h.kernel->world.out.slot_sounds.size();
    const std::size_t direct_before = h.kernel->world.out.script_sounds.size();
    if (!deliver({make_protocol_message(s2c::PLAY_SOUND, encode_play_sound(positioned)),
            make_protocol_message(s2c::PLAY_SOUND, encode_play_sound(listener))})) return false;
    if (!expect(h.kernel->world.out.slot_sounds.size() == before + 1,
            "positioned sound reaches the audio output exactly once")) return false;
    const auto &sound = h.kernel->world.out.slot_sounds.back();
    if (!expect(std::string(sound.set_name) == "MEDIC_REQUEST" &&
            sound.pos[0] == -12 * 65536 && sound.pos[1] == 23 * 65536 && sound.pos[2] == 4 * 65536,
            "wire sound coordinates are signed world units, converted to fixed point")) return false;
    if (!expect(h.kernel->world.out.script_sounds.size() == direct_before + 1 &&
            h.kernel->world.out.script_sounds.back().name == "MP_COMMAND1" &&
            h.kernel->world.out.script_sounds.back().kind == w::ScriptSoundEvent::Kind::Interface,
            "unpositioned sound reaches listener audio")) return false;
    h.role.run_tick(h.input);
    return expect(h.kernel->world.out.script_sounds.size() == direct_before + 1,
            "a received sound is not replayed on the next frame");
}


// Drive the production joiner frame: rendering can read a channel repeatedly
// without integrating another slew, and an NPC's own view follows its tether.
bool run_replica_turret_channel() {
 Harness h;
 h.role.poll_preload();
 // The replica's materialized world twin: an 'ewep' render class.
 auto &world = h.kernel->world;
 world.registry.configure_pool(1, 4);
 w::Entity twin;
 twin.kind = w::EntityKind::Item;
 twin.item_id = 123;
 twin.emplaced_ctrl_publisher = true;
 if (!expect(world.registry.spawn(1, twin).packed == 0x1000,
   "the world twin shares the wire handle")) return false;
 auto &state = h.role.runtime->state();
 mission::ItemSeatSpec spec;
 spec.type_id = 123;
 spec.item_attrib2 = def::DEF_ITEM_ATTRIB2_ISTURRET;
 w::Seat seat;
 seat.type = w::SeatType::Gunner;
 seat.bone_index = 1;
 spec.seats.push_back(seat);
 h.kernel->seat_specs.push_back(spec);
 auto &mount = state.upsert(0x1000);
 mount.type_id = 123;
 mount.cls = EntityClass::NoNetworkCallback;
 mount.heading_known = true;
 mount.heading_bam = 0;
 auto &gunner = state.upsert(2);
 gunner.cls = EntityClass::Player;
 gunner.carrier_handle = 0x1000;
 gunner.mount_bone = 1;
 gunner.heading_bam = 0x55555555;
 gunner.pitch_bam = 0x10000000;
 h.role.run_tick(h.input);
 auto *gun = state.find(0x1000);
 if (!expect(gun != nullptr && gun->emplaced_controls_valid &&
   gun->emplaced_gun_yaw_word == -147 && gun->emplaced_gun_pitch_word == -147,
   "remote turret advances one retail slew on each axis")) return false;
 if (!expect(state.find(2)->heading_bam == 0x55555555,
   "remote Player gunner skips local and NPC tethers")) return false;
 w::EmplacedWeaponControls first, second;
 inmatch::emplaced_weapon_controls_for_client(*gun, first);
 inmatch::emplaced_weapon_controls_for_client(*gun, second);
 if (!expect(first.gun_yaw == second.gun_yaw && gun->emplaced_gun_yaw_word == -147,
   "repeated presentation reads do not advance the turret")) return false;
 h.role.run_tick(h.input);
 if (!expect(state.find(0x1000)->emplaced_gun_yaw_word == -294,
   "second tick integrates from the stored high word")) return false;
 state.find(0x1000)->emplaced_gun_yaw_word = 0;
 state.find(2)->cls = EntityClass::Infantry;
 state.find(2)->heading_bam = 0x55555555;
 h.role.run_tick(h.input);
 if (!expect(state.find(2)->heading_bam == 0x2D7AD80,
   "NPC gunner yaw is written back at the four-degree tether")) return false;
 // Immediate gun, a stamped 45-degree yaw window (pitch +-90): word and
 // gunner look both pin on yaw.
 h.kernel->seat_specs[0].item_attrib2 = 0;
 h.kernel->seat_specs[0].turret_limits_valid = true;
 h.kernel->seat_specs[0].turret_yaw_range_bam = 0x20000000;
 h.kernel->seat_specs[0].turret_pitch_max_bam = 0x40000000;
 h.kernel->seat_specs[0].turret_pitch_min_bam = 0x40000000;
 state.find(2)->heading_bam = 0x55555555;
 state.find(2)->pitch_bam = 0;
 state.find(2)->recoil_pitch = 0x4000000;
 h.role.run_tick(h.input);
 if (!expect(state.find(0x1000)->emplaced_gun_yaw_word == -8192 &&
   (state.find(2)->heading_bam == 0x20400000 || state.find(2)->heading_bam == 0x1FC00000),
   "weapon window pins the barrel and writes back occupant look")) return false;
 if (!expect(state.find(0x1000)->emplaced_gun_pitch_word == -1024,
   "gun pitch consumes recoil before the body decay")) return false;
 state.find(2)->carrier_handle = 0xFFFF;
 h.role.run_tick(h.input);
 // The ewep writer has no occupant test: the dismounted turret keeps
 // publishing the words it was left at.
 // [orig: HUD_CacheWeaponSlotInfo @0x440930 via the 'ewep' render-class row
 //  @0x82CFA0]
 const auto *held = state.find(0x1000);
 if (!expect(held->emplaced_controls_valid && held->emplaced_gun_yaw_word == -8192 &&
   held->emplaced_gun_pitch_word == -1024,
   "a dismounted turret holds its last traverse")) return false;
 world.registry.get(w::EntityHandle{0x1000})->emplaced_ctrl_publisher = false;
 h.role.run_tick(h.input);
 return expect(!state.find(0x1000)->emplaced_controls_valid,
   "another render class publishes no EWEAP words");
}


bool run_local_replica_turret_channel() {
 Harness h;
 auto &world = h.kernel->world;
 world.registry.configure_pool(1,4);
 world.load_systems();
 h.role.poll_preload();
 constexpr uint16_t self_handle = 5;
 h.role.runtime->seed_session(kSessionId,kClientKey,kClientScrk,kServerScrk,
   1,0,self_handle,w::kPlayerInfantryTypeId);
 h.role.run_tick(h.input);
 auto *local = h.kernel->local.player();
 if (!expect(local != nullptr,"local turret fixture has L")) return false;
 // The mission start leaves L hidden; the seeded session has no 0x0F, whose landing
 // clears the bit [orig: NapiNPClientMsg_0x00F @0x42E200 (Flags &= ~1)].
 local->flags &= ~w::kEntityFlagCarried;
 w::Entity gun;
 gun.kind = w::EntityKind::Item;
 gun.has_item_def = true; // seats need the def (D-NET-422)
 gun.has_graphic_model = true;
 gun.item_id = 123;
 gun.spawn_origin = 1u << 24;
 gun.heading = w::placed_heading_bam(90);
 gun.item_attrib2 = def::DEF_ITEM_ATTRIB2_ISTURRET;
 gun.health = 100;
 gun.team = local->team;
 w::Seat seat;
 seat.type = w::SeatType::Gunner;
 seat.bone_index = 1;
 seat.source_name = "UseGun";
 gun.seats.push_back(seat);
 const auto handle = world.registry.spawn(1,gun);
 mission::ItemSeatSpec spec;
 spec.type_id = 123;
 spec.item_attrib2 = gun.item_attrib2;
 spec.seats = gun.seats;
 h.kernel->seat_specs.push_back(spec);
 auto &state = h.role.runtime->state();
 auto &row = state.upsert(handle.packed);
 row.type_id = 123;
 row.cls = EntityClass::NoNetworkCallback;
 row.heading_known = true;
 row.heading_bam = 0;
 auto &self = state.upsert(self_handle);
 self.cls = EntityClass::Player;
 self.carrier_handle = handle.packed;
 self.mount_bone = 1;
 self.state_flags = w::kEntityFlagMounted;
 self.state_flags_known = true;
 h.role.run_tick(h.input);
 local = h.kernel->local.player();
 if (!expect(local->mounted && local->mount_type == w::SeatType::Gunner,
   "confirmed UseGun mounts local L")) return false;
 world.registry.get(handle)->emplaced_gun_yaw_word = 0;
 world.registry.get(handle)->emplaced_spin_phase = 0xFEDC;
 h.kernel->local.input.look_heading = 0x55555555;
 h.role.run_tick(h.input);
 const auto *mount = state.find(handle.packed);
 if (!expect(mount && mount->emplaced_gun_yaw_word == -147 &&
   world.registry.get(handle)->emplaced_gun_yaw_word == -147,
   "local joiner publishes exactly one slew from the world body")) return false;
 w::EmplacedWeaponControls controls;
 if (!expect(inmatch::emplaced_weapon_controls_for_client(*mount, controls) &&
   controls.spin == 0xFEDC,
   "local joiner forwards the world barrel phase without a wire field")) return false;
 return expect(h.kernel->local.input.look_heading == 0x3FFF7FC0,
   "local joiner input is tethered within ninety degrees of the gun");
}

// Every in-match pump stamps World::rules from the joiner's session: the
// session opens, and auto_scope_zero follows bit 0x10000 of the S2C
// 0x64 +44 mpattrib word (g_RulesFlags @0x24D1E34, the
// Player_AdjustWeaponZoomLevel test @0x4dbd15). The runtime view carries
// that word once the 0x64 transfer lands; here it is set on the view.
bool run_rules_stamp_from_mp_attributes(uint32_t mp_attributes, bool zoom_allowed) {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.runtime->view().set_mp_attributes(mp_attributes);
	w::World &world = h.kernel->world;
	if (!expect(!world.rules.auto_scope_zero,
			"rules stamp: a bare kernel starts without the zero rule")) return false;
	// The mission data's copy reaches the world at the mission start's load (bring_up), once
	// [orig: NapiNPClientMsg_HandleMissionDataChunk @0x432410 - the memcpy @0x432470].
	(void)h.role.bring_up();
	if (!expect(world.rules.auto_scope_zero == zoom_allowed && world.rules.mpattrib == mp_attributes,
			"rules stamp: the mission start takes the word; auto_scope_zero follows bit 0x10000"))
		return false;
	// An admin SET of the word holds: no frame overwrites it until the next mission start
	// [orig: CAdminServer_HandleSetCommand @0x405C80..0x40618A writes g_RulesFlags @0x24D1E34].
	world.rules.mpattrib = 0x200u;
	h.role.run_tick(h.input);
	h.role.run_tick(h.input);
	return expect(world.rules.mpattrib == 0x200u, "rules stamp: a SET word holds through the frames");
}

// A folded S2C 0x1D latches the joiner's round-over gate, so the frame that
// folds it holds its entity update, as retail's client does.
// [orig: NapiNPClientMsg_0x01D @0x430858 (`mov g_RoundOver,1`);
//  Game_ProcessMainFrame -- the is_in_session / g_RoundOver tests
//  @0x526734..0x526742 ahead of the Entity_UpdateAllEntities call @0x52674B]
bool run_end_round_header_holds_the_entity_update() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	w::World &world = h.kernel->world;
	world.rules.mp_session = true; // the joiner kernel boot's stamp
	h.role.run_tick(h.input);
	const uint32_t counted = world.entity_update_counter;
	h.role.run_tick(h.input);
	if (!expect(world.entity_update_counter == counted + 1,
			"end round: the round's frames run the entity update")) return false;
	if (!expect(!world.match.outcome().ended, "end round: no latch before the header"))
		return false;
	h.role.runtime->view().set_game_type(0x10000u);
	EndRoundHeader header;
	header.winner_team = 1;
	h.role.runtime->view().apply(s2c::END_ROUND_HEADER,
			encode_end_round_header(header, /*non_team_form=*/false));
	if (!expect(h.role.runtime->state().end_round.header_known,
			"end round: the header folded")) return false;
	h.role.run_tick(h.input);
	if (!expect(world.match.outcome().ended, "end round: the header latches the round over"))
		return false;
	return expect(world.entity_update_counter == counted + 1,
			"end round: the latched frame holds the entity update");
}

// The frame input packs onto L BEFORE the client net frame builds the C2S
// 0x0C: a key held this frame rides THIS frame's uplink, not the next one
// [orig: Client_ProcessNetworkFrame @0x42C180 — Player_PackInputStateToEntity
//  @0x42C3E9 precedes Player_BuildTag0CInputBody @0x42C46F].
bool run_uplink_carries_same_frame_input() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input); // the spawn frame: L exists, no key held
	if (!expect(h.role.local_spawned(), "same-frame input: L spawned")) return false;
	h.kernel->local.set_movement_keys(true, false, false, false, false, false, false);
	h.role.run_tick(h.input);
	ProtocolMessage movement;
	EntityPacketSubHeader sub;
	PlayerExtendedUplink uplink;
	size_t header_bytes = 0, body_bytes = 0;
	if (!expect(h.socket.last_message(0x0C, movement) &&
					decode_entity_packet_sub_header(movement.payload.data(),
							movement.payload.size(), sub, header_bytes) &&
					decode_player_extended_uplink(movement.payload.data() + header_bytes,
							movement.payload.size() - header_bytes, uplink, body_bytes),
			"same-frame input: the frame shipped a decodable 0x0C"))
		return false;
	return expect((uplink.move_input_byte & w::Entity::kMoveOrderMoving) != 0,
			"the forward key held THIS frame rides this frame's uplink");
}

// The session hands the main loop's frame statistics to the role ahead of the
// drain; the next C2S 0x0C carries their low bytes. [orig: Game_MainLoop
// @0x52B948 / @0x52B98F ahead of the drain @0x52BA08; NetPacket_SerializePlayerState
// case 3 @0x4C1BA2 / @0x4C1BBC]
bool run_uplink_carries_the_frame_statistics() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input); // the spawn frame
	if (!expect(h.role.local_spawned(), "frame statistics: L spawned")) return false;
	h.role.observe_frame_rate(0x13A);
	h.role.observe_cpu_share(37);
	h.role.run_tick(h.input);
	ProtocolMessage message;
	EntityPacketSubHeader sub;
	PlayerExtendedUplink uplink;
	size_t header_bytes = 0, body_bytes = 0;
	if (!expect(h.socket.last_message(0x0C, message) &&
					decode_entity_packet_sub_header(message.payload.data(),
							message.payload.size(), sub, header_bytes) &&
					decode_player_extended_uplink(message.payload.data() + header_bytes,
							message.payload.size() - header_bytes, uplink, body_bytes),
			"frame statistics: the frame shipped a decodable 0x0C"))
		return false;
	return expect(uplink.stat_byte_0 == 0x3A && uplink.stat_byte_1 == 37,
			"the 0x0C stat bytes carry the frame rate and CPU share low bytes");
}

// The uplink's own dead test is the builder's: a Health word of 0 or the dead
// bit holds the C2S 0x0C; the alive latch alone does not, and a negative Health
// word with the bit clear still sends (D-NET-433).
// [orig: Player_BuildTag0CInputBody @0x42A550 - `cmp word [eax+11Eh], 0; jz`
//  @0x42A58F..0x42A597, `test byte [eax+24h], 2; jnz` @0x42A599..0x42A59D]
bool run_uplink_dead_test_is_the_builders() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input); // the spawn frame
	if (!expect(h.role.local_spawned(), "uplink dead test: L spawned")) return false;
	ProtocolMessage message;
	auto sends = [&](int32_t health, bool alive, bool dead_bit) {
		w::Entity *local = h.kernel->world.registry.get(h.kernel->world.cached.local_player);
		local->health = health;
		local->alive = alive;
		local->flags = dead_bit ? (local->flags | 2u) : (local->flags & ~2u);
		h.socket.datagrams.clear();
		h.role.run_tick(h.input);
		return h.socket.last_message(0x0C, message);
	};
	if (!expect(sends(100, false, false), "the alive latch alone does not hold the uplink"))
		return false;
	if (!expect(!sends(100, true, true), "the dead bit holds the uplink")) return false;
	if (!expect(sends(-5, true, false), "a negative Health word with the bit clear sends"))
		return false;
	return expect(!sends(0, true, false), "a Health word of 0 holds the uplink");
}

// A joiner's stance key only sends the C2S 0x1D (every press, the current
// stance included); the latch follows the authority's 0x0A tail echo, which
// re-latches it on every frame that carries the tail. [orig:
// Input_HandleActionBinding_0 cases 169/170/172 @0x4e0d77..0x4e0e87 (no latch
// write); NapiNPClientMsg_0x00A @0x4303e5 -> @0x430562..0x43058f]
bool run_stance_follows_the_authority_echo() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	if (!expect(h.role.local_spawned(), "stance echo: L spawned")) return false;
	if (!expect(h.role.request_stance(1), "stance echo: the crouch press sends")) return false;
	h.role.run_tick(h.input);
	ProtocolMessage select;
	if (!expect(h.socket.last_message(0x1D, select) && select.payload.size() == 2 &&
					select.payload[0] == 0xA9 && select.payload[1] == 0x00,
			"stance echo: the C2S 0x1D carries action 169"))
		return false;
	if (!expect(h.kernel->local.stance_latch() == 0,
			"stance echo: the press does not latch before the echo"))
		return false;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	const auto deliver = [&](uint8_t state_byte) {
		FrameUpdate fu;
		fu.local_tail_present = true;
		fu.state_flag_byte = state_byte;
		fu.health = 150;
		fu.complete = true;
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
	};
	deliver(0x02); // the authority's copy: crouched
	const w::AiEntity *body = h.kernel->local.player_ai();
	if (!expect(h.kernel->local.stance_latch() == 1 && body != nullptr &&
					body->inf.stance == w::InfantryState::Stance::kCrouch,
			"stance echo: the 0x0A tail latches crouch"))
		return false;
	// A second press of the same key still sends.
	const size_t before = h.socket.datagrams.size();
	if (!expect(h.role.request_stance(1), "stance echo: a repeat press sends again"))
		return false;
	h.role.run_tick(h.input);
	if (!expect(h.socket.datagrams.size() > before && h.socket.last_message(0x1D, select),
			"stance echo: the repeat 0x1D left"))
		return false;
	deliver(0x00); // the authority stood the player up
	return expect(h.kernel->local.stance_latch() == 0 &&
					body->inf.stance == w::InfantryState::Stance::kStand,
			"stance echo: the next tail re-latches stand");
}

// Every C2S 0x0C body the role shipped from datagram `from` on, decoded.
std::vector<PlayerExtendedUplink> uplinks_since(const CountingSocket &socket, std::size_t from) {
	std::vector<PlayerExtendedUplink> out;
	for (std::size_t i = from; i < socket.datagrams.size(); ++i) {
		const std::vector<uint8_t> &packet = socket.datagrams[i];
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		ProtocolPacketHeader header;
		std::vector<ProtocolMessage> messages;
		if (!nw_decode_inbound(packet.data(), packet.size(), opcode, body) ||
				opcode != SESSION_OPCODE_PROTOCOL_MESSAGE ||
				!decode_protocol_packet_plaintext(body.data(), body.size(), kClientScrk,
						header, messages)) continue;
		for (const ProtocolMessage &message : messages) {
			if (message.tag != 0x0C) continue;
			EntityPacketSubHeader sub;
			PlayerExtendedUplink uplink;
			size_t header_bytes = 0, body_bytes = 0;
			if (decode_entity_packet_sub_header(message.payload.data(), message.payload.size(),
						sub, header_bytes) &&
					decode_player_extended_uplink(message.payload.data() + header_bytes,
						message.payload.size() - header_bytes, uplink, body_bytes))
				out.push_back(uplink);
		}
	}
	return out;
}

// Under a dictated send holdoff the input pack rides the uplink's send block:
// the keys held on any frame of a 12-tick window fold into the input-flag word
// and pack once, at the boundary. A jump, a lean and a forward tap between two
// boundaries therefore all reach the boundary 0x0C, and only that one: the next
// window's pack clears what the last one reported. The local MoveOrder is the
// same packed word, so L's own body sees the taps at the boundary too and keeps
// that word until the next pack.
// [orig: Input_ProcessFrame @0x49d541; Client_ProcessNetworkFrame @0x42C3DD ->
//  Player_PackInputStateToEntity @0x42C3E9 (the clear @0x4DF904/@0x4DF909) ->
//  Player_BuildTag0CInputBody @0x42C482]
bool run_holdoff_window_taps_reach_the_boundary_uplink() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input); // the spawn frame
	if (!expect(h.role.local_spawned(), "holdoff taps: L spawned")) return false;
	// The host dictates the NovaWorld period: CS dir-0 field 3 = 12.
	SessionSequencing server_tx = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> framed;
	if (!expect(frame_session_packet(server_tx, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(0x00, {0x01, 0x08, 0x00, 0x00, 0x00, 12, 0x00, 0x00, 0x00},
						0xA0)}, framed), "holdoff taps: the CS update frames")) return false;
	const std::vector<uint8_t> settings =
			nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(framed));
	h.role.runtime->receive(settings.data(), settings.size());

	const auto local_move = [&h]() -> uint8_t {
		const w::Entity *local = h.kernel->local.player();
		return local != nullptr ? local->net_move_input : 0xFFu;
	};
	struct Keys { bool forward, lean_left, jump; };
	// One frame: the keys, the role tick, and the 0x0C bodies it shipped.
	const auto frame = [&h](Keys k) {
		h.kernel->local.set_movement_keys(k.forward, false, false, false, k.lean_left, false,
				k.jump);
		const std::size_t before = h.socket.datagrams.size();
		h.role.run_tick(h.input);
		return uplinks_since(h.socket, before);
	};
	constexpr Keys kNone{false, false, false};
	// B0: the reset counter keeps this first boundary open; it then re-arms
	// from the dictated period.
	std::vector<PlayerExtendedUplink> shipped = frame(kNone);
	if (!expect(shipped.size() == 1 && h.role.runtime->send_holdoff_ticks() == 12,
			"holdoff taps: the first boundary ships one 0x0C under a 12-tick period"))
		return false;
	if (!expect(shipped[0].move_input_byte == 0, "holdoff taps: B0 carries no input"))
		return false;
	// Window 1: jump held on ticks 2..4, lean-left on 6..7, forward on 9 only.
	for (int tick = 1; tick <= 11; ++tick) {
		const Keys keys{tick == 9, tick == 6 || tick == 7, tick >= 2 && tick <= 4};
		shipped = frame(keys);
		if (!expect(shipped.empty(), "holdoff taps: no 0x0C between boundaries")) {
			std::fprintf(stderr, "  window tick %d shipped %zu\n", tick, shipped.size());
			return false;
		}
		if (!expect(local_move() == 0,
				"holdoff taps: the local MoveOrder waits for the boundary pack")) {
			std::fprintf(stderr, "  window tick %d move %02x\n", tick, local_move());
			return false;
		}
	}
	// B1 (tick 12): no key is held on the boundary frame itself.
	shipped = frame(kNone);
	if (!expect(shipped.size() == 1, "holdoff taps: B1 ships one 0x0C")) return false;
	std::printf("holdoff taps: B1 move=%02x local=%02x\n", shipped[0].move_input_byte,
			local_move());
	if (!expect((shipped[0].move_input_byte & w::Entity::kMoveOrderJump) != 0,
			"holdoff taps: the jump tapped on window ticks 2..4 rides the B1 0x0C"))
		return false;
	if (!expect((shipped[0].move_input_byte & w::Entity::kMoveOrderLeanLeft) != 0,
			"holdoff taps: the lean tapped on window ticks 6..7 rides the B1 0x0C"))
		return false;
	if (!expect((shipped[0].move_input_byte & 0x0Fu) == w::Entity::kMoveOrderMoving,
			"holdoff taps: the forward tap on window tick 9 rides B1 as moving, dir 0"))
		return false;
	if (!expect(local_move() == shipped[0].move_input_byte,
			"holdoff taps: L's own MoveOrder is the packed boundary word"))
		return false;
	const uint8_t packed = shipped[0].move_input_byte;
	// Window 2: nothing held. L keeps the B1 word until the next pack.
	for (int tick = 1; tick <= 11; ++tick) {
		shipped = frame(kNone);
		if (!expect(shipped.empty() && local_move() == packed,
				"holdoff taps: the packed word persists through the next window")) {
			std::fprintf(stderr, "  window 2 tick %d shipped %zu move %02x\n", tick,
					shipped.size(), local_move());
			return false;
		}
	}
	shipped = frame(kNone);
	if (!expect(shipped.size() == 1, "holdoff taps: B2 ships one 0x0C")) return false;
	if (!expect((shipped[0].move_input_byte &
						(w::Entity::kMoveOrderJump | w::Entity::kMoveOrderLeanLeft |
								w::Entity::kMoveOrderMoving)) == 0,
			"holdoff taps: B2 no longer carries the window-1 taps"))
		return false;
	return expect(local_move() == shipped[0].move_input_byte,
			"holdoff taps: L's MoveOrder follows the B2 pack");
}

// S2C 0x0F re-snaps L to the authoritative pose it carries (position, yaw,
// pitch, roll and the look yaw) once per decoded 0x0F, and the un-hide runs
// while no death screen is up [orig: NapiNPClientMsg_0x00F @0x42E200 — the
//  pose stores (Pitch @0x42E3E9, Roll @0x42E3F2), `Flags &= ~1`].
bool run_world_state_load_resnaps_local_pose() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr, "0x0F re-snap: L spawned")) return false;
	local->hidden = true;
	// 23-byte header [u32 tick][i32 x][i32 y][i32 z][i16 yaw][i16 pitch][i16 roll][u8 flags],
	// the 128 pool dwords, zero waypoint/location counts.
	std::vector<uint8_t> body(23 + kWorldStateAmmoPoolCount * 4 + 4, 0);
	const auto put_i32 = [&body](size_t at, int32_t v) {
		body[at] = uint8_t(v);
		body[at + 1] = uint8_t(v >> 8);
		body[at + 2] = uint8_t(v >> 16);
		body[at + 3] = uint8_t(v >> 24);
	};
	put_i32(4, 100 << 16);
	put_i32(8, 200 << 16);
	put_i32(12, 10 << 16);
	body[16] = 0x00; body[17] = 0x40; // yaw high word 0x4000 -> BAM 0x40000000
	body[18] = 0x00; body[19] = 0x08; // pitch 0x0800 -> 0x08000000
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::WORLD_STATE_LOAD, body)}, packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input);
	local = h.kernel->local.player();
	const w::AiEntity *local_ai = h.kernel->local.player_ai();
	if (!expect(local != nullptr && local_ai != nullptr, "0x0F re-snap: L still resolvable"))
		return false;
	const bool posed = std::fabs(local->position.x - 100.0f) < 0.01f &&
			std::fabs(local->position.y - 200.0f) < 0.01f &&
			std::fabs(local->position.z - 10.0f) < 0.01f;
	if (!expect(posed, "0x0F re-snap: L sits at the wire position")) return false;
	if (!expect(local_ai->heading == 0x40000000 && local_ai->pitch == 0x08000000 &&
					h.kernel->local.input.look_heading == 0x40000000,
			"0x0F re-snap: the body heading, pitch and the look yaw follow the wire"))
		return false;
	return expect(!local->hidden, "0x0F re-snap: the un-hide ran with no death screen up");
}

// A medic revive's deploy sends no 0x5A (D-NET-379): L comes back on its own
// record's respawn edge, facing the record's heading byte, through the same
// respawn reset as any deploy, with the kit it died with (no grant folded).
// [orig: NetPacket_SerializePlayerState @0x4C1109..0x4C114C -> Game_InitNewRound;
//  Server_DeployPlayer @0x5178C5]
bool run_revive_deploy_respawns_l_on_its_record_edge() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	auto &self_row = h.role.runtime->state().upsert(0x0005);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	auto frame = [&](int16_t health, uint8_t state_flags) {
		FrameUpdate fu;
		fu.carried_handle = 0xFFFF;
		fu.health = health;
		FrameUpdateRecord rec;
		rec.handle = 0x0005;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.yaw_byte = 0xCC;
		rec.player.state_flags = state_flags;
		fu.records.push_back(rec);
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
	};
	frame(100, 0x00);
	frame(0, w::kEntityFlagDead);
	const w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr && !local->alive && h.role.runtime->deployment_pick_pending(),
			"revive: the death frame downs L and opens the deploy flow"))
		return false;
	if (!expect(h.role.runtime->queue_deployment_pick(0xFFFF), "revive: the Default Spawn pick"))
		return false;
	h.role.run_tick(h.input);
	const uint64_t grants = h.role.runtime->authoritative_loadout_revision();
	h.seams.clear();
	// The respawn reset returns the map zoom to its spawn default (the round
	// init leaves the zooms alone), so a moved zoom marks it.
	h.kernel->local.hud_map_control.zoom_q16 = 1;
	frame(100, 0x00); // the revive deploy: no 0x5A, the record alive again
	frame(100, 0x00); // the next tail revives L
	local = h.kernel->local.player();
	const w::AiEntity *local_ai = h.kernel->local.player_ai();
	if (!expect(local != nullptr && local_ai != nullptr && local->alive && local->health == 100 &&
					!h.role.runtime->deployment_pick_pending(),
			"revive: the record's respawn edge releases the deploy and L stands"))
		return false;
	if (!expect(h.kernel->local.hud_map_control.zoom_q16 ==
					h.kernel->local.hud_map_control.spawn_zoom_q16,
			"revive: L runs the respawn reset"))
		return false;
	if (!expect(local_ai->heading == static_cast<int32_t>(0xCC000000u),
			"revive: L faces the record's heading byte"))
		return false;
	return expect(h.role.runtime->authoritative_loadout_revision() == grants,
			"revive: no grant folds, so L keeps the kit it died with");
}

// The 0x0A tail's last word is the host's copy of L's +0x120 (Armor; ammo class
// 1's carried pool): the joiner stores it on L every frame that carries the
// tail, alive or dead (B-4).
// [orig: NapiNPClientMsg_0x00A read @0x430442, store @0x4305EB]
bool run_tail_armor_lands_on_l() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	auto &self_row = h.role.runtime->state().upsert(0x0005);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	auto frame = [&](int16_t health, int16_t armor, uint8_t state_flags) {
		FrameUpdate fu;
		fu.carried_handle = 0xFFFF;
		fu.health = health;
		fu.armor = armor;
		FrameUpdateRecord rec;
		rec.handle = 0x0005;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.state_flags = state_flags;
		fu.records.push_back(rec);
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
	};
	frame(100, 33, 0x00);
	const w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr && local->mana == 33, "armor: the tail's word lands on L"))
		return false;
	frame(100, -1, 0x00);
	if (!expect(local->mana == -1, "armor: a negative word lands raw")) return false;
	frame(0, 7, w::kEntityFlagDead);
	return expect(!local->alive && local->mana == 7, "armor: a dead L still takes the word");
}

// D-NET-728: the 0x0A tail's carried word lands on L: a word naming an entity
// with a def that L does not carry attaches it (L's +0x268, the object's +0x170
// and its hidden bit); 0xFFFF drops what L carries through the drop's own legs
// (the fall installed, the hidden bit cleared); a word naming no def'd entity
// reads as none.
// [orig: NapiNPClientMsg_0x00A @0x4305F2..0x43069A; Entity_AttachCarriedObject
//  @0x43C130; Entity_DropCarriedObject @0x439DF0]
bool run_tail_carried_word_lands_on_l() {
	constexpr uint16_t kFlagType = 4091;
	Harness h;
	w::World &world = h.kernel->world;
	h.kernel->wire_header_world = true;
	world.registry.configure_pool(1, 16);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	def::DefItemDef defs[2]{};
	defs[0].id = w::kPlayerInfantryTypeId + mission::kItemIdOffset;
	defs[0].type = 3;
	defs[0].hp = 100;
	defs[1].id = kFlagType + mission::kItemIdOffset;
	def::DefItemsFile items{defs, 2};
	h.kernel->set_items_table(&items);
	const w::EntityHandle flag_h{0x1003};
	PoolSpawnBatch batch;
	PoolSpawnRecord spawn;
	spawn.slot_id = flag_h.packed;
	spawn.item_type_id = kFlagType;
	spawn.pos_x = 10 << 16;
	spawn.pos_y = -20 << 16;
	batch.records.push_back(spawn);
	h.role.runtime->view().apply(s2c::POOL_SPAWN, encode_pool_spawn_batch(batch));
	auto &self_row = h.role.runtime->state().upsert(0x0005);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	auto frame = [&](uint16_t carried) {
		FrameUpdate fu;
		fu.carried_handle = carried;
		fu.health = 100;
		FrameUpdateRecord rec;
		rec.handle = 0x0005;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		fu.records.push_back(rec);
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
	};
	frame(0xFFFF);
	// The embedder's trait sweep over the materialized row: the flag takes its def.
	h.kernel->resolve_item_traits([](int) { return uint8_t{0}; });
	w::Entity *local = h.kernel->local.player();
	w::Entity *flag = world.registry.get(flag_h);
	if (!expect(local != nullptr && flag != nullptr && flag->has_item_def &&
					!local->mounted_child.valid(),
			"carried: L carries nothing on 0xFFFF"))
		return false;
	frame(flag_h.packed);
	if (!expect(local->mounted_child == flag_h && flag->primary_occupant == local->handle &&
					(flag->flags & w::kEntityFlagCarried) != 0,
			"carried: the named flag attaches to L"))
		return false;
	frame(flag_h.packed);
	if (!expect(local->mounted_child == flag_h, "carried: the same word changes nothing"))
		return false;
	frame(0xFFFF);
	if (!expect(!local->mounted_child.valid() && !flag->primary_occupant.valid() &&
					(flag->flags & w::kEntityFlagCarried) == 0 &&
					flag->drop_motion == w::DropMotion::Fall,
			"carried: 0xFFFF drops it through the drop's legs"))
		return false;
	frame(0x1009); // a slot no row holds
	return expect(!local->mounted_child.valid(), "carried: a word naming no def'd entity is none");
}

// D-NET-143 (L4-s9 FOUND 3): every S2C 0x5A the joiner applies rebuilds the process's
// loadout name list from the list's rows, and the joiner's world reads that list for the
// equipped default of every body it spawns (the image's initializer, WPN_M4AUTO first,
// until a list arrives).
// [orig: NapiNPClientMsg_HandleWeaponLoadoutSync @0x4290E0 - the memset @0x42920C, the rows
//  @0x429230..0x4293B9; PlayerClass_InitEntity @0x4B1060 (@0x4B1100..0x4B1116)]
bool run_a_weapon_loadout_rebuilds_the_process_name_list() {
	Harness h;
	for (const char *name : {"WPN_KNIFE", "WPN_M4AUTO", "WPN_AK47AUTO"}) {
		w::WeaponTableEntry def;
		def.name = name;
		def.valid = true;
		h.kernel->world.tables.weapons.entries.push_back(def);
	}
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	bool ok = expect(h.kernel->world.loadout_weapon_names == &h.role.runtime->loadout_weapon_names(),
			"name list: the joiner's world reads the process's list");
	ok &= expect(w::player_spawn_equipped_default(h.kernel->world) == 1,
			"name list: the initializer's WPN_M4AUTO before any list");
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	WeaponLoadout list;
	list.avatar_class = 6;
	WeaponLoadoutSlot ak;
	ak.type_id = 2;
	ak.ammo_primary = 3;
	WeaponLoadoutSlot m4;
	m4.type_id = 1;
	list.slots = {ak, m4};
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::WEAPON_LOADOUT, encode_weapon_loadout(list))}, packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input);
	const w::LoadoutWeaponNameList &names = h.role.runtime->loadout_weapon_names();
	ok &= expect(names.set && names.entries.size() == 2 && names.entries[0].name == "WPN_AK47AUTO" &&
					names.entries[1].name == "WPN_M4AUTO",
			"name list: the 0x5A's rows, in order");
	ok &= expect(w::player_spawn_equipped_default(h.kernel->world) == 2,
			"name list: a body spawned now takes the list's first weapon");
	return ok;
}

// D-NET-348 (V-L4-s10 c): the joiner's person pass keys a body's class event on its def's
// ai_function, as retail looks ONE class row up by that tag ("Null" for an empty one) and
// takes the event (+0x138) and the serializer (+0x164) from it together: a plyr row takes
// the plyr arm (its +0x2AC = 64), an org row the organic arm (no store), and a brrl row's
// Player-bit body classifies as no person at all, so the pass never reaches it.
// [orig: EntityDef_InitAllCallbacks @0x4A5A70 (the tag push @0x4A5AA6, "Null" @0x4A5AA9,
//  the call @0x4A5AAE);
//  EntityDef_LookupClassCallbacks @0x407DC0; Projectile_ProcessDamageOnTarget @0x4E7FB0
//  (the +0x1C8 call @0x4E81FF..0x4E820E)]
bool run_the_person_pass_keys_the_class_row() {
	Harness h;
	def::DefItemDef defs[3]{};
	const char *tags[3] = {"plyr", "org0", "brrl"};
	for (int i = 0; i < 3; ++i) {
		defs[i].id = 0x2000 + i + mission::kItemIdOffset;
		std::strncpy(defs[i].ai_function, tags[i], sizeof(defs[i].ai_function) - 1);
	}
	def::DefItemsFile items{defs, 3};
	h.kernel->set_items_table(&items);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::World &world = h.kernel->world;
	bool ok = expect(static_cast<bool>(world.round_sim.wire_person_body),
			"person pass: the joiner binds the wire person view");
	// The rows classify as the joiner's spawn stream types them: through the item
	// catalog the shell installs on the view (Simulation's item_replication_catalog),
	// by the def's ai_function.
	const auto catalog = std::make_shared<const replication::ItemReplicationCatalog>(
			replication::ItemReplicationCatalog::from_items_def(items));
	h.role.runtime->view().set_item_catalog(catalog);
	for (int i = 0; i < 3; ++i) {
		replication::ClientEntityState &row = h.role.runtime->state().upsert(
				static_cast<uint16_t>(0x0010 + i));
		row.type_id = static_cast<uint16_t>(0x2000 + i);
		const std::optional<EntityClass> cls = catalog->resolve_wire_entity_class(row.type_id);
		ok &= expect(cls.has_value(), "person pass: the catalog types every row");
		row.cls = cls.value_or(EntityClass::Unknown);
		row.rm_entity_flags = w::kEntityFlagPlayer;
	}
	ok &= expect(h.role.runtime->state().find(0x0012)->cls == EntityClass::NoNetworkCallback,
			"person pass: a brrl row is no person row");
	bool plyr_visited = false, plyr_class = false;
	ok &= expect(world.round_sim.wire_person_body(0x0010, [&](w::WirePersonBody &b) {
		plyr_visited = true;
		plyr_class = b.plyr_class;
	}) && plyr_visited && plyr_class, "person pass: a plyr row takes the plyr arm");
	bool org_visited = false, org_plyr = true;
	ok &= expect(world.round_sim.wire_person_body(0x0011, [&](w::WirePersonBody &b) {
		org_visited = true;
		org_plyr = b.plyr_class;
	}) && org_visited && !org_plyr, "person pass: an org row's Player-bit body takes the organic arm");
	bool brrl_visited = false;
	ok &= expect(!world.round_sim.wire_person_body(0x0012, [&](w::WirePersonBody &) {
		brrl_visited = true;
	}) && !brrl_visited, "person pass: a brrl row's Player-bit body is never visited (no 64)");
	h.kernel->set_items_table(nullptr);
	return ok;
}

// D-NET-NEW-L2-15's wire face (V-L2-s6's test gap): a seated L killed by a pump that carries a
// zero 0x0A tail and the S2C 0x13 naming L. The tail stores Health alone, so the 0x13's plyr
// callback runs past its dead return into the seat leave, which on a client is
// Entity_SendDetachPacket: one reliable C2S 0x27 [self][the seat's carrier][0].
// [orig: NapiNPClientMsg_0x00A @0x42FEC0 - the Health store @0x4305DF;
//  Entity_HandleDamageAndTriggerZones @0x407720 - the dead return @0x40772A..0x40772F, the
//  parent @0x407B0B, the leave @0x407B0D..0x407B33; Entity_SendDetachPacket @0x435510 - the
//  reliable 0x27 @0x435596..0x4355A2]
bool run_a_seated_death_sends_the_seat_leave() {
	Harness h;
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	world.load_systems();
	h.role.poll_preload();
	constexpr uint16_t self_handle = 0x0005;
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, self_handle, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr, "seated death: L spawned")) return false;
	w::Entity vehicle;
	vehicle.kind = w::EntityKind::Item;
	vehicle.has_item_def = true;
	vehicle.has_graphic_model = true;
	vehicle.position = local->position;
	vehicle.position.x += 1.0f;
	vehicle.health = vehicle.health_max = 100;
	vehicle.team = local->team;
	w::Seat seat;
	seat.type = w::SeatType::Passenger;
	seat.bone_index = 2;
	seat.retail_slot = 0;
	seat.source_name = "sitex00";
	vehicle.seats.push_back(seat);
	const auto vh = world.registry.spawn(1, vehicle);
	auto &self = h.role.runtime->state().upsert(self_handle);
	self.cls = EntityClass::Player;
	self.type_id = w::kPlayerInfantryTypeId;
	self.carrier_handle = vh.packed;
	self.mount_bone = 2;
	self.state_flags = w::kEntityFlagMounted;
	self.state_flags_known = true;
	h.role.run_tick(h.input);
	local = h.kernel->local.player();
	if (!expect(local != nullptr && local->mounted && local->mount_target == vh,
			"seated death: the host's echo seats L")) return false;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	FrameUpdate dead_tail;
	dead_tail.carried_handle = 0xFFFF;
	dead_tail.health = 0;
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(dead_tail)),
					make_protocol_message(s2c::ENTITY_DEATH,
							std::vector<uint8_t>{0x05, 0x00, 0x2A, 0x00})},
			packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.socket.datagrams.clear();
	h.role.runtime->receive(datagram.data(), datagram.size());
	for (int i = 0; i < 12; ++i) h.role.run_tick(h.input);
	ProtocolMessage leave;
	return expect(h.socket.last_message(0x27, leave) && leave.payload.size() == 6 &&
					leave.payload[0] == 0x05 && leave.payload[1] == 0x00 &&
					leave.payload[2] == static_cast<uint8_t>(vh.packed) &&
					leave.payload[3] == static_cast<uint8_t>(vh.packed >> 8),
			"seated death: the 0x13's plyr callback sends the seat leave's C2S 0x27");
}

// D-NET-588: an S2C 0x13 naming the joiner's own player lands on L: Health 0,
// the word as L's deathAnimStateId and the plyr class callback with event 4
// (its think re-arm to 64), as retail's handler runs on the slot entity at the
// handle, whatever its class.
// [orig: NapiNPClientMsg_EntityDeath @0x42EB50 - Health = 0 @0x42EBD6, +0x2C0
//  @0x42EBDF, cb(entity, 4, 0) @0x42EBF5; Entity_HandleDamageAndTriggerZones
//  @0x407B5E]
bool run_entity_death_names_l() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	auto &self_row = h.role.runtime->state().upsert(0x0005);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	auto send = [&](std::vector<ProtocolMessage> messages) {
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey}, messages, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
	};
	FrameUpdate fu;
	fu.carried_handle = 0xFFFF;
	fu.health = 100;
	FrameUpdateRecord rec;
	rec.handle = 0x0005;
	rec.type_id = w::kPlayerInfantryTypeId;
	rec.cls = EntityClass::Player;
	rec.player.carrier_handle = 0xFFFF;
	fu.records.push_back(rec);
	send({make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))});
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr && local->health == 100 && local->alive,
			"0x13 on L: L stands alive at 100"))
		return false;
	local->spawn_phase = 0;
	send({make_protocol_message(s2c::ENTITY_DEATH, std::vector<uint8_t>{0x05, 0x00, 0x2A, 0x00})});
	// The same frame's tick then runs L's body: its death edge takes the Health
	// the 0x13 zeroed, and the think the callback re-armed to 64 steps once.
	if (!expect(local->health == 0 && (local->flags & w::kEntityFlagDead) != 0 &&
					local->spawn_phase == 63,
			"0x13 on L: Health 0 kills L, and the plyr callback re-armed its think"))
		return false;
	{
		// L5-s3 FOUND 1: a zero tail and a 0x13 in one pump. The tail stores Health
		// alone; the dead bit is the body's death edge in the entity update after the
		// pump, so the 0x13's callback still runs past its dead return (the re-arm to
		// 64, and on a seated L the seat leave's C2S 0x27)
		// [orig: NapiNPClientMsg_0x00A @0x42FEC0 - the Health store @0x4305DF;
		//  Entity_HandleDamageAndTriggerZones @0x407720 (the dead return @0x40772A..0x40772F);
		//  Entity_UpdateInfantryPlayerBody
		//  @0x4B40E0 - the death edge @0x4B4BF1..0x4B4C02].
		Harness h2;
		h2.role.poll_preload();
		h2.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
				1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
		h2.role.run_tick(h2.input);
		auto &row2 = h2.role.runtime->state().upsert(0x0005);
		row2.cls = EntityClass::Player;
		row2.type_id = w::kPlayerInfantryTypeId;
		SessionSequencing seq2 = inmatch::make_jo_game_session_sequencing();
		auto send2 = [&](std::vector<ProtocolMessage> messages) {
			std::vector<uint8_t> packet;
			frame_session_packet(seq2, SessionCrypto{kServerScrk, {}, kClientKey}, messages, packet);
			auto datagram =
					nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
			h2.role.runtime->receive(datagram.data(), datagram.size());
			h2.role.run_tick(h2.input);
		};
		send2({make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))});
		w::Entity *l2 = h2.kernel->local.player();
		if (!expect(l2 != nullptr && l2->alive, "zero tail + 0x13: L stands alive"))
			return false;
		l2->spawn_phase = 0;
		FrameUpdate dead_tail = fu;
		dead_tail.health = 0;
		send2({make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(dead_tail)),
				make_protocol_message(s2c::ENTITY_DEATH, std::vector<uint8_t>{0x05, 0x00, 0x2A, 0x00})});
		if (!expect(l2->health == 0 && (l2->flags & w::kEntityFlagDead) != 0 &&
						l2->spawn_phase == 63,
				"zero tail + 0x13 in one pump: the callback runs past its dead return; the "
				"death edge latches after"))
			return false;
	}

	// An AI-data twin's callback is its brain machine (event 4, the death): in
	// session off the authority it ticks the brain's state row and queues the
	// type-20 event [orig: the callback @0x42EBF5; cveh @0x813378 ->
	// EntityAI_ProcessGroundStateMachine @0x4583C0, the client death leg
	// @0x45848B..0x4584D4].
	w::World &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	w::Entity hull;
	hull.kind = w::EntityKind::Item;
	hull.item_id = 901;
	hull.has_item_def = true;
	hull.is_ai_capable = true;
	hull.health = 400;
	hull.alive = true;
	const w::EntityHandle hull_h = world.registry.spawn_at(w::EntityHandle::make(1, 3), hull);
	world.ai.attach(hull_h);
	w::VehicleTraits ground;
	ground.brain_class = w::VehicleBrainClass::Ground;
	world.vehicles.traits.set(901, ground);
	// A joiner in a multiplayer session (the brain machine's in-session leg).
	world.rules.mp_session = true;
	world.ai.is_in_session = true;
	w::AiEntity *brain = world.ai.for_handle(hull_h);
	if (!expect(brain != nullptr, "0x13 on an AI-data twin: the twin carries a brain"))
		return false;
	brain->health = 400;
	send({make_protocol_message(s2c::ENTITY_DEATH,
			std::vector<uint8_t>{static_cast<uint8_t>(hull_h.packed & 0xFF),
					static_cast<uint8_t>(hull_h.packed >> 8), 0x00, 0x00})});
	// The machine's in-session death leg zeroes the brain's Health mirror with
	// the entity's ahead of its state row's tick [orig: @0x45848B].
	return expect(world.registry.get(hull_h) != nullptr &&
					world.registry.get(hull_h)->health == 0 && brain->health == 0,
			"0x13 on an AI-data twin: its brain machine ran the death event");
}

// D-COL-21: the redeploy release's reset ends in the candidate-slice rebuild and
// L's own list, so L walks a slice built at its redeployed pose at once, where it
// kept the one built at its death spot until the next 17-tick edge.
// [orig: NetPacket_SerializePlayerState @0x4C113C -> Entity_ResetToSpawnState -
//  the Entity_BuildProximityListsFromPools call @0x4B98EB, the
//  Entity_BuildProximityList call @0x4B98F1]
bool run_redeploy_release_rebuilds_the_candidate_slices() {
	Harness h;
	auto &world = h.kernel->world;
	world.registry.configure_pool(2, 4);
	// Retail's head row, then the player's: its ordinal 1 is the +0x1C word a
	// pool-0 source needs.
	def::DefItemDef defs[2]{};
	defs[0].id = 100000;
	defs[1].id = w::kPlayerInfantryTypeId + mission::kItemIdOffset;
	defs[1].type = 3;
	defs[1].hp = 100;
	// Retail's 105305 row moves on org2: the player body's update (D-NET-NEW-L4-14).
	std::strcpy(defs[1].move_function, "org2");
	def::DefItemsFile items{defs, 2};
	h.kernel->set_items_table(&items);
	h.kernel->resolve_collision_instances();
	// The redeploy lands on the frame anchor, where a wall stands.
	const int32_t anchor[3] = {40 << 16, 30 << 16, 0};
	w::Entity wall;
	wall.kind = w::EntityKind::Building;
	wall.position = {40.0f, 30.0f, 0.0f};
	wall.bound_radius = 1.0f;
	wall.alive = true;
	world.registry.spawn(2, wall);
	h.kernel->collision.build_initial_tables(world);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	auto &self_row = h.role.runtime->state().upsert(0x0005);
	self_row.cls = EntityClass::Player;
	self_row.type_id = w::kPlayerInfantryTypeId;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	auto frame = [&](int16_t health, uint8_t state_flags) {
		FrameUpdate fu;
		fu.carried_handle = 0xFFFF;
		fu.health = health;
		fu.anchor_x = anchor[0];
		fu.anchor_y = anchor[1];
		fu.anchor_z = anchor[2];
		FrameUpdateRecord rec;
		rec.handle = 0x0005;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.state_flags = state_flags;
		fu.records.push_back(rec);
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
	};
	frame(100, 0x00);
	frame(0, w::kEntityFlagDead);
	w::Entity *local = world.registry.get(world.cached.local_player);
	w::AiEntity *local_ai = h.kernel->local.player_ai();
	if (!expect(local != nullptr && local_ai != nullptr && !local->alive &&
					local->item_type_index == 1 && h.role.runtime->deployment_pick_pending(),
			"slice: the death frame downs L, a pool-0 slice source"))
		return false;
	// L's corpse lies 500 u off, its slice built there: no wall.
	local->position = {540.0f, 30.0f, 0.0f};
	local_ai->self->position.x.q16 = 540 << 16;
	local_ai->self->position.y.q16 = 30 << 16;
	local_ai->self->position.z.q16 = 0;
	h.kernel->collision.rebuild_candidate_slices(world);
	if (!expect(h.kernel->collision.candidate_count(local->handle) == 0,
			"slice: the corpse's slice holds no wall"))
		return false;
	if (!expect(h.role.runtime->queue_deployment_pick(0xFFFF), "slice: the Default Spawn pick"))
		return false;
	h.role.run_tick(h.input);
	frame(100, 0x00); // the record alive again
	frame(100, 0x00); // the next tail releases L
	local = world.registry.get(world.cached.local_player);
	if (!expect(local != nullptr && local->alive && std::fabs(local->position.x - 40.0f) < 0.01f,
			"slice: the release lands L on the record's pose"))
		return false;
	return expect(h.kernel->collision.candidate_count(local->handle) == 1,
			"slice: the release rebuilds L's slice at the redeployed pose");
}

// The Respawn key on a joiner (dispatch 55): past the live-player gate the
// input reset runs (with L's def bound: the input word cleared, the pack over
// it, a moving state dropped to the idle 43) and, in a session, one bodiless
// C2S 0x41 leaves at the frame's send block; a dead L sends nothing, and out
// of a session the reset runs alone. [orig: Input_HandleActionBinding case 55
// -- the dead bit @0x49AD7B, Player_ResetLocalInput @0x49B20B, is_in_session @0x49B210,
// is_mp_session_peer @0x49B21D, NetPacket_SendRequestRespawn @0x49B22A]
bool run_respawn_key_resets_input_and_sends_0x41() {
	std::vector<opennova::def::DefItemDef> rows(1);
	rows[0].id = static_cast<int>(w::kPlayerInfantryTypeId) +
			static_cast<int>(opennova::mission::kItemIdOffset);
	rows[0].hp = 100;
	opennova::def::DefItemsFile items{};
	items.entries = rows.data();
	items.count = rows.size();
	const auto respawn_sent = [](const CountingSocket &socket) {
		ProtocolMessage m;
		return socket.last_message(c2s::DEATH_TIMEOUT_RESET, m) && m.payload.empty();
	};
	bool ok = true;
	for (const bool in_session : {true, false}) {
		Harness h;
		h.kernel->world.load_systems();
		h.kernel->set_items_table(&items);
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
		h.role.run_tick(h.input); // the spawn frame
		w::LocalPlayer &lp = h.kernel->local;
		w::AiEntity *body = lp.player_ai();
		if (!expect(h.role.local_spawned() && body != nullptr && lp.player()->has_item_def,
				"respawn key: L spawned with its def")) {
			h.kernel->set_items_table(nullptr);
			return false;
		}
		h.kernel->world.rules.mp_session = in_session; // the joiner boot's session word
		inmatch::TickInput press = h.input;
		press.player.pressed_action_bits = inmatch::PRESSED_RESPAWN;
		// A live health tail; the self record's dead bit decides below.
		h.role.runtime->state().local_health = 100;
		if (in_session) {
			// The self record carries the dead bit (byte13 0x02): it zeroes the local
			// entity's Health and the frame's body tick latches Flags & 2, which the
			// row's bit-0 gate reads, so the key drops; a later record alive is the
			// respawn arm, which clears it.
			// [orig: NetPacket_SerializePlayerState @0x4C10F1..0x4C10FB, @0x4C1109;
			//  Entity_UpdateInfantryPlayerBody @0x4B4BF1..0x4B4C02]
			SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
			const auto self_record = [&](uint8_t flags) {
				FrameUpdate fu;
				fu.carried_handle = 0xFFFF;
				fu.health = 100; // a live tail: the record's dead bit decides
				FrameUpdateRecord rec;
				rec.handle = 0x0005;
				rec.type_id = w::kPlayerInfantryTypeId;
				rec.cls = EntityClass::Player;
				rec.player.carrier_handle = 0xFFFF;
				rec.player.state_flags = flags;
				fu.records.push_back(rec);
				std::vector<uint8_t> packet;
				frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
						{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))},
						packet);
				auto datagram = nw_encode_outbound(
						SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
				h.role.runtime->receive(datagram.data(), datagram.size());
				h.role.run_tick(h.input);
			};
			{
				// The spawn stream's row (a compact record never creates one).
				replication::ClientEntityState &self = h.role.runtime->state().upsert(0x0005);
				self.cls = EntityClass::Player;
				self.type_id = w::kPlayerInfantryTypeId;
			}
			self_record(0x00);
			self_record(0x02);
			ok &= expect(h.role.runtime->local_player_dead(),
					"respawn key: the self record's dead bit reads dead");
			body->inf.anim_state = w::anim_state::kWalkForward;
			h.role.apply_input(press);
			ok &= expect(body->inf.anim_state == w::anim_state::kWalkForward,
					"respawn key: a dead L takes no reset");
			for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
			ok &= expect(!respawn_sent(h.socket), "respawn key: a dead L sends nothing");
			self_record(0x00);
			ok &= expect(!h.role.runtime->local_player_dead(), "respawn key: L lives again");
		}
		body->inf.anim_state = w::anim_state::kWalkForward;
		lp.input_flags.flags = w::kInputFlagForward;
		lp.move_order = w::pack_player_body_input(lp.input_flags.flags, lp.input);
		// The same frame holds the forward key: the reset clears the key state, so it
		// reads up, and the queue's memset drops the frame's later events; the devices'
		// own reset is owed to the embedder (D-NET-560) [orig: Player_ResetLocalInput
		// @0x4B2B80 - Input_ResetAllState @0x4B2B99; Input_ResetAllState - the memset
		// @0x497EE6; Input_ProcessFrame - the count test @0x49D5E0].
		press.player.movement.forward = true;
		(void)h.role.take_input_reset();
		h.role.apply_input(press);
		ok &= expect(body->inf.anim_state == w::anim_state::kIdle &&
				lp.input_flags.flags == 0 && lp.input_flags.prev == 0 &&
				lp.move_order.direction_bits == 0 && !lp.move_order.moving,
				"respawn key: the input reset (word, pack, idle 43)");
		ok &= expect(!lp.input.forward && h.role.take_input_reset(),
				"respawn key: a key held through the reset reads up; the devices' reset is owed");
		press.player.movement.forward = false;
		for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
		ok &= expect(respawn_sent(h.socket) == in_session,
				in_session ? "respawn key: in a session one bodiless 0x41 leaves"
				           : "respawn key: out of a session nothing is sent");
		h.kernel->set_items_table(nullptr);
	}
	return ok;
}

// D-NET-562: the Respawn key's head reads the local entity, which the joiner has from
// its mission start's Player_InitPlayer, live (its dead bit clear) until a Health of 0
// latches it. Before L spawns and before the first 0x0A tail (the replica's Health still
// its zero default) the key resets the input and sends the bodiless 0x41.
// [orig: Input_HandleActionBinding @0x49AD40 - a6 @0x49AD76..0x49AD7E, the row's bit 0
//  @0x49AD95..0x49ADA0, case 55 @0x49B20B..0x49B22A; Game_StartMission @0x524360 -
//  Player_InitPlayer @0x525BBC]
bool run_respawn_key_before_the_first_tail() {
	Harness h;
	h.kernel->world.load_systems();
	h.kernel->world.rules.mp_session = true;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	if (!expect(!h.role.local_spawned() && h.role.runtime->state().local_health == 0 &&
				h.role.runtime->state().health_updates_applied == 0,
			"respawn before the tail: no L yet, no tail folded"))
		return false;
	if (!expect(!h.role.runtime->local_player_dead() && h.role.request_respawn(),
			"respawn before the tail: the mission start's entity reads live and the key is taken"))
		return false;
	for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
	ProtocolMessage m;
	return expect(h.socket.last_message(c2s::DEATH_TIMEOUT_RESET, m) && m.payload.empty(),
			"respawn before the tail: one bodiless 0x41 leaves");
}

// D-NET-348's joiner half: every self record re-resolves L's row by L's class, the
// camouflage item by the mission climate, rebound when its index differs. Desert (climate
// 0): class 6's camouflage is 5311, the third person row (index 3).
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the local jump to @0x4C1248, the
//  climate pick @0x4C1248..0x4C129C, ItemList_FindIndexByTypeId @0x4C129D, the compare
//  @0x4C12A5, the stores @0x4C12B8..0x4C12D6]
bool run_the_self_record_rebinds_ls_camouflage_row() {
	static const char kItems[] =
			"begin \"Null\"\r\n id 100000\r\n type marker\r\nend\r\n"
			"begin \"Player #1, Multiplayer\"\r\n id 105305\r\n type person\r\n hp 150\r\n move_function org2\r\nend\r\n"
			"begin \"Player #1, Single player\"\r\n id 105310\r\n type person\r\n hp 120\r\n move_function org2\r\nend\r\n"
			"begin \"Generic Soldier\"\r\n id 105311\r\n type person\r\n hp 100\r\n move_function org2\r\nend\r\n";
	static const char kCharattr[] =
		"[CHARACTER1]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER2]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER3]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER4]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER5]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5311\r\n"
		"[CHARACTER6]\r\nJUNGLE_CAMMO\t= 9999\r\nDESERT_CAMMO\t= 5311\r\nARCTIC_CAMMO\t= 5305\r\n";
	opennova::def::DefItemsFile items{};
	if (!expect(opennova::def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
						sizeof(kItems) - 1, &items) == 0,
				"camouflage row: the items parse"))
		return false;
	Harness h;
	w::World &world = h.kernel->world;
	opennova::mission::resolve_item_traits(world, items, {}, nullptr);
	world.tables.bms_climate = 0;
	inmatch::CharAttrTable table;
	inmatch::charattr_load(table, reinterpret_cast<const uint8_t *>(kCharattr), sizeof(kCharattr) - 1);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	// The joiner's charattr.def words reach the world at each frame's end
	// (JoinerRole::sync_class_attribute_flags).
	h.role.runtime->set_charattr_table(table);
	h.role.run_tick(h.input);
	auto &row = h.role.runtime->state().upsert(0x0005);
	row.cls = EntityClass::Player;
	row.type_id = w::kPlayerInfantryTypeId;
	w::Entity *local = h.kernel->local.player();
	bool ok = expect(local != nullptr, "camouflage row: L spawned");
	if (ok) {
		local->player_class = 6;
		SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
		FrameUpdate fu;
		fu.carried_handle = 0xFFFF;
		fu.health = 100;
		FrameUpdateRecord rec;
		rec.handle = 0x0005;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		fu.records.push_back(rec);
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
		local = h.kernel->local.player();
		ok = expect(local != nullptr && local->item_type_index == 3 && local->item_id == 0x14BF,
				"camouflage row: the self record rebinds L to its class's desert row (5311)");
		if (ok) {
			// The index compares first: a record whose row is the body's own stores
			// nothing, so a word another writer moved keeps its value
			// [orig: @0x4C12A5..0x4C12A8 -> @0x4C12F4].
			local->health_max = 77;
			std::vector<uint8_t> again;
			frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
					{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, again);
			auto second = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(again));
			h.role.runtime->receive(second.data(), second.size());
			h.role.run_tick(h.input);
			ok = expect(local->item_type_index == 3 && local->health_max == 77,
					"camouflage row: a record naming the body's own row stores nothing");
		}
	}
	opennova::def::def_free_items(&items);
	return ok;
}

// D-NET-348's remote half: a remote player record re-picks its row by the record's
// class nibble, the camouflage item for the joiner's climate looked up as a row; the
// self record is L's fold's. Desert: class 6 -> 5311 (0x14BF).
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the class store @0x4C11B4
//  (Entity_SetHealthFromDifficultyByte @0x4AD5A2), the climate pick @0x4C1248..0x4C129C,
//  ItemList_FindIndexByTypeId @0x4C129D, the compare @0x4C12A5, the rebind
//  @0x4C12B8..0x4C12D6]
bool run_a_remote_record_repicks_its_rows_camouflage_row() {
	// The camouflage rows are plyr rows (a player body's class row).
	static const char kItems[] =
			"begin \"Null\"\r\n id 100000\r\n type marker\r\nend\r\n"
			"begin \"Player #1, Multiplayer\"\r\n id 105305\r\n ai_function plyr\r\n type person\r\n hp 150\r\n"
			" move_function org2\r\nend\r\n"
			"begin \"Player #1, Single player\"\r\n id 105310\r\n type person\r\n hp 120\r\n move_function org2\r\nend\r\n"
			"begin \"Generic Soldier\"\r\n id 105311\r\n ai_function plyr\r\n type person\r\n hp 100\r\n"
			" move_function org2\r\nend\r\n";
	static const char kCharattr[] =
		"[CHARACTER1]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER2]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER3]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER4]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5310\r\nARCTIC_CAMMO\t= 5310\r\n"
		"[CHARACTER5]\r\nJUNGLE_CAMMO\t= 5310\r\nDESERT_CAMMO\t= 5305\r\nARCTIC_CAMMO\t= 5311\r\n"
		"[CHARACTER6]\r\nJUNGLE_CAMMO\t= 9999\r\nDESERT_CAMMO\t= 5311\r\nARCTIC_CAMMO\t= 5305\r\n";
	constexpr uint16_t kRemote = 0x0007;
	constexpr uint16_t kSelf = 0x0005;
	opennova::def::DefItemsFile items{};
	if (!expect(opennova::def::def_parse_items_memory(reinterpret_cast<const uint8_t *>(kItems),
						sizeof(kItems) - 1, &items) == 0,
				"remote camouflage row: the items parse"))
		return false;
	Harness h;
	w::World &world = h.kernel->world;
	opennova::mission::resolve_item_traits(world, items, {}, nullptr);
	world.tables.bms_climate = 0;
	inmatch::CharAttrTable table;
	inmatch::charattr_load(table, reinterpret_cast<const uint8_t *>(kCharattr), sizeof(kCharattr) - 1);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
			1, 0, kSelf, w::kPlayerInfantryTypeId);
	h.role.runtime->set_charattr_table(table);
	// The view classifies through the item catalog the shell installs.
	h.role.runtime->view().set_item_catalog(
			std::make_shared<const replication::ItemReplicationCatalog>(
					replication::ItemReplicationCatalog::from_items_def(items)));
	h.role.run_tick(h.input);
	for (uint16_t handle : {kRemote, kSelf}) {
		auto &row = h.role.runtime->state().upsert(handle);
		row.cls = EntityClass::Player;
		row.type_id = w::kPlayerInfantryTypeId;
	}
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	FrameUpdate fu;
	fu.carried_handle = 0xFFFF;
	fu.health = 100;
	for (uint16_t handle : {kRemote, kSelf}) {
		FrameUpdateRecord rec;
		rec.handle = handle;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.health_class_byte = 0x26; // tier 2, class 6
		fu.records.push_back(rec);
	}
	std::vector<uint8_t> packet;
	frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
			{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
	auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
	h.role.runtime->receive(datagram.data(), datagram.size());
	h.role.run_tick(h.input);
	// Class 6's DESERT_CAMMO (5311) is the minted table's fourth row; its wire type is
	// the row's id less the items offset.
	const int expected_type = items.entries[3].id - opennova::mission::kItemIdOffset;
	const auto *remote = h.role.runtime->state().find(kRemote);
	const auto *self = h.role.runtime->state().find(kSelf);
	bool ok = expect(items.count == 4 && expected_type == 5311 &&
					expected_type != w::kPlayerInfantryTypeId,
			"remote camouflage row: the minted class-6 desert row");
	ok = ok && expect(remote != nullptr && remote->type_id == expected_type,
			"remote camouflage row: a class-6 record re-types the remote row to 5311");
	ok = ok && expect(remote->cls == EntityClass::Player,
			"remote camouflage row: the re-typed row's class follows its plyr row");
	ok = ok && expect(self != nullptr && self->type_id == w::kPlayerInfantryTypeId,
			"remote camouflage row: the self row is left to L's fold");
	opennova::def::def_free_items(&items);
	return ok;
}

// D-NET-710: a console-queued action dispatches by its code at the next input frame:
// the Respawn row's code 55 runs the Respawn key's arm (its head gates, the input reset,
// the bodiless 0x41), a code the role does not own goes to the embedder's dispatch. The
// local entity is bound from the mission start, so the 55's reset clears the event queue
// and a code queued after it never dispatches (FOUND 4, D-NET-560).
// [orig: Input_ProcessFrame @0x49D520 - the drain @0x49D599..0x49D5E7 ->
//  Input_HandleActionBinding @0x49D5D8, case 55 @0x49B20B..0x49B22A; Player_ResetLocalInput
//  @0x4B2B80 - Input_ResetAllState @0x4B2B99]
bool run_a_console_action_dispatches_by_code() {
	Harness h;
	h.kernel->world.load_systems();
	h.kernel->world.rules.mp_session = true;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	std::vector<int32_t> sunk;
	h.role.runtime->set_console_action_sink(
			[&sunk](int32_t code, int32_t, int32_t, int32_t) { sunk.push_back(code); });
	(void)h.role.runtime->queue_console_action(201, 0, 0, 0);
	(void)h.role.runtime->queue_console_action(55, 0, 0, 0);
	(void)h.role.runtime->queue_console_action(202, 0, 0, 0);
	h.role.apply_input(h.input);
	for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
	ProtocolMessage m;
	return expect(h.socket.last_message(c2s::DEATH_TIMEOUT_RESET, m) && m.payload.empty() &&
					sunk == std::vector<int32_t>{201},
			"console actions: code 55 sends the bodiless 0x41; code 201 goes to the embedder; "
			"the reset drops 202");
}

// L8-s9 FOUND (routed to L2): a link the connection lost with a record outside the
// description class stores no exit reason; once the round-over gate is up the HUD's overlay
// runs input action 3: exit reason 1 and its Exit Mission tone. With the gate down nothing is
// stored by the joiner's tick.
// [orig: HUD_DrawOverlayPanels @0x5C0060 - g_LoadingTimeoutFlag @0x5C00FC, g_RoundOver @0x5C0105,
//  Input_HandleActionBinding(3) @0x5C0118; CNapiNetwork_OnDisconnectedFromServer @0x4C63D0 -
//  @0x4C64A4, the description-class gate @0x4C6563]
bool run_a_lost_link_at_round_over_runs_input_action_3() {
	bool ok = true;
	for (const bool round_over : {false, true}) {
		Harness h;
		h.kernel->world.load_systems();
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
		h.role.run_tick(h.input);
		h.role.runtime->state().spawn_success_gate = round_over;
		// The host's stop record (class 9), outside the description class.
		DisconnectEvent stop;
		stop.ds = 1;
		stop.dc = 9;
		stop.ddstr = "NP.C:SH:STOP";
		SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(0x03, connection_description_to_bytes(stop), 0xA0)},
				packet);
		auto datagram = nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.kernel->world.out.script_sounds.clear();
		h.role.run_tick(h.input);
		bool tone = false;
		for (const w::ScriptSoundEvent &s : h.kernel->world.out.script_sounds)
			tone |= s.kind == w::ScriptSoundEvent::Kind::Interface && s.name == "PU_EXIT_CONFIRM";
		ok &= expect(h.role.runtime->session_lost(), "lost link: the stop record ends the link");
		ok &= expect(h.role.runtime->mission_exit_reason() ==
						(round_over ? inmatch::kMissionExitQuit : inmatch::kMissionExitNone) &&
						tone == round_over,
				round_over ? "lost link: at round over the HUD runs action 3 (reason 1, the tone)"
				           : "lost link: mid-round the tick stores no reason and plays no tone");
	}
	return ok;
}

// L2-s8 FOUND 4: the mission start's block on a non-null local player runs on a joiner's L
// too: the indoors bit and the medic-revive latch cleared, the hidden bit cleared, the
// proximity rebuild, the hidden bit set again; so a spawned L with no record folded and no
// 0x0F landed is hidden, and the 0x0F's landing clears it.
// [orig: Game_StartMission @0x524360 - @0x525C74, @0x525C81, @0x525C8C,
//  Entity_BuildAllProximityLists @0x525C90, `or [eax+24h], edi` @0x525C9E (edi = 1 from
//  @0x5255B9); NapiNPClientMsg_0x00F @0x42E200 (Flags &= ~1 unless the death screen)]
bool run_the_mission_start_leaves_l_hidden() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	w::Entity *local = h.kernel->local.player();
	if (!expect(local != nullptr, "mission start block: L spawned")) return false;
	bool ok = expect((local->flags & w::kEntityFlagCarried) != 0 &&
					(local->flags & w::kEntityFlagIndoors) == 0 && !local->medic_reviving,
			"mission start block: L leaves the mission start hidden, indoors and medic clear");
	++h.role.runtime->state().world_state.revision;
	h.role.run_tick(h.input);
	local = h.kernel->local.player();
	ok &= expect(local != nullptr && (local->flags & w::kEntityFlagCarried) == 0,
			"mission start block: the 0x0F's landing clears the hidden bit");
	return ok;
}

// D-NET-710 (L2-s8 FOUND 1): the console's "quit" (row 78, code 4, flags 0x04000000: no head
// gate) stores exit reason 2 alone, on the joiner's runtime: the session leaves the mission
// (the shell's Post Menu route ends the game), nothing is sent at the action, and the
// teardown's session reset bursts the record as it stands (zeros: nothing latched).
// [orig: Input_HandleActionBinding @0x49AD40 - case 4 @0x49AF4C; PostMenu_RouteMissionExit
//  @0x568460 - CNapiGameSession_ResetActiveSession @0x56846F, UI_NavHistoryPush(0) @0x56849A]
bool run_the_console_quit_stores_the_reset_exit() {
	Harness h;
	h.kernel->world.load_systems();
	h.kernel->world.rules.mp_session = true;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	std::vector<int32_t> sunk;
	h.role.runtime->set_console_action_sink(
			[&sunk](int32_t code, int32_t, int32_t, int32_t) { sunk.push_back(code); });
	(void)h.role.runtime->queue_console_action(4, 0, 0, 0);
	h.socket.datagrams.clear();
	h.role.apply_input(h.input);
	inmatch::SessionError lost;
	bool ok = expect(h.role.runtime->mission_exit_reason() == inmatch::kMissionExitReset &&
					sunk.empty() && h.socket.datagrams.empty() && h.role.session_lost(lost),
			"console quit: code 4 stores exit reason 2 on the runtime and sends nothing");
	h.role.close();
	std::size_t goodbyes = 0;
	for (const std::vector<uint8_t> &datagram : h.socket.datagrams) {
		uint8_t opcode = 0;
		std::vector<uint8_t> body;
		DisconnectEvent event;
		if (nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_GOODBYE && body.size() >= 4 &&
				parse_disconnect_event(body.data() + 4, body.size() - 4, event) &&
				event.dc == 0 && event.dstr.empty())
			++goodbyes;
	}
	ok &= expect(goodbyes == 4, "console quit: the session reset bursts the unlatched record");
	return ok;
}

// D-NET-710 (V-L2-s7): the stance rows 9..11 carry flags 0x0C000C01, so the dispatcher's head
// drops a stance select in the pre-round, under the deploy overlay and at round over, a
// console-queued one (the role's own arm now) and a key's alike; with none held the select
// sends its C2S 0x1D. The mouse look rows (0x0C100425 / 0x0C200425) pass the same head, so the
// frame's look leaves L's heading alone while a gate holds (V-LI-s1 FOUND 5).
// [orig: Input_HandleActionBinding @0x49AD40 - 0x400 @0x49ADB3..0x49ADC2, 0x8000000
//  @0x49ADC8..0x49ADD7, g_RoundOver @0x49ADEE..0x49AE16; Input_HandleActionBinding_0
//  @0x4E0420 - cases 170 @0x4E0DF3, 169 @0x4E0D77; Input_TryTriggerMouseAxisBinding
//  @0x499130 - Input_QueueEvent @0x499156]
bool run_a_stance_select_and_the_look_take_the_rows_head() {
	enum class Gate { None, PreRound, Overlay, RoundOver };
	bool ok = true;
	for (const Gate gate : {Gate::None, Gate::PreRound, Gate::Overlay, Gate::RoundOver}) {
		Harness h;
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
		h.role.run_tick(h.input);
		if (!expect(h.role.local_spawned(), "stance head: L spawned")) return false;
		replication::ClientState &cs = h.role.runtime->state();
		if (gate == Gate::PreRound) {
			cs.preround_delay_seconds = 5;
			h.kernel->world.preround_delay_seconds = 5;
		}
		if (gate == Gate::Overlay) cs.deploy_overlay_active = true;
		if (gate == Gate::RoundOver) cs.spawn_success_gate = true;
		const bool admitted = gate == Gate::None;
		ok &= expect(h.role.action_head_admits(170) == admitted,
				admitted ? "stance head: nothing held admits the row"
				         : "stance head: the gate drops the row");
		// The seat key's row (17, code 182, 0x0C000C01) takes the same head (V-L2-s8); the
		// Knife row (28, code 201, 0x0C000801) carries no 0x400, so the pre-round passes it.
		ok &= expect(h.role.action_head_admits(182) == admitted,
				"seat head: the seat key's row follows the stance row's head");
		ok &= expect(h.role.action_head_admits(201) == (gate == Gate::None || gate == Gate::PreRound),
				"knife head: no 0x400, so only the overlay and the round over drop it");
		std::vector<int32_t> sunk;
		h.role.runtime->set_console_action_sink(
				[&sunk](int32_t code, int32_t, int32_t, int32_t) { sunk.push_back(code); });
		h.socket.datagrams.clear();
		// The console's "Prone" (row 9, code 170) dispatches at the role's input frame.
		(void)h.role.runtime->queue_console_action(170, 0, 0, 0);
		const int32_t heading_before = h.kernel->local.input.look_heading;
		inmatch::TickInput look = h.input;
		look.player.look_delta_x = 40.0f;
		h.role.apply_input(look);
		ok &= expect((h.kernel->local.input.look_heading != heading_before) == admitted,
				admitted ? "stance head: the look turns L" : "stance head: the gate drops the look");
		// The held forward row (row 2, code 152, 0x0C002C05) queues its event each frame and
		// takes the same head [orig: Input_ProcessAnalogBindings @0x4995F0 - @0x499660].
		inmatch::TickInput walk = h.input;
		walk.player.movement.forward = true;
		h.role.apply_input(walk);
		ok &= expect(h.kernel->local.input.forward == admitted,
				admitted ? "held head: the forward key holds" : "held head: the gate drops the forward key");
		ok &= expect(sunk.empty(), "stance head: the stance codes are the role's, not the embedder's");
		for (int i = 0; i < 12; ++i) h.role.run_tick(h.input);
		ProtocolMessage select;
		const bool console_sent = h.socket.last_message(0x1D, select);
		ok &= expect(console_sent == admitted,
				admitted ? "stance head: the console's prone sends its 0x1D"
				         : "stance head: the console's prone sends nothing");
		if (console_sent)
			ok &= expect(select.payload.size() == 2 && select.payload[0] == 0xAA &&
							select.payload[1] == 0x00,
					"stance head: the console's 0x1D carries action 170");
		// The key's crouch takes the same head.
		h.socket.datagrams.clear();
		ok &= expect(h.role.request_stance(1) == admitted,
				"stance head: the key's select passes or drops with the console's");
		for (int i = 0; i < 12; ++i) h.role.run_tick(h.input);
		ok &= expect(h.socket.last_message(0x1D, select) == admitted,
				"stance head: the key's 0x1D follows the head");
	}
	return ok;
}

// D-NET-685 (V-L2-s7): the deploy action's head runs Input_ResetAllState, whose event-queue
// memset ends the input frame's drain, so a console-queued pick (code 12, the game's sink)
// leaves the codes queued behind it undispatched: the Respawn row's 55 sends no 0x41 and the
// embedder never sees the code after it.
// [orig: Input_HandleActionBinding @0x49AD40 - case 12 @0x49B024, Input_ResetAllState
//  @0x49B042; Input_ResetAllState @0x497ED0 - the queue's memset @0x497EE6; Input_ProcessFrame
//  @0x49D520 - the drain's count test @0x49D5E0]
bool run_a_console_deploy_pick_ends_the_drain() {
	Harness h;
	h.kernel->world.load_systems();
	h.kernel->world.rules.mp_session = true;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	std::vector<int32_t> sunk;
	// The game's sink takes 12 as the deploy pick (Simulation::send_deployment_pick ->
	// Role::request_deployment_pick).
	h.role.runtime->set_console_action_sink([&](int32_t code, int32_t, int32_t, int32_t) {
		sunk.push_back(code);
		if (code == 12) (void)h.role.request_deployment_pick(0xFFFFu);
	});
	(void)h.role.take_input_reset();
	(void)h.role.runtime->queue_console_action(12, 0, 0, 0);
	(void)h.role.runtime->queue_console_action(55, 0, 0, 0);
	(void)h.role.runtime->queue_console_action(201, 0, 0, 0);
	h.role.apply_input(h.input);
	for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
	ProtocolMessage m;
	bool ok = expect(sunk == std::vector<int32_t>{12},
			"console deploy pick: the drain ends at the pick (201 never reaches the embedder)");
	ok &= expect(!h.socket.last_message(c2s::DEATH_TIMEOUT_RESET, m),
			"console deploy pick: the Respawn code behind it sends no 0x41");
	ok &= expect(h.role.take_input_reset(), "console deploy pick: the devices' reset is owed");
	return ok;
}

// V-L2-s7 FOUND 2 + V-L2-s8 (NEW-L2-17): no input frame runs under a menu screen, so a
// console-queued action waits for the screen's close; every close runs Input_ResetAllState
// (UI_CloseMenuScreen's tail call: the shell's resume and screen closes, and the armory's
// ACCEPT / CANCEL head before it), whose event-queue memset drops it, and the devices owe their
// reset. The close reaches the role as Role::reset_all_input_state.
// [orig: Game_ProcessMainFrame @0x5263F0 - UI_IsMenuScreenOpen @0x52660D skips
//  Input_ProcessFrame @0x52661D; UI_CloseMenuScreen @0x54E5E0 - Input_ResetAllState @0x54E63F;
//  WeaponLoadout_ApplyFromBuffer @0x565CD0 - Input_ResetAllState @0x565D01; Input_ResetAllState
//  @0x497ED0 - the event queue's memset @0x497EE6]
bool run_a_menu_screen_holds_the_console_queue_and_its_reset_drops_it() {
	Harness h;
	h.kernel->world.load_systems();
	h.kernel->world.rules.mp_session = true;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	(void)h.role.runtime->queue_console_action(55, 0, 0, 0);
	inmatch::TickInput menu = h.input;
	menu.player.held_action_bits = inmatch::HELD_MENU_OPEN;
	h.role.apply_input(menu);
	for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
	ProtocolMessage m;
	bool ok = expect(!h.socket.last_message(c2s::DEATH_TIMEOUT_RESET, m),
			"menu screen: the queued Respawn waits under the screen");
	(void)h.role.take_input_reset();
	h.role.reset_all_input_state(); // the screen's close
	ok &= expect(h.role.take_input_reset(), "menu screen: the close owes the devices' reset");
	h.role.apply_input(h.input);
	for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
	ok &= expect(!h.socket.last_message(c2s::DEATH_TIMEOUT_RESET, m),
			"menu screen: the screen's close dropped the held Respawn");
	return ok;
}

// L2-s8 row 2 iii: the joiner's teardown at the mission's exit leaves by its post-mission
// route: a route that destroys the session latches "N.C:SWWAD[1]:<exit reason>" (FullDestroy's
// leave), the session reset (reason 2, the "quit" row's) bursts the record as it stands (zeros:
// nothing latched), and an error arm (reason 5) tears nothing down. Input action 3's own leave
// latched "I.C:CIDEMIS" first (remote_admin's GOTO MENUSTATE pin).
// [orig: PostMenu_RouteMissionExit @0x568460 - CNapiGameSession_ResetActiveSession @0x56846F,
//  loc_5686DA (no teardown), CNapiGameSession_FullDestroy(g_MissionExitReason) @0x568683;
//  CNapiGameSession_FullDestroy @0x4C96A0 - the sprintf @0x4C96CC, the leave @0x4C96DB;
//  SendDisconnectPacket @0x61F2A0 - the record's fields @0x61F3B1..0x61F4AA]
bool run_the_teardown_goodbye_follows_the_exit_route() {
	struct Case {
		int32_t reason;
		bool burst;
		uint32_t dc;
		const char *dstr;
	};
	// Reason 0 never reaches the Post Menu: its teardown is the process exit's session reset,
	// the record as it stands (V-L2-s8) [orig: Game_ProcessMainFrame @0x5263F0 - @0x526860;
	// NapiLib_Shutdown @0x4C9580 - CNapiGameSession_ResetActiveSession @0x4C958B].
	const Case cases[] = {{0, true, 0, ""}, {1, true, 2, "N.C:SWWAD[1]:1"},
			{2, true, 0, ""}, {5, false, 0, ""}};
	bool ok = true;
	for (const Case &c : cases) {
		Harness h;
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
		h.role.run_tick(h.input);
		h.role.runtime->set_mission_exit_reason(c.reason);
		h.socket.datagrams.clear();
		// A joiner in the match has no pre-game join screen: the shell's join cancel sends
		// nothing there (JoinerRole::cancel_join).
		h.role.cancel_join();
		ok &= expect(h.socket.datagrams.empty(), "teardown: the pre-game cancel is not a match's");
		h.role.close();
		std::vector<DisconnectEvent> goodbyes;
		for (const std::vector<uint8_t> &datagram : h.socket.datagrams) {
			uint8_t opcode = 0;
			std::vector<uint8_t> body;
			DisconnectEvent event;
			if (nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
					opcode == SESSION_OPCODE_CLIENT_GOODBYE && body.size() >= 4 &&
					parse_disconnect_event(body.data() + 4, body.size() - 4, event))
				goodbyes.push_back(event);
		}
		ok &= expect(goodbyes.size() == (c.burst ? 4u : 0u),
				c.burst ? "teardown: the route's teardown writes the four-datagram burst"
				        : "teardown: an error arm tears nothing down");
		if (!goodbyes.empty())
			ok &= expect(goodbyes[0].dc == c.dc && goodbyes[0].dstr == c.dstr &&
							goodbyes[0].dp1 == 0 && goodbyes[0].dp2 == 0 && goodbyes[0].dpc == 0,
					"teardown: the burst carries the route's record");
	}
	return ok;
}

// V-L2-s9 (d): the join screen's ABORT ahead of the mission start leaves through the join
// state machine's FullDestroy(-1): a joiner connected (its 0x82 taken) and still in the
// pre-game bursts the "N.C:SWWAD[1]:-1" record. In the match it sends nothing
// (run_the_teardown_goodbye_follows_the_exit_route). In the wait for the JOIN's S2C 0x00
// (state 5: the host's initial settings answered with the C2S 0x00) the same pass's abort
// test destroys with -2 (V-ROLL-C on L2-s11), so that burst carries "N.C:SWWAD[1]:-2".
// [orig: PreMenu_OnAbortCancel @0x568CA0 - g_PreMenuState = 9 @0x568CB4;
//  MultiPlayer_JoinSessionStateMachine @0x56A320 - state 9's CNapiGameSession_FullDestroy(-1)
//  @0x56A3E3, state 5's abort test @0x56A591..0x56A59C and FullDestroy(-2) @0x56A59E /
//  @0x56A5A0; CNapiGameSession_FullDestroy @0x4C96A0 - the sprintf @0x4C96CC, the leave
//  @0x4C96DB]
bool run_the_join_screen_abort_bursts_its_state_record(bool join_sent, const char *dstr) {
	Harness h;
	h.role.poll_preload();
	uint8_t opcode = 0;
	std::vector<uint8_t> body;
	ClientHello hello;
	if (!expect(h.socket.datagrams.size() == 1 &&
						nw_decode_inbound(h.socket.datagrams[0].data(),
								h.socket.datagrams[0].size(), opcode, body) &&
						parse_client_hello(body.data(), body.size(), hello),
				"abort: the ClientHello shipped"))
		return false;
	ServerHello server_hello = build_server_hello(hello, 0x7F000001u, 32769);
	server_hello.hk = 0x13572468u;
	const std::vector<uint8_t> server_hello_dg = nw_encode_outbound(
			SESSION_OPCODE_SERVER_HELLO, server_hello_to_bytes(server_hello));
	h.socket.datagrams.clear();
	h.role.runtime->receive(server_hello_dg.data(), server_hello_dg.size());
	// The 0x81's poll records the session, the next starts the connect state, and the 0x42
	// leaves at the first poll whose (steady) clock reads past that start (D-NET-NEW-L2-25).
	ClientAuth auth;
	bool have_auth = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!have_auth && std::chrono::steady_clock::now() < deadline) {
		h.role.poll_preload();
		for (const std::vector<uint8_t> &dg : h.socket.datagrams)
			if (nw_decode_inbound(dg.data(), dg.size(), opcode, body) &&
					opcode == SESSION_OPCODE_CLIENT_AUTH &&
					parse_client_auth(body.data(), body.size(), auth))
				have_auth = true;
	}
	if (!expect(have_auth, "abort: the 0x81 leads to the 0x42")) return false;
	ServerAuth server_auth = build_server_auth(
			auth, 0x7F000001u, 32769, 0x0C0FFEE1u, "SERVER-ABORT-SCRK", "", "", "", false);
	server_auth.mi = 4;
	const std::vector<uint8_t> server_auth_dg = nw_encode_outbound(
			SESSION_OPCODE_SERVER_AUTH, server_auth_to_bytes(server_auth));
	h.role.runtime->receive(server_auth_dg.data(), server_auth_dg.size());
	h.role.poll_preload();
	if (join_sent) {
		// Golden retail frame 6: the host's initial settings pair, which the joiner answers with
		// its header-only ACK and the C2S 0x00 JOIN; the 0x00's answer is still owed.
		SessionSequencing server_seq = inmatch::make_jo_game_session_sequencing();
		const std::vector<ProtocolMessage> settings = {
				make_protocol_message(
						0x00, {0x00, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0),
				make_protocol_message(
						0x00, {0x01, 0x00, 0x20, 0x00, 0x00, 0x14, 0x05, 0x00, 0x00}, 0xA0),
		};
		std::vector<uint8_t> packet;
		if (!expect(frame_session_packet(server_seq,
							SessionCrypto{"SERVER-ABORT-SCRK", {}, auth.ck}, settings, packet),
					"abort: frame the initial settings pair"))
			return false;
		const std::vector<uint8_t> settings_dg =
				nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.socket.datagrams.clear();
		h.role.runtime->receive(settings_dg.data(), settings_dg.size());
		h.role.poll_preload();
		if (!expect(std::string(h.role.runtime->admission_stage_name()) ==
							"awaiting the JOIN acknowledgement",
					"abort: the settings are answered with the JOIN, whose 0x00 is owed"))
			return false;
	}
	if (!expect(h.role.runtime->in_session() && h.role.runtime->in_pre_game(),
				"abort: the joiner is connected and still in the pre-game"))
		return false;
	h.socket.datagrams.clear();
	h.role.cancel_join();
	std::vector<DisconnectEvent> goodbyes;
	for (const std::vector<uint8_t> &datagram : h.socket.datagrams) {
		DisconnectEvent event;
		if (nw_decode_inbound(datagram.data(), datagram.size(), opcode, body) &&
				opcode == SESSION_OPCODE_CLIENT_GOODBYE && body.size() >= 4 &&
				parse_disconnect_event(body.data() + 4, body.size() - 4, event))
			goodbyes.push_back(event);
	}
	if (!expect(goodbyes.size() == 4u, "abort: the cancel writes the four-datagram burst"))
		return false;
	return expect(goodbyes[0].dc == 2u && goodbyes[0].dstr == dstr &&
					goodbyes[0].dp1 == 0 && goodbyes[0].dp2 == 0 && goodbyes[0].dpc == 0,
			"abort: the burst carries the state's FullDestroy record");
}

// D-NET-562's pre-round leg: a client runs no entity update while its pre-round timer
// (the 0x0A's phase-0 byte) is nonzero, so a Health of 0 a tail stores in the pre-round
// latches no dead bit; the first frame whose tail reads the timer 0 runs the body tick
// and its death edge.
// [orig: Game_ProcessMainFrame @0x5263F0 - `cmp g_ClientPreRoundTimer` @0x52672C,
//  Entity_UpdateAllEntities @0x52674B; Entity_UpdateInfantryPlayerBody @0x4B40E0 - the
//  death edge @0x4B4BF1..0x4B4C02; NapiNPClientMsg_0x00A @0x430064]
bool run_a_preround_death_latches_when_the_round_opens() {
	Harness h;
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	h.kernel->world.rules.mp_session = true;
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	const auto deliver_dead_tail = [&](uint8_t preround) {
		FrameUpdate fu;
		fu.local_tail_present = true;
		fu.health = 0;
		fu.complete = true;
		fu.weapon.present = true;
		fu.weapon.preround_timer = preround;
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram =
				nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
	};
	deliver_dead_tail(5);
	bool ok = expect(h.role.runtime->state().preround_delay_seconds == 5 &&
			h.role.runtime->state().health_updates_applied != 0 &&
			h.role.runtime->state().local_health == 0 && !h.role.runtime->local_player_dead(),
			"pre-round death: a Health of 0 in the pre-round latches no dead bit");
	h.role.run_tick(h.input);
	ok &= expect(!h.role.runtime->local_player_dead(),
			"pre-round death: the held timer keeps the body tick off");
	deliver_dead_tail(0);
	ok &= expect(h.role.runtime->local_player_dead(),
			"pre-round death: the round's first body tick latches the dead bit");
	return ok;
}

// V-L2-s6 FOUND 4: Player_ResetLocalInput tests g_LocalPlayerEntity and its def, which a
// joiner has from its mission start's Player_InitPlayer, before L spawns at the name-match:
// a Respawn / Escape press in that window resets the devices and the event queue; the
// key-input leg waits for L. Before the mission start nothing resets. The reset's return
// owes the embedder the Scroll Lock turn-off past the same tests (D-NET-563).
// [orig: Player_ResetLocalInput @0x4B2B80 - the entity and def tests @0x4B2B85..0x4B2B8D,
//  Input_DisableScrollLock @0x4B2B91, Input_ResetAllState @0x4B2B99; Player_InitPlayer
//  @0x4E15F0 - the store @0x4E17EB]
bool run_a_pre_l_reset_takes_the_device_legs() {
	Harness unbound;
	bool ok = expect(!unbound.role.reset_local_input() && !unbound.role.take_input_reset(),
			"pre-L reset: before the mission start nothing resets, the Scroll Lock leg neither");
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	ok &= expect(h.role.runtime->local_entity_bound() && !h.role.local_spawned(),
			"pre-L reset: the local entity is bound, L is not spawned");
	ok &= expect(h.role.reset_local_input() && h.role.take_input_reset(),
			"pre-L reset: the device and queue legs, the Scroll Lock turn-off among them, run "
			"from the mission start");
	return ok;
}

// V-L2-s6 FOUND 1: a 0x0A whose recipient tail is 0 beside an ALIVE self record (a stock
// host's death frame: its 0x0A is built before its entity update raises the victim's dead
// bit) leaves the local entity alive at Health 1, as the record loop floors the word the
// tail stored; the death latches with the next 0x0A, whose dead self record stores 0.
// [orig: NapiNPClientMsg_0x00A @0x42FEC0 - the tail's store @0x4305DF;
//  NetPacket_SerializePlayerState @0x4C09C0 - the dead arm's store @0x4C10FB, `cmp edi,
//  g_LocalPlayerEntity` @0x4C11AC, the floor @0x4C11C4..0x4C11CE;
//  Entity_UpdateInfantryPlayerBody @0x4B40E0 - the death edge @0x4B4BF1..0x4B4C02;
//  Server_TickUpdate @0x5266B6 ahead of Entity_UpdateAllEntities @0x52674B]
bool run_an_alive_self_record_floors_a_zero_tail() {
	std::vector<opennova::def::DefItemDef> rows(1);
	rows[0].id = static_cast<int>(w::kPlayerInfantryTypeId) +
			static_cast<int>(opennova::mission::kItemIdOffset);
	rows[0].hp = 100;
	opennova::def::DefItemsFile items{};
	items.entries = rows.data();
	items.count = rows.size();
	Harness h;
	h.kernel->world.load_systems();
	h.kernel->set_items_table(&items);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input); // the spawn frame
	w::Entity *local = h.kernel->local.player();
	if (!expect(h.role.local_spawned() && local != nullptr && local->health > 0,
			"floor: L spawned alive")) {
		h.kernel->set_items_table(nullptr);
		return false;
	}
	h.kernel->world.rules.mp_session = true;
	{
		// The spawn stream's row (a compact record never creates one).
		replication::ClientEntityState &self = h.role.runtime->state().upsert(0x0005);
		self.cls = EntityClass::Player;
		self.type_id = w::kPlayerInfantryTypeId;
	}
	SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
	const auto deliver = [&](uint8_t record_flags) {
		FrameUpdate fu;
		fu.local_tail_present = true;
		fu.health = 0;
		fu.carried_handle = 0xFFFF;
		fu.complete = true;
		FrameUpdateRecord rec;
		rec.handle = 0x0005;
		rec.type_id = w::kPlayerInfantryTypeId;
		rec.cls = EntityClass::Player;
		rec.player.carrier_handle = 0xFFFF;
		rec.player.state_flags = record_flags;
		fu.records.push_back(rec);
		std::vector<uint8_t> packet;
		frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
				{make_protocol_message(s2c::PER_FRAME_UPDATE, encode_frame_update(fu))}, packet);
		auto datagram =
				nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
		h.role.runtime->receive(datagram.data(), datagram.size());
		h.role.run_tick(h.input);
	};
	deliver(0x00);
	bool ok = expect(h.role.runtime->state().local_health == 0 &&
			h.role.runtime->local_entity_health() == 1 && !h.role.runtime->local_player_dead(),
			"floor: a zero tail beside an alive self record latches nothing");
	ok &= expect(local->health == 1 && local->alive && (local->flags & 2u) == 0,
			"floor: L keeps a Health of 1");
	deliver(0x02);
	ok &= expect(h.role.runtime->local_entity_health() == 0 && h.role.runtime->local_player_dead(),
			"floor: the next 0x0A's dead self record latches the death");
	ok &= expect(local->health == 0, "floor: the dead record's 0 reaches L");
	h.kernel->set_items_table(nullptr);
	return ok;
}

// The deploy action on a joiner (dispatch 12) under permanent death (the S2C
// 0x08 bit 15): a player killed by a 0x0A tail at Health 0 is back in the
// deploy flow, owing a pick, and its press sends the reliable 0x51 [i16
// 0xFFFF] when spectators are allowed (bit 13) and nothing when they are not;
// the 0x0E pick it owes never ships (the listen host's own press, live and
// dead: host_role).
// [orig: Input_HandleActionBinding -- the dead bit @0x49AD7B, case 12
//  @0x49B024..0x49B03C, the convert @0x49B085..0x49B0B9;
//  NapiNPClientMsg_HandleSessionConfig @0x428386 / @0x42838C]
// D-NET-685: the deploy action's shared head runs ahead of both send arms and on
// neither of them on the spectators-off arm: the input reset (the deferred queue in the
// kernel, the devices through the role's take_input_reset) and Dialog_ResetAll, then
// the 0x0E pick or the 0x51 convert.
// [orig: Input_HandleActionBinding @0x49AD40 - the spectators-off jump @0x49B03C,
//  Input_ResetAllState @0x49B042, Dialog_ResetAll @0x49B060, the convert
//  @0x49B065..0x49B0B9, the pick from @0x49B0C3]
bool run_deploy_action_head_resets_input_and_dialogs() {
	struct Case {
		bool permanent;
		bool spectators;
		uint8_t sent; // the C2S the press sends, 0 for none
	};
	const Case cases[] = {{false, false, c2s::RESPAWN_REQUEST},
			{true, true, c2s::SPECTATOR_RESPAWN}, {true, false, 0}};
	bool ok = true;
	for (const Case &c : cases) {
		Harness h;
		h.kernel->world.load_systems();
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
		h.role.run_tick(h.input);
		h.kernel->world.rules.mp_session = true;
		SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
		const auto deliver = [&](uint8_t tag, std::vector<uint8_t> body) {
			std::vector<uint8_t> packet;
			frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
					{make_protocol_message(tag, std::move(body))}, packet);
			auto datagram =
					nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
			h.role.runtime->receive(datagram.data(), datagram.size());
			h.role.run_tick(h.input);
		};
		std::vector<uint8_t> config(47, 0);
		const uint32_t rules = (c.permanent ? 0x8000u : 0u) | (c.spectators ? 0x2000u : 0u);
		for (int i = 0; i < 4; ++i) config.push_back(static_cast<uint8_t>(rules >> (8 * i)));
		deliver(s2c::SESSION_CONFIG, config);
		FrameUpdate fu;
		fu.local_tail_present = true;
		fu.health = 0;
		fu.complete = true;
		deliver(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
		(void)h.role.take_input_reset();
		(void)h.kernel->world.script.dialog.enqueue("dlg001", {});
		if (!expect(!h.kernel->world.script.dialog.history().empty(),
				"deploy head: a dialog is registered ahead of the press"))
			return false;
		h.socket.datagrams.clear();
		const bool queued = h.role.request_deployment_pick(0xFFFFu);
		const bool head = c.sent != 0;
		ok &= expect(queued == head, "deploy head: the press queues on its send arms");
		ok &= expect(h.role.take_input_reset() == head,
				"deploy head: the input reset runs on both send arms, never on the spectators-off arm");
		ok &= expect(h.kernel->world.script.dialog.history().empty() == head,
				"deploy head: Dialog_ResetAll runs with it");
		for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
		ProtocolMessage m;
		if (head)
			ok &= expect(h.socket.last_message(c.sent, m),
					"deploy head: the arm's message follows the head");
	}
	return ok;
}

bool run_deploy_action_converts_a_dead_permanent_death_player() {
	bool ok = true;
	for (const bool spectators : {true, false}) {
		Harness h;
		h.kernel->world.load_systems();
		h.role.poll_preload();
		h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
		                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
		h.role.run_tick(h.input);
		h.kernel->world.rules.mp_session = true;
		SessionSequencing seq = inmatch::make_jo_game_session_sequencing();
		const auto deliver = [&](uint8_t tag, std::vector<uint8_t> body) {
			std::vector<uint8_t> packet;
			frame_session_packet(seq, SessionCrypto{kServerScrk, {}, kClientKey},
					{make_protocol_message(tag, std::move(body))}, packet);
			auto datagram =
					nw_encode_outbound(SESSION_OPCODE_SERVER_PROTOCOL_MESSAGE, std::move(packet));
			h.role.runtime->receive(datagram.data(), datagram.size());
			h.role.run_tick(h.input);
		};
		// The 0x08 record: 10 rule dwords, 7 bytes, the rules word.
		std::vector<uint8_t> config(47, 0);
		const uint32_t rules = 0x8000u | (spectators ? 0x2000u : 0u);
		for (int i = 0; i < 4; ++i) config.push_back(static_cast<uint8_t>(rules >> (8 * i)));
		deliver(s2c::SESSION_CONFIG, config);
		const replication::ClientState &cs = h.role.runtime->state();
		ok &= expect(cs.permanent_death && cs.spectators_allowed == spectators,
				"deploy action: the 0x08 latches permanent death and the spectator bit");
		// The death edge: a 0x0A tail at Health 0 returns the joiner to the
		// deploy flow (JoinerConnection::begin_redeployment), owing a pick.
		FrameUpdate fu;
		fu.local_tail_present = true;
		fu.health = 0;
		fu.complete = true;
		deliver(s2c::PER_FRAME_UPDATE, encode_frame_update(fu));
		ok &= expect(h.role.runtime->local_player_dead() &&
				h.role.runtime->awaiting_deploy_pick(),
				"deploy action: the death edge reads dead and owes a pick");
		ProtocolMessage m;
		const bool queued = h.role.request_deployment_pick(0xFFFFu);
		for (int i = 0; i < 3; ++i) h.role.run_tick(h.input);
		const bool convert = h.socket.last_message(c2s::SPECTATOR_RESPAWN, m) &&
				m.payload == std::vector<uint8_t>{0xFF, 0xFF};
		ok &= expect(queued == spectators && convert == spectators,
				spectators ? "deploy action: a dead player under permanent death sends 0x51 [0xFFFF]"
				           : "deploy action: with spectators off a dead player sends nothing");
		ok &= expect(!h.socket.last_message(c2s::RESPAWN_REQUEST, m) &&
				h.role.runtime->awaiting_deploy_pick(),
				"deploy action: the owed 0x0E pick never ships");
	}
	return ok;
}

} // namespace

// A pool-1 shooter's round takes the Indoors bit retail's client keeps for the
// entity, which its BB walk rebuilds on each think visit (the def type picks
// the walk: the candidate slice for type 1, none for type 5, the static
// building prefix otherwise). Each shooter's 0x0D carries 0x800000, and the
// rounds come 20 ticks after it, past the 17-tick candidate-slice edge:
// - the cveh vehicle: a row a 0x0D lands after the mission start's class-init
//   pass has no brain (+0x64; only that init builds one), so its state machine
//   returns before re-arming +0x2AC and the think gate passes on every visit;
//   its first visits walk the slice the 0x0D memset emptied, and once the edge
//   fills it the walk finds the box;
// - the type-5 row: no walk on any visit, so the bit stays clear;
// - the type-6 object, Null class: its first visit, in the 0x0D frame's pass,
//   walks the static prefix, which already holds the box, and NullClass then
//   freezes its clock, so the bit stays set.
// [orig: NapiNPClientMsg_0x00D - the memset @0x432D1B..0x432D23, the Flags
//  store @0x432D7D, the callbacks @0x433301 / @0x43331A; Entity_UpdatePool1Slot
//  - the gate @0x4B8E1B..0x4B8E22, the walk @0x4B8E25, the class call @0x4B8E3C;
//  EntityAI_ProcessGroundStateMachine - the brain test @0x4583CA..0x4583D1;
//  NullClass_HandleEvent @0x406FF4; Entity_BuildProximityList - the type-5 test
//  @0x4B3E4A..0x4B3E4D, the empty-slice exit @0x4B3F46; the slice rebuild call
//  @0x4C2418; RoundData_SpawnRound @0x4EC830]
bool run_pool1_shooter_round_takes_its_indoors_state() {
	Harness h;
	auto &world = h.kernel->world;
	world.registry.configure_pool(1, 16);
	world.registry.configure_pool(2, 4);
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, 0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	// The items table: the player, a cveh vehicle (type 1), a building-type
	// item (type 5) and an object (type 6), the last two Null class, none
	// naming a graphic.
	def::DefItemDef defs[4]{};
	defs[0].id = w::kPlayerInfantryTypeId + mission::kItemIdOffset;
	defs[0].type = 3;
	defs[0].hp = 100;
	defs[1].id = 1291 + mission::kItemIdOffset;
	defs[1].type = 1;
	std::strcpy(defs[1].ai_function, "cveh"); // the shipped vehicle class
	defs[2].id = 1292 + mission::kItemIdOffset;
	defs[2].type = 5;
	defs[3].id = 1293 + mission::kItemIdOffset;
	defs[3].type = 6;
	def::DefItemsFile items{defs, 4};
	h.kernel->set_items_table(&items);
	h.kernel->resolve_collision_instances();
	// A static building whose blink box spans (10, -20, 0) +- 5 u.
	w::Entity building;
	building.kind = w::EntityKind::Building;
	building.item_type = 5;
	building.position = {10.0f, -20.0f, 0.0f};
	building.bound_radius = 30.0f;
	const w::EntityHandle bh = world.registry.spawn(2, building);
	w::CollisionModel model;
	w::CollisionVolume bb;
	bb.type = 8;
	bb.flags = 0; // flags ^ 6 includes the indoor bit
	bb.min_x = bb.min_y = bb.min_z = -5 * 65536;
	bb.max_x = bb.max_y = bb.max_z = 5 * 65536;
	model.volumes.push_back(bb);
	w::CollisionSection sec;
	sec.volume_count = 1;
	model.sections.push_back(sec);
	h.kernel->collision.assign_entity(bh, h.kernel->collision.add_model(std::move(model)));
	h.kernel->collision.build_initial_tables(world);
	world.tables.ammo.entries.resize(1);
	w::AmmoTableEntry &ammo = world.tables.ammo.entries[0];
	ammo.valid = true;
	ammo.velocity = 620;
	ammo.max_age_ticks = 100;
	world.tables.weapons.entries.resize(2);
	world.tables.weapons.entries[1].valid = true;
	world.tables.weapons.entries[1].ammo_index = 0;
	struct Case {
		uint16_t handle;
		uint16_t type_id;
		int32_t x;
		bool indoors;
		const char *what;
	};
	const Case cases[] = {
		{0x1003, 1291, 10, true, "a cveh vehicle inside a BB: its candidate walk finds the box"},
		{0x1004, 1291, 60, false, "a cveh vehicle outside any BB: the walk clears the bit"},
		{0x1005, 1292, 10, false, "a type-5 row runs no walk: the bit stays clear"},
		{0x1006, 1293, 10, true, "a Null-class object inside a BB: its first walk set the bit"},
	};
	PoolSpawnBatch batch;
	for (const Case &c : cases) {
		PoolSpawnRecord rec;
		rec.spawn_flags = 0x0020; // the Flags dword rides
		rec.slot_id = c.handle;
		rec.item_type_id = c.type_id;
		rec.pos_x = c.x << 16;
		rec.pos_y = -20 << 16;
		rec.pos_z = 2 << 16;
		rec.entity_flags = w::kEntityFlagIndoors;
		batch.records.push_back(rec);
	}
	h.role.runtime->view().apply(s2c::POOL_SPAWN, encode_pool_spawn_batch(batch));
	// Ticks after the 0x0D: past the 17-tick candidate-slice edge, so the
	// vehicle's slice holds the building.
	for (int t = 0; t < 20; ++t) h.role.run_tick(h.input);
	bool ok = true;
	for (const Case &c : cases) {
		ok &= expect(h.role.runtime->state().find(c.handle) != nullptr, "the 0x0D row landed");
		world.round_sim.reset();
		FrameUpdate frame;
		frame.carried_handle = 0xFFFF;
		frame.health = 100;
		RoundEventRecord ev;
		ev.flags = kRoundEventFlagAdmIndexed;
		ev.adm_index = 1;
		ev.shooter_handle = c.handle;
		frame.round_events.push_back(ev);
		h.role.runtime->view().apply(s2c::PER_FRAME_UPDATE, encode_frame_update(frame));
		const w::LiveRound *round = nullptr;
		for (const w::LiveRound &r : world.round_sim.rounds)
			if (r.active) round = &r;
		ok &= expect(round != nullptr, "the round event spawned its round");
		if (round == nullptr) continue;
		ok &= expect(round->indoors_word == c.indoors && round->indoors_flags == c.indoors, c.what);
	}
	return ok;
}

// A decoded shooter's stance row (A-27's joiner half): Parachute 0x20, Drowning
// 0x8000 and LadderContact 0x100000 in the row's Flags dword force the standing
// row 2, a crouched body otherwise keeps row 1; Drowning also takes the recoil's
// x4 leg. Observed through the ammo recoil each row's spawn adds to the row.
// [orig: RoundData_SpawnRound `test dword ptr [ebp+24h], 108020h` @0x4EC2D5,
//  row 2 @0x4EC2EC; the single round's recoil row and `test ecx, 8000h`
//  @0x4EC85B..0x4EC8A3]
bool run_round_event_stance_reads_the_flags_legs() {
	Harness h;
	auto &world = h.kernel->world;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, 0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	world.tables.ammo.entries.resize(1);
	w::AmmoTableEntry &ammo = world.tables.ammo.entries[0];
	ammo.valid = true;
	ammo.velocity = 620;
	ammo.max_age_ticks = 100;
	ammo.recoil[0] = 1; // prone
	ammo.recoil[1] = 2; // crouched / mounted
	ammo.recoil[2] = 3; // standing / the override
	world.tables.weapons.entries.resize(2);
	world.tables.weapons.entries[1].valid = true;
	world.tables.weapons.entries[1].ammo_index = 0;
	int crouch = -1;
	for (int st = 0; st < w::kInfantryAnimStateCount && crouch < 0; ++st)
		if ((w::infantry_anim_flags(st) & (w::kAnimStanceFlagCrouched | w::kAnimStanceFlagProne)) ==
				w::kAnimStanceFlagCrouched)
			crouch = st;
	if (!expect(crouch >= 0, "a crouched anim state exists")) return false;
	// The row's Flags dword is the motor mirror with the latest record's byte
	// folded in under 0xFD: a spawn word's stale Parachute or Drowning (0x8020
	// seeded the mirror, the motor cleared 0x8000, the record carries 0) is gone.
	// [orig: NetPacket_SerializePlayerState @0x4C1307..0x4C1311]
	struct Case {
		uint8_t state_flags;
		uint32_t mirror_flags;
		uint32_t spawn_flags;
		int32_t recoil;
		const char *what;
	};
	const Case cases[] = {
		{0, 0, 0, 2 << 18, "a crouched shooter keeps the crouched row"},
		{0x20, 0, 0, 3 << 18, "a parachuting shooter takes the standing row"},
		{0, 0x100000, 0, 3 << 18, "a shooter on a ladder takes the standing row"},
		{0, 0x8000, 0, 3 << 20, "a drowning shooter takes the standing row and the x4 leg"},
		{0, 0x20, 0x8020, 2 << 18, "a spawn word's parachute and drowning do not outlive the record"},
	};
	bool ok = true;
	constexpr uint16_t kShooter = 0x0003;
	for (const Case &c : cases) {
		auto &row = h.role.runtime->state().upsert(kShooter);
		row.type_id = w::kPlayerInfantryTypeId;
		row.cls = EntityClass::Player;
		row.anim_state_id = crouch;
		row.carrier_handle = 0xFFFF;
		row.state_flags = c.state_flags;
		row.state_flags_known = true;
		row.rm_entity_flags = c.mirror_flags;
		row.spawn_entity_flags = c.spawn_flags;
		row.recoil_pitch = 0;
		world.round_sim.reset();
		FrameUpdate frame;
		frame.carried_handle = 0xFFFF;
		frame.health = 100;
		RoundEventRecord ev;
		ev.flags = kRoundEventFlagAdmIndexed;
		ev.adm_index = 1;
		ev.shooter_handle = kShooter;
		frame.round_events.push_back(ev);
		h.role.runtime->view().apply(s2c::PER_FRAME_UPDATE, encode_frame_update(frame));
		const auto *after = h.role.runtime->state().find(kShooter);
		ok &= expect(after != nullptr && after->recoil_pitch == c.recoil, c.what);
	}
	return ok;
}

// An alt-fire (bit 0) round event's byte is an ammo index: the joiner flies that
// ammo with no weapon, so the spawn's spread takes the ammo's ERROR leg, never the
// round of a weapon at that index. [orig: NetPacket_DeserializeRoundEvent @0x42F521,
//  @0x42F52C..0x42F5BA; RoundData_SpawnRound's null-weapon test @0x4EC38D..0x4EC397,
//  the ammo leg @0x4EC4BF..0x4EC4D2]
bool run_alt_round_event_flies_its_ammo_byte() {
	Harness h;
	auto &world = h.kernel->world;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, 0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	world.tables.ammo.entries.resize(3);
	for (w::AmmoTableEntry &ammo : world.tables.ammo.entries) {
		ammo.valid = true;
		ammo.velocity = 620;
		ammo.max_age_ticks = 100;
	}
	world.tables.ammo.entries[2].spread_error_fp16 = 30 << 16;
	// A loaded table's slot 0 is the engine's valid "null" def; weapon 2 flies ammo 0.
	world.tables.weapons.entries.resize(3);
	world.tables.weapons.entries[0].name = "null";
	world.tables.weapons.entries[0].valid = true;
	world.tables.weapons.entries[2].name = "ADM_TWO";
	world.tables.weapons.entries[2].valid = true;
	world.tables.weapons.entries[2].ammo_index = 0;
	constexpr uint16_t kShooter = 0x0003;
	auto &row = h.role.runtime->state().upsert(kShooter);
	row.type_id = w::kPlayerInfantryTypeId;
	row.cls = EntityClass::Player;
	row.carrier_handle = 0xFFFF;
	world.round_sim.reset();
	FrameUpdate frame;
	frame.carried_handle = 0xFFFF;
	frame.health = 100;
	RoundEventRecord ev;
	ev.flags = kRoundEventFlagAltFire;
	ev.adm_index = 2;
	ev.shot_seq = 1;
	ev.shooter_handle = kShooter;
	frame.round_events.push_back(ev);
	h.role.runtime->view().apply(s2c::PER_FRAME_UPDATE, encode_frame_update(frame));
	const w::LiveRound *round = nullptr;
	for (const w::LiveRound &r : world.round_sim.rounds)
		if (r.active) round = &r;
	if (!expect(round != nullptr && round->ammo_index == 2,
			"the alt round event flies ammo 2, its byte"))
		return false;
	return expect(round->vel.y != 0.0f || round->vel.z != 0.0f,
			"the weaponless alt round spreads by its ammo's ERROR");
}

// A round event whose shooter resolves to nothing on this client spawns nothing; the
// same event from a decoded shooter flies. [orig: NetPacket_DeserializeRoundEvent
//  @0x42F451..0x42F4A1]
bool run_round_event_needs_a_resolved_shooter() {
	Harness h;
	auto &world = h.kernel->world;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, 0x0005, w::kPlayerInfantryTypeId);
	h.role.run_tick(h.input);
	world.tables.ammo.entries.resize(1);
	world.tables.ammo.entries[0].valid = true;
	world.tables.ammo.entries[0].velocity = 620;
	world.tables.ammo.entries[0].max_age_ticks = 100;
	world.tables.weapons.entries.resize(2);
	world.tables.weapons.entries[1].valid = true;
	world.tables.weapons.entries[1].ammo_index = 0;
	constexpr uint16_t kShooter = 0x0006;
	const auto fire = [&]() {
		world.round_sim.reset();
		FrameUpdate frame;
		frame.carried_handle = 0xFFFF;
		frame.health = 100;
		RoundEventRecord ev;
		ev.flags = kRoundEventFlagAdmIndexed;
		ev.adm_index = 1;
		ev.shooter_handle = kShooter;
		frame.round_events.push_back(ev);
		h.role.runtime->view().apply(s2c::PER_FRAME_UPDATE, encode_frame_update(frame));
		return world.round_sim.active_count;
	};
	if (!expect(fire() == 0, "an unresolved shooter's round event spawns nothing")) return false;
	auto &row = h.role.runtime->state().upsert(kShooter);
	row.type_id = w::kPlayerInfantryTypeId;
	row.cls = EntityClass::Player;
	row.carrier_handle = 0xFFFF;
	return expect(fire() == 1, "a decoded shooter's round event flies");
}

// The joiner's sniper-zoom rule follows the S2C 0x08 flags dword's bit 16.
// [orig: NapiNPClientMsg_HandleSessionConfig @0x42837A..0x428392]
bool run_zoom_rule_follows_the_session_flags() {
	Harness h;
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId);
	w::World &world = h.kernel->world;
	h.role.runtime->state().session_rules_flags = 0x10000u;
	h.role.run_tick(h.input);
	if (!expect(world.rules.allow_sniper_scope_zoom, "zoom rule: bit 16 allows the zoom"))
		return false;
	h.role.runtime->state().session_rules_flags = 0x8000u;
	h.role.run_tick(h.input);
	return expect(!world.rules.allow_sniper_scope_zoom, "zoom rule: bit 16 clear locks it");
}

// D-NET-602: the role hands every runtime it creates the process's 0x4C count, a
// rebuild included: a count standing at 300 reports on the 11th game-loop frame.
// [orig: Client_ProcessNetworkFrame @0x42C180 - the step @0x42C22D..0x42C239, the
//  zero @0x42C246; g_NetQualityReportTimer @0xA85B84]
bool run_role_runtimes_step_the_process_report_count() {
	Harness h;
	inmatch::ClientProcessState process;
	process.net_quality_report_timer = 300;
	h.role.use_process_state(&process);
	h.role.create_runtime("BridgeJoiner", inmatch::JoinRole::Player, "", "");
	h.kernel->world.load_systems();
	h.role.poll_preload();
	h.role.runtime->seed_session(kSessionId, kClientKey, kClientScrk, kServerScrk,
	                             1, 0, /*self_handle=*/0x0005, w::kPlayerInfantryTypeId, 0,
	                             0, /*replay_mode=*/false);
	int first = 0;
	for (int frame = 1; frame <= 20 && first == 0; ++frame) {
		h.socket.datagrams.clear();
		h.role.run_tick(h.input);
		ProtocolMessage report;
		if (h.socket.last_message(0x4C, report)) first = frame;
	}
	return expect(first == 11, "the role's runtime reports at the process count's phase");
}

int main() {
	if (!run_local_replica_turret_channel()) return 1;
	if (!run_replica_turret_channel()) return 1;
	if (!run_received_loadout_policy_and_sounds()) return 1;
	if (!run_received_reload_refills_an_eweap_entity()) return 1;
	bool ok = true;
	ok &= run_pre_match_frame();
	ok &= run_preload_frame();
	ok &= run_in_match_spawn_edge();
	ok &= run_spawn_edge_latch_reads_self_team();
	ok &= run_spawn_edge_latch_reads_the_frames_target_team();
	ok &= run_spawn_edge_takes_a_self_team_assign();
	ok &= run_in_match_spawn_binds_local_player_definition();
	ok &= run_self_handle_lifecycle();
	ok &= run_spawn_stamps_equipped_adm_from_midframe_grant();
	ok &= run_reset_for_join();
	ok &= run_remote_vehicle_occupancy();
	ok &= run_request_turns_l_before_the_0x26();
	ok &= run_confirmed_vehicle_drive(0);
	ok &= run_confirmed_vehicle_drive(1);
	ok &= run_confirmed_vehicle_drive(2);
	ok &= run_confirmed_vehicle_drive(3);
	ok &= run_confirmed_vehicle_drive(4);
	ok &= run_confirmed_vehicle_drive(0, true);
	ok &= run_confirmed_vehicle_drive(0, true, true);
	ok &= run_full_bms_child_gun_mount();
	ok &= run_wire_proxy_twin_takes_the_owned_lifetime(true);
	ok &= run_wire_proxy_twin_takes_the_owned_lifetime(false);
	ok &= run_dead_joiner_keeps_its_wire_proxies();
	ok &= run_flare_descriptor_carries_the_pilot_handheld();
	ok &= run_guided_fire_preserves_selected_target();
	ok &= run_rules_stamp_from_mp_attributes(0x10000u, true);
	ok &= run_rules_stamp_from_mp_attributes(0x3A02u, false);
	ok &= run_uplink_carries_same_frame_input();
	ok &= run_uplink_carries_the_frame_statistics();
	ok &= run_uplink_dead_test_is_the_builders();
	ok &= run_stance_follows_the_authority_echo();
	ok &= run_holdoff_window_taps_reach_the_boundary_uplink();
	ok &= run_end_round_header_holds_the_entity_update();
	ok &= run_world_state_load_resnaps_local_pose();
	ok &= run_revive_deploy_respawns_l_on_its_record_edge();
	ok &= run_tail_armor_lands_on_l();
	ok &= run_tail_carried_word_lands_on_l();
	ok &= run_entity_death_names_l();
	ok &= run_a_weapon_loadout_rebuilds_the_process_name_list();
	ok &= run_the_person_pass_keys_the_class_row();
	ok &= run_a_seated_death_sends_the_seat_leave();
	ok &= run_redeploy_release_rebuilds_the_candidate_slices();
	ok &= run_respawn_key_resets_input_and_sends_0x41();
	ok &= run_respawn_key_before_the_first_tail();
	ok &= run_a_preround_death_latches_when_the_round_opens();
	ok &= run_an_alive_self_record_floors_a_zero_tail();
	ok &= run_a_pre_l_reset_takes_the_device_legs();
	ok &= run_a_console_action_dispatches_by_code();
	ok &= run_a_stance_select_and_the_look_take_the_rows_head();
	ok &= run_the_console_quit_stores_the_reset_exit();
	ok &= run_the_mission_start_leaves_l_hidden();
	ok &= run_a_lost_link_at_round_over_runs_input_action_3();
	ok &= run_a_console_deploy_pick_ends_the_drain();
	ok &= run_a_menu_screen_holds_the_console_queue_and_its_reset_drops_it();
	ok &= run_the_teardown_goodbye_follows_the_exit_route();
	ok &= run_the_join_screen_abort_bursts_its_state_record(false, "N.C:SWWAD[1]:-1");
	ok &= run_the_join_screen_abort_bursts_its_state_record(true, "N.C:SWWAD[1]:-2");
	ok &= run_the_spawn_takes_the_mission_starts_identity();
	ok &= run_the_spawn_takes_its_sides_class_word();
	ok &= run_the_self_record_rebinds_ls_camouflage_row();
	ok &= run_a_remote_record_repicks_its_rows_camouflage_row();
	ok &= run_a_self_team_assign_reinits_l();
	ok &= run_deploy_action_converts_a_dead_permanent_death_player();
	ok &= run_deploy_action_head_resets_input_and_dialogs();
	ok &= run_pool1_shooter_round_takes_its_indoors_state();
	ok &= run_round_event_stance_reads_the_flags_legs();
	ok &= run_alt_round_event_flies_its_ammo_byte();
	ok &= run_round_event_needs_a_resolved_shooter();
	ok &= run_zoom_rule_follows_the_session_flags();
	ok &= run_role_runtimes_step_the_process_report_count();
	if (!ok) return 1;
	std::printf("joiner_role_test: OK\n");
	return 0;
}
