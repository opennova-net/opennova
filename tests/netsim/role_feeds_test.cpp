// The role feeds (inmatch/role_feeds.h), pinned where they used to live in
// the Simulation binding (ADR 0040 ladder E8): the end-of-round session state
// and overlay input off the folded 0x1D header, the stat rows joined to the
// roster with the local row resolved through the board index, the tab
// filter, the DEATH screen status off the joiner's 0x0A / 0x6E facts, and the
// bare-role fallbacks.
// [orig: NapiNPClientMsg_0x01D @0x430840; populate_stat_results_list
//  @0x562240; UI_UpdateDeathScreenContent @0x5536a0]
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/effect_pose_index.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/world/angle.h>

#include <cstdio>
#include <map>
#include <string>

using namespace opennova;
using namespace opennova::inmatch;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

int main() {
	// The bare local role: nothing folded, nothing gathered.
	{
		RoleView bare;
		CHECK(!local_death_screen_active(bare) && !local_player_dead(bare));
		const EndRoundSessionState s = end_round_session_state(bare);
		CHECK(!s.header_known && !s.board_known && !s.session_open);
		CHECK(end_round_rows(bare, 0).empty());
		std::vector<world::FriendlyTagSource> tags;
		CHECK(!collect_friendly_tags(bare, tags) && tags.empty());
		const world::DeployScreenStatus status =
				deploy_screen_status(bare, world::SpawnZoneRegistry(), "M", hud::GameTextLookup());
		CHECK(status.penalty_seconds == 0 && status.line.kind == world::DeployStatusLine::Kind::None &&
				!status.statics.medic && status.respawn_text.empty());
		CHECK(status.statics_text.call_medic == "Press M to call a medic");
		std::vector<hud::HudLfpZone> zones;
		CHECK(!collect_lfp_zones(bare, world::SpawnZoneRegistry(), 1, zones) && zones.empty());
		// The sun-visibility diff without a kernel emits nothing and keeps its
		// caches; a layout change forgets the wire-domain cache.
		SunQualityFeed sun;
		sun.last_by_wire[7] = 2;
		sun.last_by_bms[3] = 1;
		const int32_t step[3] = { 0, 0, 65536 * 200 };
		std::vector<SunQualityChange> changes;
		sun.collect(bare, step, {}, {}, 5, changes);
		CHECK(changes.empty() && sun.last_by_wire.size() == 1 && sun.layout_revision_seen == -1);
	}

	// A joiner's view: the 0x1D header + 0x56 board folded into its replica state.
	ClientRuntime runtime("RoleFeeds");
	replication::ClientState &cs = runtime.state();
	cs.end_round.header_known = true;
	cs.end_round.known = true;
	cs.end_round.header.winner_team = 2;
	cs.end_round.header.team_score_0 = 7;
	cs.end_round.header.team_score_1 = 9;
	cs.end_round.header.draw = 0;
	cs.end_round.header.player_index = 1;
	cs.end_round.header.player_names[0] = "Alice";
	cs.end_round.header.player_scores[0] = 12;
	cs.death_screen_active = true;
	cs.round_time_remaining_ticks = 620;
	RoleView joiner;
	joiner.runtime = &runtime;
	joiner.joiner = true;
	CHECK(local_death_screen_active(joiner));
	const EndRoundSessionState s = end_round_session_state(joiner);
	CHECK(s.header_known && s.board_known && s.winner == 2 && s.team_score_0 == 7 &&
			s.team_score_1 == 9 && !s.draw && s.my_index == 1 && s.round_ticks == 620 &&
			s.death_screen && s.session_open); // a live joiner runtime: not lost
	const hud::EndRoundOverlayInput in = end_round_overlay_input(joiner);
	CHECK(in.winner_team == 2 && in.team_scores[1] == 9 && in.player_names[0] == "Alice" &&
			in.player_scores[0] == 12 && in.death_screen && in.round_time_remaining_ticks == 620);
	// A negative round clock reads as 0 (the untimed arm).
	cs.round_time_remaining_ticks = -1;
	CHECK(end_round_session_state(joiner).round_ticks == 0);

	// The DEATH screen facts: the sub-block-0 timers and the being-revived latch.
	cs.respawn_penalty_seconds = 4;
	cs.local_revive_seconds = 30;
	cs.spawn_hold_seconds = 0;
	cs.local_medic_reviving = false;
	{
		std::map<std::string, std::string> rows = { { "Overlays/STROVER_PENALTYTIMER", "Penalty" },
			{ "Overlays/STROVER_MEDICTIMER", "Medic" } };
		const hud::GameTextLookup gametext = [rows](const char *section, const char *key,
													   const char *fallback) {
			const auto it = rows.find(std::string(section) + "/" + key);
			return it != rows.end() ? it->second : std::string(fallback);
		};
		const world::DeployScreenStatus status =
				deploy_screen_status(joiner, world::SpawnZoneRegistry(), "Q", gametext);
		CHECK(status.penalty_seconds == 4 && status.revive_seconds == 30 &&
				status.line.kind == world::DeployStatusLine::Kind::Penalty);
		CHECK(status.respawn_text == "Penalty  <cFF4040>4");
		CHECK(status.statics.medic && !status.statics.psp_respawn);
		CHECK(status.statics_text.medic_timer == "Medic  <cFF4040>30");
		CHECK(status.statics_text.call_medic == "Press Q to call a medic");
		cs.local_medic_reviving = true; // a medic already reviving hides the pair
		CHECK(!deploy_screen_status(joiner, world::SpawnZoneRegistry(), "Q", gametext).statics.medic);
	}

	// The present-effect pose index over a joiner's decoded rows: a lookup
	// resolves through the wire row (mission units, the yaw off the heading),
	// a miss is remembered for the epoch, and a newly applied replica frame
	// rebuilds the cache.
	{
		mission::MissionKernel kernel;
		RoleView jv;
		jv.kernel = &kernel;
		jv.runtime = &runtime;
		jv.joiner = true;
		replication::ClientEntityState row;
		row.handle = 5;
		row.type_id = 7;
		row.x = 3 * 65536;
		row.y = 4 * 65536;
		row.z = 5 * 65536;
		row.heading_bam = 0x4000;
		cs.entities.push_back(row);
		EffectPoseIndex index;
		const EffectPose *pose = index.for_handle(jv, 5);
		CHECK(pose != nullptr && pose->x == 3.0f && pose->y == 4.0f && pose->z == 5.0f);
		CHECK(pose != nullptr && pose->pitch_deg == 0.0f && pose->roll_deg == 0.0f &&
				pose->yaw_deg == static_cast<float>(world::mission_yaw_deg_from_bam_heading(0x4000)));
		CHECK(index.for_handle(jv, 6) == nullptr);
		// A joiner resolves wire identity only.
		world::BmsHandleIndex bms;
		CHECK(index.for_ssn(jv, 9) == nullptr && index.for_bms_id(jv, 9, bms) == nullptr &&
				index.for_origin(jv, 1, 2) == nullptr);
		// The epoch: the same frame keeps the miss, a new applied frame retries.
		cs.entities.push_back(replication::ClientEntityState());
		cs.entities.back().handle = 6;
		cs.entities.back().type_id = 7;
		CHECK(index.for_handle(jv, 6) == nullptr);
		cs.frames_applied += 1;
		CHECK(index.for_handle(jv, 6) != nullptr);
		// No runtime: nothing resolves and the cache empties.
		RoleView cold;
		cold.kernel = &kernel;
		CHECK(index.for_handle(cold, 5) == nullptr);
	}

	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("role_feeds_test OK\n");
	return 0;
}
