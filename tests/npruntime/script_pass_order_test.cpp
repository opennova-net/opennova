// The script pass's sends at their calls (D-NET-NEW-V3-1): retail queues each record
// the WAC tick and the BMS quarter pass raise at its call, so a WAC Win's or a BMS
// Win's 0x61 / 0x1D leads a record a later call raises in the same frame, the WAC
// VM's 0x23 leads the scoreboard boundary's 0x16, and a script-ended round's slot
// is in state 7 before the same frame's violation sweep (D-NET-NEW-L1-16).
// [orig: WacScript_ExecuteBytecode @0x4F58B0 (SendFiltered(0x23) @0x4F5ED1 ahead of
//  the call switch @0x4F5EF6); WacAction_Win @0x4ED4A0 (the Server_ProcessRoundEnd
//  call @0x4ED4A5); EventTrigger_UpdateEntry @0x454C30 (EventAction_Dispatch per
//  action @0x454CA6); EventAction_Dispatch @0x4542E0 (RedWin's call @0x454495);
//  Server_TickUpdate: the WAC tick @0x51D8BF, the quarter pass @0x51D8F4, the
//  scoreboard @0x51D90D, the win check @0x51DF5A, the sweep @0x51DF5F]
#include <runtime/inmatch/loopback_channel.h>
#include <runtime/inmatch/napi_np_connection.h>
#include <runtime/inmatch/napi_np_server_ctx.h>
#include <runtime/inmatch/punt_log.h>
#include <runtime/inmatch/server_files.h>
#include <runtime/inmatch/server_tick.h>
#include <runtime/mission/event_runtime.h>
#include <runtime/wac/compiler.h>
#include <runtime/wac/wac_system.h>
#include <runtime/world/world.h>

#include <net/npwire/ingame_message_id.h>

#include <base/gameprofile/game_type.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "conn_fixture.h"
#include "../common/test_expect.h"

namespace {

using namespace opennova;
namespace inmatch = opennova::inmatch;
namespace ns = opennova::replication;
namespace w = opennova::world;

int &failures = test_expect::failures;
using test_expect::expect;

w::EntityHandle match_player(w::World &world, uint8_t slot, uint8_t team, const char *name) {
	w::Entity e;
	e.kind = w::EntityKind::Organic;
	e.item_type_index = 3;
	e.player_class = 8;
	e.team = team;
	e.health = 100;
	e.alive = true;
	e.flags = w::kEntityFlagPlayer;
	e.engine_flags = w::kEntityFlagPlayer;
	const w::EntityHandle handle = world.registry.spawn(0, e);
	world.match.upsert_player({handle, slot, name});
	world.match.set_player_slot_team(handle, team);
	return handle;
}

// One remote player in a Co-op session (no automatic win condition: the
// script owns the round end).
struct Session {
	std::unique_ptr<w::World> world = std::make_unique<w::World>();
	ns::LoopbackChannel wire;
	inmatch::NapiNPServerCtx ctx;
	w::EntityHandle player;

	Session() {
		world->registry.configure_pool(0, 4);
		world->registry.configure_pool(1, 4);
		world->rules.mp_session = true;
		w::MatchRules rules;
		rules.game_type = game_type::kCoop;
		world->match.configure(rules);
		player = match_player(*world, 4, 2, "Red");
		ctx.world = world.get();
		ctx.is_authority = 1;
		ctx.is_in_session = 1;
		ctx.config.game_type = game_type::kCoop;
		ctx.np_protocol.connection_list.push_back(conn_fixture::make_conn(
				3, 1, &wire, ns::TransportMode::Client, player, true));
		inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list.front();
		conn.reply.player_slot = 4;
		conn.admission_stage = inmatch::GameAdmissionStage::Complete;
	}

	std::vector<uint8_t> drain_tags() {
		std::vector<uint8_t> tags;
		ns::Datagram datagram;
		while (wire.client_recv(datagram)) tags.push_back(datagram.tag);
		return tags;
	}
};

// A _PUNT.TXT sink in memory (the violation sweep's "#S>9" lines).
class MemoryFiles final : public inmatch::ServerFileSink {
public:
	std::map<std::string, std::string> files;
	bool exists(const std::string &name) override { return files.count(name) != 0; }
	void remove(const std::string &name) override { files.erase(name); }
	bool write(const std::string &name, std::string_view bytes, bool append) override {
		std::string &f = files[name];
		if (!append) f.clear();
		f.append(bytes.data(), bytes.size());
		return true;
	}
	bool any_line(const char *needle) const {
		for (const auto &file : files)
			if (file.second.find(needle) != std::string::npos) return true;
		return false;
	}
};

size_t index_of(const std::vector<uint8_t> &tags, uint8_t tag) {
	for (size_t i = 0; i < tags.size(); ++i)
		if (tags[i] == tag) return i;
	return tags.size();
}

// A BMS trigger whose first action is RedWin and whose second shows a win
// subgoal: the round end's 0x61 / 0x1D go out at the RedWin's latch, the 0x3F
// notice at the next action's call. [orig: EventTrigger_UpdateEntry @0x454C30
// (EventAction_Dispatch @0x454CA6 per action); EventAction_Dispatch case 35
// (HUD_ShowObjectiveNotification @0x4546E2 -> Server_BroadcastEntityActionPacket
// @0x5080D0, SendFiltered(0x3F) @0x508199)]
void test_bms_win_leads_a_later_action_of_its_trigger() {
	Session s;
	mission::BmsEventSystem bms;
	bms::Event event{};
	event.action_count = 2;
	bms::Action win{};
	win.action_type = bms::ActionType::RedWin;
	bms::Action notice{};
	notice.action_type = bms::ActionType::ShowWinSubgoal;
	notice.param1 = 1;
	notice.param2 = 1;
	bms.load({event}, {}, {win, notice});
	s.world->add_system(&bms);
	s.world->load_systems();

	// The quarter pass reaches the one event on the 16th tick.
	for (int tick = 1; tick < 16; ++tick) inmatch::Server_TickUpdate(s.ctx);
	expect(!s.world->match.outcome().ended, "BMS: no round end before the trigger fires");
	s.drain_tags();
	inmatch::Server_TickUpdate(s.ctx);
	const std::vector<uint8_t> tags = s.drain_tags();
	const size_t seed = index_of(tags, s2c::TICK_SEED);
	const size_t header = index_of(tags, s2c::END_ROUND_HEADER);
	const size_t relay = index_of(tags, s2c::OBJECTIVE_NOTIFICATION);
	expect(s.world->match.outcome().ended && s.world->match.outcome().winner_team == 2,
			"BMS: RedWin ends the round for team 2");
	expect(seed < header && header < tags.size(), "BMS: the 0x61 then the 0x1D reach the slot");
	expect(relay < tags.size(), "BMS: the later action's 0x3F reaches the slot");
	expect(header < relay, "BMS: the RedWin's 0x61 / 0x1D lead the next action's 0x3F");
}

// A WAC program that wins, then runs a replicated command: the round end
// announces at WacAction_Win's latch, and the later command's 0x23 follows it.
// [orig: WacAction_Win @0x4ED4A5; SendFiltered(0x23, mask 0x90) @0x4F5ED1 for the
//  next statement; the 0x80 leg passes a state-7 slot @0x4C893E]
void test_wac_win_leads_a_later_replicated_command() {
	Session s;
	wac::WacSystem wac_system;
	wac::CompileEnv env;
	auto program = wac::compile_source("win(2)\ntext#(after,1)\n", env);
	expect(program.ok(), "WAC: the win-then-text program compiles");
	wac_system.set_program(std::move(program));
	s.world->add_system(&wac_system);
	s.world->load_systems();

	for (int tick = 1; tick < wac::WacSystem::kTicksPerExecution; ++tick)
		inmatch::Server_TickUpdate(s.ctx);
	expect(!s.world->match.outcome().ended, "WAC: no round end before the program runs");
	s.drain_tags();
	inmatch::Server_TickUpdate(s.ctx);
	const std::vector<uint8_t> tags = s.drain_tags();
	const size_t header = index_of(tags, s2c::END_ROUND_HEADER);
	const size_t command = index_of(tags, s2c::SCRIPT_REMOTE_COMMAND);
	expect(s.world->match.outcome().ended, "WAC: win(2) ends the round");
	expect(header < tags.size() && command < tags.size(),
			"WAC: the slot hears the round end and the replicated command");
	expect(index_of(tags, s2c::TICK_SEED) < header && header < command,
			"WAC: the win's 0x61 / 0x1D lead the later command's 0x23");
}

// Move (b): a scoreboard-boundary frame whose WAC tick replicates a command:
// the pass's 0x23 is queued ahead of the boundary's 0x16. [orig: Server_TickUpdate
// - WacScript_AdvanceTick @0x51D8BF ahead of Server_BuildAndBroadcastScoreboard
// @0x51D90D (its timer 0x136 @0x51D901)]
void test_wac_command_leads_the_scoreboard() {
	Session s;
	wac::WacSystem wac_system;
	wac::CompileEnv env;
	auto program = wac::compile_source("text#(board,1)\n", env);
	expect(program.ok(), "board: the text program compiles");
	wac_system.set_program(std::move(program));
	s.world->add_system(&wac_system);
	s.world->load_systems();

	for (int tick = 1; tick < wac::WacSystem::kTicksPerExecution; ++tick)
		inmatch::Server_TickUpdate(s.ctx);
	s.drain_tags();
	// The boundary's timer stands one short of its fire on the WAC frame.
	s.ctx.scoreboard_broadcast_timer = 0x136u;
	inmatch::Server_TickUpdate(s.ctx);
	const std::vector<uint8_t> tags = s.drain_tags();
	const size_t command = index_of(tags, s2c::SCRIPT_REMOTE_COMMAND);
	const size_t board = index_of(tags, s2c::PLAYER_LIST);
	expect(command < tags.size() && board < tags.size(),
			"board: the frame carries the 0x23 and the 0x16");
	expect(command < board, "board: the script pass's 0x23 leads the scoreboard's 0x16");
}

// The WAC / BMS state-7 leg: a pre-round second shifts the WAC divider onto
// the periodic second, so the WAC win and the violation sweep share a frame;
// the win's latch puts the slot in state 7 inside the pass, so the sweep walks
// nobody (no suicide punt for ten suicides) and the round end leads the flag
// refresh's 0x2F. [orig: Server_ProcessRoundEnd slot state 7 @0x51685E;
// Server_CheckPlayerViolations @0x51ABD0 (the state test, the `> 9` compare
// @0x51ACED); the sweep @0x51DF5F and the refresh @0x51DF64]
void test_wac_win_on_the_periodic_second_skips_the_sweep() {
	Session s;
	MemoryFiles files;
	inmatch::PuntLog punt_log(files);
	punt_log.set_time_text([] { return std::string("T"); });
	punt_log.arm_punt_log();
	s.ctx.logs.punt = &punt_log;
	s.ctx.config.max_friendly_kills = 3;
	w::Entity flag;
	flag.kind = w::EntityKind::Item;
	flag.item_id = 4091;
	flag.has_item_def = true;
	flag.alive = true;
	// Two flags: the round-robin refresh sends one each second (the first
	// second's flag 0, this second's flag 1).
	expect(s.world->registry.spawn(1, flag).valid() && s.world->registry.spawn(1, flag).valid(),
			"sweep: the flags spawn");
	wac::WacSystem wac_system;
	wac::CompileEnv env;
	auto program = wac::compile_source("win(2)\n", env);
	expect(program.ok(), "sweep: the win program compiles");
	wac_system.set_program(std::move(program));
	s.world->add_system(&wac_system);
	s.world->load_systems();
	s.world->preround_delay_seconds = 1;

	// Frame 1 is the pre-round second (the WAC divider holds); the divider
	// counts from frame 2, so its 62nd count is frame 63, the second periodic
	// second. The ten suicides land after the first second's sweep.
	for (int frame = 1; frame < 63; ++frame) inmatch::Server_TickUpdate(s.ctx);
	expect(!s.world->match.outcome().ended, "sweep: no round end before the WAC frame");
	s.world->match.player(s.player)->stats[w::MatchStats::kSuicides] = 10;
	s.drain_tags();
	inmatch::Server_TickUpdate(s.ctx);
	const std::vector<uint8_t> tags = s.drain_tags();
	const inmatch::NapiNPConnection &conn = s.ctx.np_protocol.connection_list.front();
	expect(s.world->match.outcome().ended && s.world->match.periodic_second(),
			"sweep: the WAC win lands on a periodic second");
	expect(conn.slot_state == inmatch::PlayerSlotState::RoundOver && !conn.host_disconnect_sent &&
					!files.any_line("#S>9"),
			"sweep: the slot is in state 7 and the sweep punts nobody");
	const size_t header = index_of(tags, s2c::END_ROUND_HEADER);
	const size_t refresh = index_of(tags, s2c::OBJECTIVE_ENTITY_STATE);
	expect(header < tags.size() && refresh < tags.size(),
			"sweep: the frame carries the round end and the flag refresh");
	expect(header < refresh, "sweep: the round end leads the flag refresh's 0x2F");
}

} // namespace

int main() {
	test_bms_win_leads_a_later_action_of_its_trigger();
	test_wac_win_leads_a_later_replicated_command();
	test_wac_command_leads_the_scoreboard();
	test_wac_win_on_the_periodic_second_skips_the_sweep();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	std::printf("npruntime_script_pass_order: ok\n");
	return 0;
}
